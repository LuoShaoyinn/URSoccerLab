#include "Inspector/URSInspectorCameraComponent.h"
#include "NDisplay/URSDisplayClusterCameraBinderComponent.h"
#include "Async/Async.h"
#include "IImageWrapperModule.h"
#include "MuJoCo/Utils/MjUtils.h"
#include "Core/URSRobotCoreComponent.h"
#include "Scene/URSSceneConfigComponent.h"
#include "Misc/CommandLine.h"
UURSInspectorCameraComponent::UURSInspectorCameraComponent() { PrimaryComponentTick.bCanEverTick = true; }
void UURSInspectorCameraComponent::BeginPlay()
{
 Super::BeginPlay();
 Mailbox = MakeShared<FMailbox, ESPMode::ThreadSafe>();
 Binder = GetOwner()->FindComponentByClass<UURSDisplayClusterCameraBinderComponent>();
 EncoderModule = &FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
 if (const auto* Config = GetOwner()->FindComponentByClass<UURSSceneConfigComponent>()) Settings = Config->GetActiveConfig().GuestInspector;

}
bool UURSInspectorCameraComponent::SetPose(uint64 Id, const URSoccerLab::FInspectorPose& Pose)
{
 const double Pos[3] = {Pose.Position.X, Pose.Position.Y, Pose.Position.Z};
 const double Quat[4] = {Pose.Rotation.W, Pose.Rotation.X, Pose.Rotation.Y, Pose.Rotation.Z};
 if (!Settings.bEnabled || !Binder || !Binder->SetGuestPose(Id, MjUtils::MjToUEPosition(Pos), MjUtils::MjToUERotation(Quat)))
  return false;
 if (!Sessions.Contains(Id))
 {
  FSession Session;
  if (Settings.Rgb.Compression != URSoccerLab::EURSRgbCompression::Raw && Settings.Rgb.Compression != URSoccerLab::EURSRgbCompression::Jpeg)
   Session.VideoEncoder = MakeShared<URSoccerLab::FVideoEncoder, ESPMode::ThreadSafe>(Settings.Rgb);
  // Ignore an atlas already completed before this slot was assigned.
  Session.LastAtlasSequence = Binder->GetLatestRgbFrameSequence();
  Sessions.Add(Id, MoveTemp(Session));
 }
 return true;
}
void UURSInspectorCameraComponent::Remove(uint64 Id)
{
 if (Binder) Binder->RemoveGuest(Id);
 Sessions.Remove(Id);
}

void UURSInspectorCameraComponent::Reset()
{
 ++Generation;
 TArray<uint64> Ids; Sessions.GetKeys(Ids);
 for (uint64 Id : Ids) Remove(Id);
}
void UURSInspectorCameraComponent::EndPlay(const EEndPlayReason::Type Reason)
{
 OnFrame.Clear(); Reset();
 for (auto& Job : Jobs) Job.Wait();
 Jobs.Empty(); Mailbox.Reset();
 Super::EndPlay(Reason);
}
void UURSInspectorCameraComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* Function)
{
 Super::TickComponent(DeltaTime, TickType, Function);
 Jobs.RemoveAll([](const TFuture<void>& Job) { return Job.IsReady(); });
 FCompleted Complete;
 while (Mailbox->Frames.Dequeue(Complete))
 {
  if (Complete.Generation != Generation) continue;
  if (auto* Session = Sessions.Find(Complete.Session))
  {
   Session->Busy = false;
   if (!Complete.Frame.Images.IsEmpty()) OnFrame.Broadcast(Complete.Session, Complete.Frame);
  }
 }
 if (!Binder || !Binder->IsReady() || !OnFrame.IsBound()) return;
 const double Now = FPlatformTime::Seconds();
 for (auto& Pair : Sessions)
 {
  auto& Session = Pair.Value;
  if (Session.Busy || Now < Session.NextCapture || Jobs.Num() >= 4) continue;
  Binder->RequestRgbFrame();
  URSoccerLab::FRawCameraImage Image;
  uint64 AtlasSequence = 0;
  if (!Binder->CopyGuestFrame(Pair.Key, Session.LastAtlasSequence, Image.Pixels,
                             Image.Width, Image.Height, AtlasSequence)) continue;
  Session.LastAtlasSequence = AtlasSequence;
  const double Interval = 1.0 / Settings.Rgb.RateHz;
  if (Session.NextCapture == 0) Session.NextCapture = Now;
  do { Session.NextCapture += Interval; } while (Session.NextCapture <= Now);
  Session.Busy = true;
  Image.Name = TEXT("inspector");
  URSoccerLab::FEncodedCameraFrame Frame;
  Frame.Sequence = Session.Sequence++;
  if (auto* Core = GetOwner()->FindComponentByClass<UURSRobotCoreComponent>())
  {
   const auto Ids = Core->GetRobotIds(); FURSRobotState State;
   if (!Ids.IsEmpty() && Core->GetRobotState(Ids[0], State)) Frame.SimTime = State.SimTime;
  }
  const auto Outbox = Mailbox;
  auto* Module = EncoderModule;
  const bool Compress = Settings.Rgb.Compression == URSoccerLab::EURSRgbCompression::Jpeg;
  const int Quality = Settings.Rgb.JpegQuality;
  const auto VideoEncoder = Session.VideoEncoder;
  const uint32 Gen = Generation;
  const uint64 Id = Pair.Key;
  Jobs.Add(Async(EAsyncExecution::ThreadPool,
   [Id, Gen, Outbox, Module, Compress, Quality, VideoEncoder, Image = MoveTemp(Image), Frame = MoveTemp(Frame)]() mutable {
    if (VideoEncoder)
    {
     TArray<URSoccerLab::FRawCameraImage> Images; Images.Add(MoveTemp(Image));
     if (!VideoEncoder->Encode(Images, Frame)) Frame.Images.Empty();
    }
    else
    {
     URSoccerLab::FEncodedCameraImage Encoded;
     if (URSoccerLab::FImageEncoder::Encode(Image, Compress, Quality, *Module, Encoded)) Frame.Images.Add(MoveTemp(Encoded));
    }
    Outbox->Frames.Enqueue({Gen, Id, MoveTemp(Frame)});
   }));
 }
}
