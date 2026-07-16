#include "Runtime/URSZmqRobotBridgeComponent.h"

#include "MuJoCo/Components/Actuators/MjActuator.h"
#include "MuJoCo/Core/AMjManager.h"
#include "MuJoCo/Core/MjArticulation.h"
#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#endif
#include "zmq.h"
#if PLATFORM_WINDOWS
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
void ConfigureNonBlockingSocket(void* Socket)
{
	int LingerMs = 0;
	zmq_setsockopt(Socket, ZMQ_LINGER, &LingerMs, sizeof(LingerMs));
	int Conflate = 1;
	zmq_setsockopt(Socket, ZMQ_CONFLATE, &Conflate, sizeof(Conflate));
}
} // namespace

UURSZmqRobotBridgeComponent::UURSZmqRobotBridgeComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	RobotNames = URSoccerLab::FRobotProtocol::MakeDefaultRobotNames();
}

void UURSZmqRobotBridgeComponent::BeginPlay()
{
	Super::BeginPlay();
	if (bAutoStart)
	{
		StartBridge();
	}
}

void UURSZmqRobotBridgeComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopBridge();
	Super::EndPlay(EndPlayReason);
}

void UURSZmqRobotBridgeComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!bBridgeStarted)
	{
		return;
	}

	DrainCommandSockets();
	const double NowSec = GetWorld() ? GetWorld()->GetTimeSeconds() : FPlatformTime::Seconds();
	ApplyLatestCommands(NowSec);
}

URSoccerLab::FRobotRuntimeConfig UURSZmqRobotBridgeComponent::MakeRuntimeConfig() const
{
	URSoccerLab::FRobotRuntimeConfig Config;
	Config.CommandBasePort = CommandBasePort;
	Config.StatePort = StatePort;
	Config.MetaPort = MetaPort;
	Config.CommandTimeoutSec = CommandTimeoutSec;
	Config.RobotNames = RobotNames.Num() > 0 ? RobotNames : URSoccerLab::FRobotProtocol::MakeDefaultRobotNames();
	return Config;
}

bool UURSZmqRobotBridgeComponent::StartBridge()
{
	if (bBridgeStarted)
	{
		return true;
	}

	if (!RebuildEndpointCache())
	{
		return false;
	}

	ZmqContext = zmq_ctx_new();
	if (!ZmqContext)
	{
		UE_LOG(LogTemp, Error, TEXT("URSoccerLab ZMQ bridge: failed to create ZMQ context."));
		return false;
	}

	if (!BindCommandSockets())
	{
		StopBridge();
		return false;
	}

	bBridgeStarted = true;
	UE_LOG(LogTemp, Log, TEXT("URSoccerLab ZMQ bridge started with %d robot command endpoints."), RuntimeEndpoints.Num());
	return true;
}

void UURSZmqRobotBridgeComponent::StopBridge()
{
	CloseCommandSockets();
	if (ZmqContext)
	{
		zmq_ctx_term(ZmqContext);
		ZmqContext = nullptr;
	}
	bBridgeStarted = false;
}

bool UURSZmqRobotBridgeComponent::RebuildEndpointCache()
{
	CloseCommandSockets();
	RuntimeEndpoints.Reset();
	EndpointInfo.Reset();

	Manager = AAMjManager::GetManager();
	AAMjManager* ManagerPtr = Manager.Get();
	if (!ManagerPtr)
	{
		UE_LOG(LogTemp, Warning, TEXT("URSoccerLab ZMQ bridge: no AAMjManager in world yet."));
		return false;
	}

	URSoccerLab::FRobotRuntimeConfig Config = MakeRuntimeConfig();
	TArray<URSoccerLab::FRobotPortAssignment> Assignments;
	if (!URSoccerLab::FRobotProtocol::BuildPortAssignments(Config, Assignments))
	{
		UE_LOG(LogTemp, Error, TEXT("URSoccerLab ZMQ bridge: invalid command port range starting at %d for %d robots."),
			Config.CommandBasePort, Config.RobotNames.Num());
		return false;
	}

	TMap<FString, AMjArticulation*> ArticulationsByName;
	for (AMjArticulation* Articulation : ManagerPtr->GetAllArticulations())
	{
		if (Articulation)
		{
			ArticulationsByName.Add(Articulation->GetName(), Articulation);
			if (!Articulation->ActorId.IsEmpty())
			{
				ArticulationsByName.Add(Articulation->ActorId, Articulation);
			}
		}
	}

	for (const URSoccerLab::FRobotPortAssignment& Assignment : Assignments)
	{
		AMjArticulation* const* ArticulationPtr = ArticulationsByName.Find(Assignment.RobotName);
		if (!ArticulationPtr || !*ArticulationPtr)
		{
			UE_LOG(LogTemp, Verbose, TEXT("URSoccerLab ZMQ bridge: robot '%s' not present; skipping endpoint."), *Assignment.RobotName);
			continue;
		}

		TArray<UMjActuator*> Actuators = (*ArticulationPtr)->GetActuators();
		Actuators.RemoveAll([](const UMjActuator* Actuator) {
			return !Actuator || Actuator->GetMjID() < 0;
		});
		Actuators.Sort([](const UMjActuator& Left, const UMjActuator& Right) {
			return Left.GetMjID() < Right.GetMjID();
		});

		FRobotRuntimeEndpoint Endpoint;
		Endpoint.RobotName = Assignment.RobotName;
		Endpoint.CommandEndpoint = URSoccerLab::FRobotProtocol::BuildTcpBindEndpoint(Assignment.CommandPort);
		Endpoint.StateTopic = FString::Printf(TEXT("state/%s"), *Assignment.RobotName);
		Endpoint.Articulation = *ArticulationPtr;
		Endpoint.CommandBuffer.Configure(Actuators.Num(), Config.CommandTimeoutSec);

		FURSRobotEndpointInfo Info;
		Info.RobotName = Endpoint.RobotName;
		Info.CommandEndpoint = Endpoint.CommandEndpoint;
		Info.StateTopic = Endpoint.StateTopic;

		for (UMjActuator* Actuator : Actuators)
		{
			Endpoint.Actuators.Add(Actuator);
			Endpoint.ActuatorNames.Add(Actuator->GetMjName());
			Endpoint.ActuatorIds.Add(Actuator->GetMjID());
			Info.ActuatorNames.Add(Actuator->GetMjName());
			Info.ActuatorIds.Add(Actuator->GetMjID());
		}

		RuntimeEndpoints.Add(MoveTemp(Endpoint));
		EndpointInfo.Add(MoveTemp(Info));
	}

	return RuntimeEndpoints.Num() > 0;
}

bool UURSZmqRobotBridgeComponent::BindCommandSockets()
{
	if (!ZmqContext)
	{
		return false;
	}

	for (FRobotRuntimeEndpoint& Endpoint : RuntimeEndpoints)
	{
		Endpoint.CommandSocket = zmq_socket(ZmqContext, ZMQ_PULL);
		if (!Endpoint.CommandSocket)
		{
			UE_LOG(LogTemp, Error, TEXT("URSoccerLab ZMQ bridge: failed to create command socket for %s."), *Endpoint.RobotName);
			return false;
		}

		ConfigureNonBlockingSocket(Endpoint.CommandSocket);
		const FTCHARToUTF8 EndpointUtf8(*Endpoint.CommandEndpoint);
		const int Rc = zmq_bind(Endpoint.CommandSocket, EndpointUtf8.Get());
		if (Rc != 0)
		{
			UE_LOG(LogTemp, Error, TEXT("URSoccerLab ZMQ bridge: failed to bind %s for %s."),
				*Endpoint.CommandEndpoint, *Endpoint.RobotName);
			return false;
		}
	}
	return true;
}

void UURSZmqRobotBridgeComponent::CloseCommandSockets()
{
	for (FRobotRuntimeEndpoint& Endpoint : RuntimeEndpoints)
	{
		if (Endpoint.CommandSocket)
		{
			zmq_close(Endpoint.CommandSocket);
			Endpoint.CommandSocket = nullptr;
		}
	}
}

int32 UURSZmqRobotBridgeComponent::DrainCommandSockets()
{
	int32 AcceptedCount = 0;
	const double NowSec = GetWorld() ? GetWorld()->GetTimeSeconds() : FPlatformTime::Seconds();

	for (FRobotRuntimeEndpoint& Endpoint : RuntimeEndpoints)
	{
		if (!Endpoint.CommandSocket)
		{
			continue;
		}

		while (true)
		{
			zmq_msg_t Message;
			zmq_msg_init(&Message);
			const int Rc = zmq_msg_recv(&Message, Endpoint.CommandSocket, ZMQ_DONTWAIT);
			if (Rc < 0)
			{
				zmq_msg_close(&Message);
				break;
			}

			const uint8* Data = static_cast<const uint8*>(zmq_msg_data(&Message));
			const int32 Size = static_cast<int32>(zmq_msg_size(&Message));
			const TArrayView<const uint8> Payload(Data, Size);
			URSoccerLab::FMotorCommandParseResult Parsed =
				URSoccerLab::FRobotProtocol::DecodeMotorCommand(Payload, Endpoint.Actuators.Num());
			if (Parsed.IsAccepted())
			{
				const URSoccerLab::EMotorCommandValidation Status = Endpoint.CommandBuffer.TryAccept(Parsed.Command, NowSec);
				if (Status == URSoccerLab::EMotorCommandValidation::Accepted)
				{
					++AcceptedCount;
				}
				else
				{
					UE_LOG(LogTemp, Verbose, TEXT("URSoccerLab ZMQ bridge: rejected command for %s: %s."),
						*Endpoint.RobotName, URSoccerLab::FRobotProtocol::LexToString(Status));
				}
			}
			else
			{
				UE_LOG(LogTemp, Verbose, TEXT("URSoccerLab ZMQ bridge: rejected payload for %s: %s."),
					*Endpoint.RobotName, URSoccerLab::FRobotProtocol::LexToString(Parsed.Status));
			}

			zmq_msg_close(&Message);
		}
	}

	return AcceptedCount;
}

void UURSZmqRobotBridgeComponent::ApplyLatestCommands(double NowSec)
{
	for (FRobotRuntimeEndpoint& Endpoint : RuntimeEndpoints)
	{
		const TArray<float> Command = Endpoint.CommandBuffer.GetCommandOrZero(NowSec);
		const int32 Count = FMath::Min(Command.Num(), Endpoint.Actuators.Num());
		for (int32 Idx = 0; Idx < Count; ++Idx)
		{
			if (UMjActuator* Actuator = Endpoint.Actuators[Idx].Get())
			{
				Actuator->SetNetworkControl(Command[Idx]);
			}
		}
	}
}
