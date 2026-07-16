#include "Runtime/URSZmqRobotBridgeComponent.h"

#include "MuJoCo/Components/Actuators/MjActuator.h"
#include "MuJoCo/Components/Joints/MjJoint.h"
#include "MuJoCo/Core/AMjManager.h"
#include "MuJoCo/Core/MjArticulation.h"
#include "MuJoCo/Core/MjPhysicsEngine.h"
#include "Dom/JsonObject.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
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

	const double StateIntervalSec = StatePublishRateHz > 0.0 ? 1.0 / StatePublishRateHz : 0.0;
	if (StateIntervalSec <= 0.0 || NowSec - LastStatePublishSec >= StateIntervalSec)
	{
		PublishState();
		LastStatePublishSec = NowSec;
	}
	if (NowSec - LastMetaPublishSec >= MetaPublishIntervalSec)
	{
		PublishMetadata();
		LastMetaPublishSec = NowSec;
	}
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
	if (!BindPublisherSockets())
	{
		StopBridge();
		return false;
	}

	bBridgeStarted = true;
	PublishMetadata();
	LastMetaPublishSec = GetWorld() ? GetWorld()->GetTimeSeconds() : FPlatformTime::Seconds();
	LastStatePublishSec = 0.0;
	UE_LOG(LogTemp, Log, TEXT("URSoccerLab ZMQ bridge started with %d robot command endpoints."), RuntimeEndpoints.Num());
	return true;
}

void UURSZmqRobotBridgeComponent::StopBridge()
{
	CloseCommandSockets();
	ClosePublisherSockets();
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
		TArray<UMjJoint*> Joints = (*ArticulationPtr)->GetJoints();
		Joints.RemoveAll([](const UMjJoint* Joint) {
			return !Joint || Joint->GetMjID() < 0;
		});
		Joints.Sort([](const UMjJoint& Left, const UMjJoint& Right) {
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
		for (UMjJoint* Joint : Joints)
		{
			Endpoint.Joints.Add(Joint);
			Endpoint.JointNames.Add(Joint->GetMjName());
			Endpoint.JointIds.Add(Joint->GetMjID());
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

bool UURSZmqRobotBridgeComponent::BindPublisherSockets()
{
	if (!ZmqContext)
	{
		return false;
	}

	StatePublisher = zmq_socket(ZmqContext, ZMQ_PUB);
	MetaPublisher = zmq_socket(ZmqContext, ZMQ_PUB);
	if (!StatePublisher || !MetaPublisher)
	{
		UE_LOG(LogTemp, Error, TEXT("URSoccerLab ZMQ bridge: failed to create publisher sockets."));
		return false;
	}

	ConfigureNonBlockingSocket(StatePublisher);
	ConfigureNonBlockingSocket(MetaPublisher);

	const FString StateEndpoint = URSoccerLab::FRobotProtocol::BuildTcpBindEndpoint(StatePort);
	const FString MetaEndpoint = URSoccerLab::FRobotProtocol::BuildTcpBindEndpoint(MetaPort);
	const FTCHARToUTF8 StateUtf8(*StateEndpoint);
	const FTCHARToUTF8 MetaUtf8(*MetaEndpoint);
	if (zmq_bind(StatePublisher, StateUtf8.Get()) != 0)
	{
		UE_LOG(LogTemp, Error, TEXT("URSoccerLab ZMQ bridge: failed to bind state publisher at %s."), *StateEndpoint);
		return false;
	}
	if (zmq_bind(MetaPublisher, MetaUtf8.Get()) != 0)
	{
		UE_LOG(LogTemp, Error, TEXT("URSoccerLab ZMQ bridge: failed to bind metadata publisher at %s."), *MetaEndpoint);
		return false;
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

void UURSZmqRobotBridgeComponent::ClosePublisherSockets()
{
	if (StatePublisher)
	{
		zmq_close(StatePublisher);
		StatePublisher = nullptr;
	}
	if (MetaPublisher)
	{
		zmq_close(MetaPublisher);
		MetaPublisher = nullptr;
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

bool UURSZmqRobotBridgeComponent::SendJsonMessage(void* Socket, const FString& Topic, const FString& Json) const
{
	if (!Socket)
	{
		return false;
	}

	const FTCHARToUTF8 TopicUtf8(*Topic);
	const FTCHARToUTF8 JsonUtf8(*Json);
	if (zmq_send(Socket, TopicUtf8.Get(), TopicUtf8.Length(), ZMQ_SNDMORE) < 0)
	{
		return false;
	}
	return zmq_send(Socket, JsonUtf8.Get(), JsonUtf8.Length(), ZMQ_DONTWAIT) >= 0;
}

void UURSZmqRobotBridgeComponent::PublishMetadata()
{
	if (!MetaPublisher)
	{
		return;
	}

	for (const FRobotRuntimeEndpoint& Endpoint : RuntimeEndpoints)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("version"), TEXT("urs_meta_v1"));
		Root->SetStringField(TEXT("robot"), Endpoint.RobotName);
		Root->SetStringField(TEXT("command_endpoint"), Endpoint.CommandEndpoint);
		Root->SetStringField(TEXT("state_topic"), Endpoint.StateTopic);
		Root->SetStringField(TEXT("state_endpoint"), URSoccerLab::FRobotProtocol::BuildTcpBindEndpoint(StatePort));
		Root->SetStringField(TEXT("meta_endpoint"), URSoccerLab::FRobotProtocol::BuildTcpBindEndpoint(MetaPort));
		Root->SetNumberField(TEXT("command_timeout_sec"), CommandTimeoutSec);

		TArray<TSharedPtr<FJsonValue>> ActuatorNamesJson;
		TArray<TSharedPtr<FJsonValue>> ActuatorIdsJson;
		for (int32 Idx = 0; Idx < Endpoint.ActuatorNames.Num(); ++Idx)
		{
			ActuatorNamesJson.Add(MakeShared<FJsonValueString>(Endpoint.ActuatorNames[Idx]));
			ActuatorIdsJson.Add(MakeShared<FJsonValueNumber>(Endpoint.ActuatorIds.IsValidIndex(Idx) ? Endpoint.ActuatorIds[Idx] : -1));
		}
		Root->SetArrayField(TEXT("actuator_names"), ActuatorNamesJson);
		Root->SetArrayField(TEXT("actuator_ids"), ActuatorIdsJson);

		TArray<TSharedPtr<FJsonValue>> JointNamesJson;
		TArray<TSharedPtr<FJsonValue>> JointIdsJson;
		for (int32 Idx = 0; Idx < Endpoint.JointNames.Num(); ++Idx)
		{
			JointNamesJson.Add(MakeShared<FJsonValueString>(Endpoint.JointNames[Idx]));
			JointIdsJson.Add(MakeShared<FJsonValueNumber>(Endpoint.JointIds.IsValidIndex(Idx) ? Endpoint.JointIds[Idx] : -1));
		}
		Root->SetArrayField(TEXT("joint_names"), JointNamesJson);
		Root->SetArrayField(TEXT("joint_ids"), JointIdsJson);

		FString Json;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
		SendJsonMessage(MetaPublisher, FString::Printf(TEXT("meta/%s"), *Endpoint.RobotName), Json);
	}
}

void UURSZmqRobotBridgeComponent::PublishState()
{
	AAMjManager* ManagerPtr = Manager.Get();
	if (!StatePublisher || !ManagerPtr || !ManagerPtr->PhysicsEngine)
	{
		return;
	}

	mjModel* Model = ManagerPtr->PhysicsEngine->GetModel();
	mjData* Data = ManagerPtr->PhysicsEngine->GetData();
	if (!Model || !Data)
	{
		return;
	}

	const double NowSec = GetWorld() ? GetWorld()->GetTimeSeconds() : FPlatformTime::Seconds();
	for (const FRobotRuntimeEndpoint& Endpoint : RuntimeEndpoints)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("version"), TEXT("urs_state_v1"));
		Root->SetStringField(TEXT("robot"), Endpoint.RobotName);
		Root->SetNumberField(TEXT("sim_time"), Data->time);
		Root->SetNumberField(TEXT("sequence"), static_cast<double>(Endpoint.CommandBuffer.GetLastAcceptedSequence()));
		Root->SetBoolField(TEXT("command_timed_out"), Endpoint.CommandBuffer.IsTimedOut(NowSec));

		TArray<TSharedPtr<FJsonValue>> QposJson;
		TArray<TSharedPtr<FJsonValue>> QvelJson;
		for (int32 JointId : Endpoint.JointIds)
		{
			if (JointId < 0 || JointId >= Model->njnt)
			{
				continue;
			}
			const int32 QposBegin = Model->jnt_qposadr[JointId];
			const int32 QposEnd = JointId + 1 < Model->njnt ? Model->jnt_qposadr[JointId + 1] : Model->nq;
			for (int32 Idx = QposBegin; Idx < QposEnd; ++Idx)
			{
				QposJson.Add(MakeShared<FJsonValueNumber>(Data->qpos[Idx]));
			}

			const int32 QvelBegin = Model->jnt_dofadr[JointId];
			const int32 QvelEnd = JointId + 1 < Model->njnt ? Model->jnt_dofadr[JointId + 1] : Model->nv;
			for (int32 Idx = QvelBegin; Idx < QvelEnd; ++Idx)
			{
				QvelJson.Add(MakeShared<FJsonValueNumber>(Data->qvel[Idx]));
			}
		}
		Root->SetArrayField(TEXT("qpos"), QposJson);
		Root->SetArrayField(TEXT("qvel"), QvelJson);

		TArray<TSharedPtr<FJsonValue>> CommandJson;
		const TArray<float> Command = Endpoint.CommandBuffer.GetCommandOrZero(NowSec);
		for (float Value : Command)
		{
			CommandJson.Add(MakeShared<FJsonValueNumber>(Value));
		}
		Root->SetArrayField(TEXT("motor_command"), CommandJson);

		FString Json;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
		SendJsonMessage(StatePublisher, Endpoint.StateTopic, Json);
	}
}
