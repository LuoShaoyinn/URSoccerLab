#include "Inspector/URSInspectorCameraComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
#include "Async/Async.h"
#include "IImageWrapperModule.h"
#include "MuJoCo/Utils/MjUtils.h"
#include "Core/URSRobotCoreComponent.h"
#include "Misc/CommandLine.h"
#include "Misc/ScopeLock.h"
struct FURSInspectorReadback
{
 TUniquePtr<FRHIGPUTextureReadback> GPU = MakeUnique<FRHIGPUTextureReadback>(TEXT("Inspector"));
 bool InFlight = false; // render-thread only
 FCriticalSection Mutex;
 TArray<FColor> Pixels;
 bool Ready = false; // mutex-protected
};
UURSInspectorCameraComponent::UURSInspectorCameraComponent() { PrimaryComponentTick.bCanEverTick = true; }
void UURSInspectorCameraComponent::BeginPlay()
{
 Super::BeginPlay();
 Mailbox = MakeShared<FMailbox, ESPMode::ThreadSafe>();
 EncoderModule = &FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
 FString Codec;
 if (FParse::Value(FCommandLine::Get(), TEXT("URSInspectorCodec="), Codec)) Jpeg = Codec != TEXT("raw");
}
bool UURSInspectorCameraComponent::SetPose(uint64 Id, const URSoccerLab::FInspectorPose& Pose)
{
 if (!Sessions.Contains(Id))
 {
  if (Sessions.Num() >= 4) return false;
  auto* Target = NewObject<UTextureRenderTarget2D>(this);
  Target->InitCustomFormat(640, 480, PF_B8G8R8A8, false);
  Target->ClearColor = FLinearColor::Black;
  Target->UpdateResourceImmediate(true);
  auto* Capture = NewObject<USceneCaptureComponent2D>(GetOwner());
  Capture->TextureTarget = Target;
  Capture->bCaptureEveryFrame = false; Capture->bCaptureOnMovement = false;
  Capture->bAlwaysPersistRenderingState = true;
  Capture->bUseRayTracingIfEnabled = true;
  Capture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
  Capture->FOVAngle = 90;
  Capture->RegisterComponent();
  Captures.Add(Id, Capture); Targets.Add(Id, Target);
  FSession Session; Session.Readback = MakeShared<FURSInspectorReadback, ESPMode::ThreadSafe>();
  Sessions.Add(Id, MoveTemp(Session));
 }
 const double Pos[3] = {Pose.Position.X, Pose.Position.Y, Pose.Position.Z};
 const double Quat[4] = {Pose.Rotation.W, Pose.Rotation.X, Pose.Rotation.Y, Pose.Rotation.Z};
 Captures[Id]->SetWorldLocationAndRotation(MjUtils::MjToUEPosition(Pos), MjUtils::MjToUERotation(Quat));
 return true;
}
void UURSInspectorCameraComponent::Remove(uint64 Id)
{
 if (auto* Capture = Captures.Find(Id)) (*Capture)->DestroyComponent();
 Captures.Remove(Id); Targets.Remove(Id); Sessions.Remove(Id);
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
 FlushRenderingCommands();
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
 const double Now = FPlatformTime::Seconds();
 for (auto& Pair : Sessions)
 {
  auto& Session = Pair.Value; const uint64 Id = Pair.Key; auto Slot = Session.Readback;
  if (Session.Busy)
  {
   // Include encodes belonging to recently disconnected sessions in the global budget.
   if (Jobs.Num() >= 4) continue;
   URSoccerLab::FRawCameraImage Image; bool Ready;
   { FScopeLock Lock(&Slot->Mutex); Ready = Slot->Ready;
     if (Ready) { Image.Pixels = MoveTemp(Slot->Pixels); Slot->Ready = false; } }
   if (Ready)
   {
    Image.Name = TEXT("inspector"); Image.Width = 640; Image.Height = 480;
    URSoccerLab::FEncodedCameraFrame Frame; Frame.Sequence = Session.Sequence++;
    if (auto* Core = GetOwner()->FindComponentByClass<UURSRobotCoreComponent>())
    {
     const auto Ids = Core->GetRobotIds(); FURSRobotState State;
     if (!Ids.IsEmpty() && Core->GetRobotState(Ids[0], State)) Frame.SimTime = State.SimTime;
    }
    const auto Outbox = Mailbox; auto* Module = EncoderModule; const bool Compress = Jpeg; const uint32 Gen = Generation;
    Jobs.Add(Async(EAsyncExecution::ThreadPool, [Id, Gen, Outbox, Module, Compress, Image = MoveTemp(Image), Frame = MoveTemp(Frame)]() mutable {
     URSoccerLab::FEncodedCameraImage Encoded;
     if (URSoccerLab::FImageEncoder::Encode(Image, Compress, 80, *Module, Encoded)) Frame.Images.Add(MoveTemp(Encoded));
     Outbox->Frames.Enqueue({Gen, Id, MoveTemp(Frame)});
    }));
   }
   else
   {
    ENQUEUE_RENDER_COMMAND(PollInspector)([Slot](FRHICommandListImmediate&) {
     if (!Slot->InFlight || !Slot->GPU->IsReady()) return;
     int32 Pitch = 0, Height = 0;
     const auto* Pixels = static_cast<const FColor*>(Slot->GPU->Lock(Pitch, &Height));
     TArray<FColor> Copy;
     if (Pixels && Pitch >= 640 && Height >= 480)
     {
      Copy.SetNumUninitialized(640*480);
      for (int Row = 0; Row < 480; ++Row) FMemory::Memcpy(Copy.GetData()+Row*640, Pixels+Row*Pitch, 640*sizeof(FColor));
     }
     Slot->GPU->Unlock(); Slot->InFlight = false;
     FScopeLock Lock(&Slot->Mutex); Slot->Pixels = MoveTemp(Copy); Slot->Ready = true;
    });
   }
  }
  else if (OnFrame.IsBound() && Now >= Session.NextCapture)
  {
   Session.NextCapture = Now + 1.0/15; Session.Busy = true;
   Captures[Id]->CaptureScene();
   auto* Resource = Targets[Id]->GameThread_GetRenderTargetResource();
   // Resource remains alive through the command: capture/target ownership is
   // released on GT, and UE queues render-resource destruction behind this copy.
   ENQUEUE_RENDER_COMMAND(CopyInspector)([Slot, Resource](FRHICommandListImmediate& RHICmdList) {
    Slot->GPU->EnqueueCopy(RHICmdList, Resource->GetRenderTargetTexture()); Slot->InFlight = true;
   });
  }
 }
}
