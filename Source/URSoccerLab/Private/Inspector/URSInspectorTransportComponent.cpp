#include "Inspector/URSInspectorTransportComponent.h"
#include "Inspector/URSInspectorCameraComponent.h"
#include "Core/URSRobotCoreComponent.h"
#include "Scene/URSSceneConfigComponent.h"
#include "Transport/URSTcpTransportComponent.h"
#include "Misc/CommandLine.h"
UURSInspectorTransportComponent::UURSInspectorTransportComponent() { PrimaryComponentTick.bCanEverTick = true; }
void UURSInspectorTransportComponent::BeginPlay()
{
 Super::BeginPlay();
 Camera = GetOwner()->FindComponentByClass<UURSInspectorCameraComponent>();
 Core = GetOwner()->FindComponentByClass<UURSRobotCoreComponent>();
 if (const auto* Config = GetOwner()->FindComponentByClass<UURSSceneConfigComponent>())
 { const auto& Settings = Config->GetActiveConfig().GuestInspector; Port = Settings.Port; Capacity = Settings.MaxGuests; Enabled = Settings.bEnabled; }
 FParse::Value(FCommandLine::Get(), TEXT("URSInspectorPort="), Port);
 if (Camera.IsValid()) FrameHandle = Camera->OnFrame.AddUObject(this, &UURSInspectorTransportComponent::Send);
 if (Core.IsValid()) Core->OnRobotsChanged.AddDynamic(this, &UURSInspectorTransportComponent::OnRobotsChanged);
}
void UURSInspectorTransportComponent::OnRobotsChanged()
{
 if (Network) { Network->Stop(); Network.Reset(); }
 if (Camera.IsValid()) Camera->Reset();
}
void UURSInspectorTransportComponent::EndPlay(const EEndPlayReason::Type Reason)
{
 if (Camera.IsValid()) Camera->OnFrame.Remove(FrameHandle);
 if (Core.IsValid()) Core->OnRobotsChanged.RemoveDynamic(this, &UURSInspectorTransportComponent::OnRobotsChanged);
 OnRobotsChanged(); Super::EndPlay(Reason);
}
void UURSInspectorTransportComponent::Send(uint64 Id, const URSoccerLab::FEncodedCameraFrame& Frame)
{ if (Network) Network->Send(Id, Frame); }
void UURSInspectorTransportComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* Function)
{
 Super::TickComponent(DeltaTime, TickType, Function);
 if (!Enabled || !Camera.IsValid() || !Core.IsValid() || Failed) return;
 if (!Network)
 {
  const auto Ids = Core->GetRobotIds(); if (Ids.IsEmpty()) return;
  auto* RobotTransport = GetOwner()->FindComponentByClass<UURSTcpTransportComponent>();
  const int32 Base = RobotTransport ? RobotTransport->RobotBasePort : 10000;
  const int32 Admin = RobotTransport ? RobotTransport->AdminPort : 11000;
  if (Port < 1 || Port > 65535 || Port == Admin || (Port >= Base && Port < Base+Ids.Num()))
  { Failed = true; UE_LOG(LogTemp, Error, TEXT("[URS Inspector] invalid or overlapping port %d"), Port); return; }
  Network = CreateURSInspectorTcp();
  if (!Network->Start(Port, Capacity)) { Failed = true; Network.Reset(); UE_LOG(LogTemp, Error, TEXT("[URS Inspector] listener failed")); return; }
 }
 URSoccerLab::FInspectorEvent Event;
 for (int N = 0; N < 32 && Network->Dequeue(Event); ++N)
 {
  if (Event.Kind == URSoccerLab::FInspectorEvent::EKind::Disconnected) Camera->Remove(Event.Session);
  else if (Event.Kind == URSoccerLab::FInspectorEvent::EKind::Pose)
  {
   const bool Ok = Camera->SetPose(Event.Session, Event.Pose);
   Network->Reply(Event.Session, URSoccerLab::FInspectorProtocol::Reply(Ok, TEXT("nDisplay guest camera unavailable or capacity exhausted")));
  }
 }
}
