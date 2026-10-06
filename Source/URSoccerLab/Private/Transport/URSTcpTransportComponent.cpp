#include "Transport/URSTcpTransportComponent.h"
#include "Scene/URSSceneConfigComponent.h"
#include "Core/URSRobotCoreComponent.h"
#include "Vision/URSCameraStreamComponent.h"
#include "Network/URSNetworkThread.h"
#include "Runtime/URSAdminService.h"
#include "Protocol/URSMessageProtocol.h"
UURSTcpTransportComponent::UURSTcpTransportComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}
void UURSTcpTransportComponent::BeginPlay()
{
	Super::BeginPlay();
	Core = GetOwner()->FindComponentByClass<UURSRobotCoreComponent>();
	CameraStream = GetOwner()->FindComponentByClass<UURSCameraStreamComponent>();
	if (Core.IsValid())
		Core->OnRobotsChanged.AddDynamic(this, &UURSTcpTransportComponent::OnRobotsChanged);
	if (CameraStream.IsValid())
		FrameHandle = CameraStream->OnEncodedFrame.AddUObject(this, &UURSTcpTransportComponent::SendCameraFrame);
	if (const auto* Config = GetOwner()->FindComponentByClass<UURSSceneConfigComponent>())
		if (Config->GetActiveConfig().StateFreq > 0)
			StateRateHz = Config->GetActiveConfig().StateFreq;
	if (bAutoStart)
		StartTransport();
}
void UURSTcpTransportComponent::EndPlay(const EEndPlayReason::Type Reason)
{
	if (CameraStream.IsValid())
		CameraStream->OnEncodedFrame.Remove(FrameHandle);
	if (Core.IsValid())
		Core->OnRobotsChanged.RemoveDynamic(this, &UURSTcpTransportComponent::OnRobotsChanged);
	StopTransport();
	Super::EndPlay(Reason);
}
bool UURSTcpTransportComponent::StartTransport()
{
	bStarted = true;
	RebuildNetworkThread();
	return NetThread != nullptr;
}
void UURSTcpTransportComponent::StopTransport()
{
	bStarted = false;
	if (NetThread)
	{
		NetThread->Stop();
		delete NetThread;
		NetThread = nullptr;
	}
}
void UURSTcpTransportComponent::RebuildNetworkThread()
{
	if (!bStarted || !Core.IsValid())
		return;
	auto Channels = Core->GetRobotChannels();
	if (Channels.IsEmpty() || (CameraStream.IsValid() && !CameraStream->IsLayoutReady()))
	{
		if (NetThread)
		{
			NetThread->Stop();
			delete NetThread;
			NetThread = nullptr;
		}
		return;
	}
	if (!URSoccerLab::TcpProtocol::IsValidPortLayout(RobotBasePort, Channels.Num(), AdminPort))
	{
		UE_LOG(LogTemp, Error, TEXT("[URS TCP] Invalid port layout"));
		return;
	}
	if (NetThread)
	{
		NetThread->Stop();
		delete NetThread;
		NetThread = nullptr;
	}
	NetThread = new URSNetworkThread();
	FURSNetworkConfig Config;
	Config.RobotBasePort = RobotBasePort;
	Config.AdminPort = AdminPort;
	Config.StateRateHz = StateRateHz;
	if (!NetThread->Start(MoveTemp(Channels), Config))
	{
		delete NetThread;
		NetThread = nullptr;
		bStarted = false;
	}
}
void UURSTcpTransportComponent::OnRobotsChanged()
{
	RebuildNetworkThread();
}
void UURSTcpTransportComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                              FActorComponentTickFunction* TickFunction)
{
	Super::TickComponent(DeltaTime, TickType, TickFunction);
	if (bStarted && !NetThread)
		RebuildNetworkThread();
	if (!NetThread)
		return;
	if (CameraStream.IsValid())
		CameraStream->SetCaptureDemand(NetThread->HasCameraSubscribers());
	URSoccerLab::FAdminMessage Request;
	// Limit work per tick so an admin client cannot monopolize the game thread.
	for (int32 Count = 0; Count < 64 && NetThread->DequeueAdminRequest(Request); ++Count)
		NetThread->EnqueueAdminReply(Request.ClientId, URSoccerLab::FAdminService::Execute(Core.Get(), Request.Json));
}
void UURSTcpTransportComponent::SendCameraFrame(const URSoccerLab::FEncodedCameraFrame& Frame)
{
	if (NetThread)
		NetThread->EnqueueCameraFrame(Frame);
}
