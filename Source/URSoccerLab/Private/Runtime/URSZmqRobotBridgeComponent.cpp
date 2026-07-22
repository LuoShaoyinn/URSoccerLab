#include "Runtime/URSZmqRobotBridgeComponent.h"

#include "Runtime/URSAdminProtocol.h"
#include "Scene/URSSceneConfigComponent.h"
#include "MuJoCo/Components/Actuators/MjActuator.h"
#include "MuJoCo/Components/Joints/MjJoint.h"
#include "MuJoCo/Components/Sensors/MjCamera.h"
#include "MuJoCo/Core/AMjManager.h"
#include "MuJoCo/Core/MjArticulation.h"
#include "MuJoCo/Core/MjPhysicsEngine.h"
#include "MuJoCo/Utils/MjUtils.h"
#include "Transport/NetworkManager.h"
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
void ConfigureSocketBase(void* Socket)
{
	int LingerMs = 0;
	zmq_setsockopt(Socket, ZMQ_LINGER, &LingerMs, sizeof(LingerMs));
}

void ConfigureCommandSocket(void* Socket)
{
	ConfigureSocketBase(Socket);
	int Conflate = 1;
	zmq_setsockopt(Socket, ZMQ_CONFLATE, &Conflate, sizeof(Conflate));
}

void ConfigureAdminSocket(void* Socket)
{
	ConfigureSocketBase(Socket);
}

FString RecvZmqString(void* Socket)
{
	zmq_msg_t Message;
	zmq_msg_init(&Message);
	if (zmq_msg_recv(&Message, Socket, ZMQ_DONTWAIT) < 0)
	{
		zmq_msg_close(&Message);
		return FString();
	}
	const int32 Size = static_cast<int32>(zmq_msg_size(&Message));
	const ANSICHAR* Data = static_cast<const ANSICHAR*>(zmq_msg_data(&Message));
	FUTF8ToTCHAR Converter(Data, Size);
	const FString Body(Converter.Length(), Converter.Get());
	zmq_msg_close(&Message);
	return Body;
}

bool SendZmqString(void* Socket, const FString& Body)
{
	const FTCHARToUTF8 BodyUtf8(*Body);
	const int Rc = zmq_send(Socket, BodyUtf8.Get(), BodyUtf8.Length(), ZMQ_DONTWAIT);
	return Rc >= 0;
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
		if (bAutoStart)
		{
			StartBridge();
		}
		return;
	}

	DrainAdminSockets();

	if (bUsePhysicsCallbacks)
	{
		return;
	}

	DrainCommandSockets();
	const double NowSec = FPlatformTime::Seconds();
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

void UURSZmqRobotBridgeComponent::PullRobotNamesFromSceneConfig()
{
	UURSSceneConfigComponent* SceneComp = SceneConfig.Get();
	if (!SceneComp)
	{
		return;
	}
	TArray<FString> NewNames;
	NewNames.Reserve(SceneComp->GetActiveConfig().Robots.Num());
	for (const URSoccerLab::FURSRobotSpawn& Spawn : SceneComp->GetActiveConfig().Robots)
	{
		NewNames.Add(Spawn.ActorId);
	}
	if (NewNames.Num() > 0)
	{
		RobotNames = MoveTemp(NewNames);
	}
}

void UURSZmqRobotBridgeComponent::OnSceneConfigApplied()
{
	PullRobotNamesFromSceneConfig();

	if (!bBridgeStarted)
	{
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("URSoccerLab ZMQ bridge: scene config applied; rebuilding endpoints for %d robot(s)."), RobotNames.Num());
	StopBridge();
	StartBridge();
}

URSoccerLab::FRobotRuntimeConfig UURSZmqRobotBridgeComponent::MakeRuntimeConfig() const{
	URSoccerLab::FRobotRuntimeConfig Config;
	Config.CommandBasePort = CommandBasePort;
	Config.AdminBasePort = AdminBasePort;
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
	if (!BindAdminSockets())
	{
		StopBridge();
		return false;
	}
	if (!BindPublisherSockets())
	{
		StopBridge();
		return false;
	}

	if (AActor* Owner = GetOwner())
	{
		SceneConfig = Owner->FindComponentByClass<UURSSceneConfigComponent>();
	}

	if (UURSSceneConfigComponent* SceneComp = SceneConfig.Get())
	{
		PullRobotNamesFromSceneConfig();
		SceneComp->OnSceneConfigApplied.AddDynamic(this, &UURSZmqRobotBridgeComponent::OnSceneConfigApplied);
	}

	bBridgeStarted = true;
	RegisterPhysicsCallbacks();
	PublishMetadata();
	LastMetaPublishSec = FPlatformTime::Seconds();
	LastStatePublishSec = 0.0;

	UE_LOG(LogTemp, Log, TEXT("URSoccerLab ZMQ bridge started with %d robot command endpoints."), RuntimeEndpoints.Num());
	UE_LOG(LogTemp, Log, TEXT("URSoccerLab admin RPC ready: %d per-robot REP socket(s) on ports %d..%d."),
		RuntimeEndpoints.Num(),
		AdminBasePort,
		RuntimeEndpoints.Num() > 0 ? AdminBasePort + RuntimeEndpoints.Num() - 1 : AdminBasePort);
	return true;
}

void UURSZmqRobotBridgeComponent::StopBridge()
{
	if (UURSSceneConfigComponent* SceneComp = SceneConfig.Get())
	{
		SceneComp->OnSceneConfigApplied.RemoveDynamic(this, &UURSZmqRobotBridgeComponent::OnSceneConfigApplied);
	}

	if (AAMjManager* ManagerPtr = Manager.Get())
	{
		if (ManagerPtr->PhysicsEngine)
		{
			FScopeLock Lock(&ManagerPtr->PhysicsEngine->CallbackMutex);
			bBridgeStarted = false;
			CloseCommandSockets();
			CloseAdminSockets();
			ClosePublisherSockets();
		}
		else
		{
			bBridgeStarted = false;
			CloseCommandSockets();
			CloseAdminSockets();
			ClosePublisherSockets();
		}
	}
	else
	{
		bBridgeStarted = false;
		CloseCommandSockets();
		CloseAdminSockets();
		ClosePublisherSockets();
	}
	if (ZmqContext)
	{
		zmq_ctx_term(ZmqContext);
		ZmqContext = nullptr;
	}
}

bool UURSZmqRobotBridgeComponent::RebuildEndpointCache()
{
	CloseCommandSockets();
	CloseAdminSockets();
	RuntimeEndpoints.Reset();
	EndpointInfo.Reset();
	AdminEndpointInfo.Reset();

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
		Endpoint.AdminEndpoint = URSoccerLab::FRobotProtocol::BuildTcpBindEndpoint(Assignment.AdminPort);
		Endpoint.StateTopic = FString::Printf(TEXT("state/%s"), *Assignment.RobotName);
		Endpoint.Articulation = *ArticulationPtr;
		Endpoint.CommandBuffer.Configure(Actuators.Num(), Config.CommandTimeoutSec);

		FURSRobotEndpointInfo Info;
		Info.RobotName = Endpoint.RobotName;
		Info.CommandEndpoint = Endpoint.CommandEndpoint;
		Info.StateTopic = Endpoint.StateTopic;

		FURSAdminEndpointInfo AdminInfo;
		AdminInfo.RobotName = Endpoint.RobotName;
		AdminInfo.AdminEndpoint = Endpoint.AdminEndpoint;

		for (UMjActuator* Actuator : Actuators)
		{
			const FString CleanActuatorName =
				URSoccerLab::FRobotProtocol::NormalizeRobotComponentName(Actuator->GetMjName(), Endpoint.RobotName);
			Endpoint.Actuators.Add(Actuator);
			Endpoint.ActuatorNames.Add(CleanActuatorName);
			Endpoint.ActuatorIds.Add(Actuator->GetMjID());
			Info.ActuatorNames.Add(CleanActuatorName);
			Info.ActuatorIds.Add(Actuator->GetMjID());
		}
		for (UMjJoint* Joint : Joints)
		{
			const FString CleanJointName =
				URSoccerLab::FRobotProtocol::NormalizeRobotComponentName(Joint->GetMjName(), Endpoint.RobotName);
			Endpoint.Joints.Add(Joint);
			Endpoint.JointNames.Add(CleanJointName);
			Endpoint.JointIds.Add(Joint->GetMjID());
		}

		RuntimeEndpoints.Add(MoveTemp(Endpoint));
		EndpointInfo.Add(MoveTemp(Info));
		AdminEndpointInfo.Add(MoveTemp(AdminInfo));
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

		ConfigureCommandSocket(Endpoint.CommandSocket);
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

bool UURSZmqRobotBridgeComponent::BindAdminSockets()
{
	if (!ZmqContext)
	{
		return false;
	}

	for (FRobotRuntimeEndpoint& Endpoint : RuntimeEndpoints)
	{
		Endpoint.AdminSocket = zmq_socket(ZmqContext, ZMQ_REP);
		if (!Endpoint.AdminSocket)
		{
			UE_LOG(LogTemp, Error, TEXT("URSoccerLab admin RPC: failed to create REP socket for %s."), *Endpoint.RobotName);
			return false;
		}

		ConfigureAdminSocket(Endpoint.AdminSocket);
		const FTCHARToUTF8 EndpointUtf8(*Endpoint.AdminEndpoint);
		if (zmq_bind(Endpoint.AdminSocket, EndpointUtf8.Get()) != 0)
		{
			UE_LOG(LogTemp, Error, TEXT("URSoccerLab admin RPC: failed to bind %s for %s."),
				*Endpoint.AdminEndpoint, *Endpoint.RobotName);
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

	ConfigureSocketBase(StatePublisher);
	ConfigureSocketBase(MetaPublisher);

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

void UURSZmqRobotBridgeComponent::CloseAdminSockets()
{
	for (FRobotRuntimeEndpoint& Endpoint : RuntimeEndpoints)
	{
		if (Endpoint.AdminSocket)
		{
			zmq_close(Endpoint.AdminSocket);
			Endpoint.AdminSocket = nullptr;
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
	const double NowSec = FPlatformTime::Seconds();

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
	if (zmq_send(Socket, TopicUtf8.Get(), TopicUtf8.Length(), ZMQ_SNDMORE | ZMQ_DONTWAIT) < 0)
	{
		return false;
	}
	return zmq_send(Socket, JsonUtf8.Get(), JsonUtf8.Length(), ZMQ_DONTWAIT) >= 0;
}

int32 UURSZmqRobotBridgeComponent::DrainAdminSockets()
{
	if (!bBridgeStarted)
	{
		return 0;
	}

	int32 HandledCount = 0;
	for (FRobotRuntimeEndpoint& Endpoint : RuntimeEndpoints)
	{
		if (!Endpoint.AdminSocket)
		{
			continue;
		}

		zmq_pollitem_t Item;
		Item.socket = Endpoint.AdminSocket;
		Item.events = ZMQ_POLLIN;
		Item.revents = 0;
		Item.fd = 0;
		Item.revents = 0;
		if (zmq_poll(&Item, 1, 0) <= 0 || (Item.revents & ZMQ_POLLIN) == 0)
		{
			continue;
		}

		const FString Body = RecvZmqString(Endpoint.AdminSocket);
		if (Body.IsEmpty())
		{
			continue;
		}

		const FString Reply = HandleAdminRequest(Endpoint, Body);
		SendZmqString(Endpoint.AdminSocket, Reply);
		++HandledCount;
	}
	return HandledCount;
}

FString UURSZmqRobotBridgeComponent::HandleAdminRequest(FRobotRuntimeEndpoint& Endpoint, const FString& RequestBody)
{
	URSoccerLab::FAdminPoseRequest Req;
	const URSoccerLab::EAdminRequestParse ParseStatus = URSoccerLab::FAdminProtocol::ParseRequest(RequestBody, Req);
	if (ParseStatus != URSoccerLab::EAdminRequestParse::Accepted)
	{
		return URSoccerLab::FAdminProtocol::BuildErrorReply(
			TEXT("unknown"), TEXT("bad_request"), URSoccerLab::FAdminProtocol::LexToString(ParseStatus));
	}

	switch (Req.Op)
	{
	case URSoccerLab::EAdminOp::SetPose:
		return HandleSetPose(Endpoint, Req);
	case URSoccerLab::EAdminOp::GetPose:
		return HandleGetPose(Endpoint);
	case URSoccerLab::EAdminOp::Reset:
		return HandleReset(Endpoint);
	default:
		return URSoccerLab::FAdminProtocol::BuildErrorReply(TEXT("unknown"), TEXT("bad_request"), TEXT("unsupported op"));
	}
}

namespace
{
struct FQposSlot { int32 Adr; int32 Size; int32 JointType; };

struct FQposLayout
{
	TArray<FQposSlot> RootSlots;    // typically one FREE joint
	TArray<FQposSlot> NonRootSlots;
	int32 NonRootQposDim = 0;
};

FQposLayout DiscoverQposLayout(AMjArticulation* Articulation, const mjModel* Model)
{
	FQposLayout Layout;
	for (UMjJoint* Joint : Articulation->GetJoints())
	{
		if (!Joint)
		{
			continue;
		}
		const int32 JointId = Joint->GetMjID();
		if (JointId < 0 || JointId >= Model->njnt)
		{
			continue;
		}
		int32 Size = 1;
		switch (Model->jnt_type[JointId])
		{
		case mjJNT_FREE: Size = 7; break;
		case mjJNT_BALL: Size = 4; break;
		case mjJNT_SLIDE:
		case mjJNT_HINGE: Size = 1; break;
		default: break;
		}
		const FQposSlot Slot{Model->jnt_qposadr[JointId], Size, Model->jnt_type[JointId]};
		if (Slot.JointType == mjJNT_FREE)
		{
			Layout.RootSlots.Add(Slot);
		}
		else
		{
			Layout.NonRootSlots.Add(Slot);
			Layout.NonRootQposDim += Size;
		}
	}
	return Layout;
}

int32 DiscoverRootBodyId(AMjArticulation* Articulation, const mjModel* Model)
{
	for (UMjJoint* Joint : Articulation->GetJoints())
	{
		if (!Joint)
		{
			continue;
		}
		const int32 JointId = Joint->GetMjID();
		if (JointId < 0 || JointId >= Model->njnt)
		{
			continue;
		}
		const int32 JointBodyId = Model->jnt_bodyid[JointId];
		if (JointBodyId <= 0)
		{
			continue;
		}
		return Model->body_rootid[JointBodyId];
	}
	return -1;
}
} // namespace

FString UURSZmqRobotBridgeComponent::HandleSetPose(FRobotRuntimeEndpoint& Endpoint, const URSoccerLab::FAdminPoseRequest& Req)
{
	AMjArticulation* Articulation = Endpoint.Articulation.Get();
	AAMjManager* ManagerPtr = Manager.Get();
	if (!Articulation || !ManagerPtr || !ManagerPtr->PhysicsEngine)
	{
		return URSoccerLab::FAdminProtocol::BuildErrorReply(TEXT("set_pose"), TEXT("not_ready"), TEXT("articulation or physics missing"));
	}

	mjModel* Model = ManagerPtr->PhysicsEngine->GetModel();
	mjData* Data = ManagerPtr->PhysicsEngine->GetData();
	if (!Model || !Data)
	{
		return URSoccerLab::FAdminProtocol::BuildErrorReply(TEXT("set_pose"), TEXT("not_ready"), TEXT("mjModel/mjData missing"));
	}

	const FQposLayout Layout = DiscoverQposLayout(Articulation, Model);

	if (Req.JointQpos.IsSet() && Req.JointQpos.GetValue().Num() != Layout.NonRootQposDim)
	{
		return URSoccerLab::FAdminProtocol::BuildErrorReply(
			TEXT("set_pose"), TEXT("dim_mismatch"),
			FString::Printf(TEXT("joint_qpos length %d != non-root qpos dim %d"),
				Req.JointQpos.GetValue().Num(), Layout.NonRootQposDim));
	}

	if (Layout.RootSlots.IsEmpty() && (Req.TranslationMeters.IsSet() || Req.RotationQuatXyzw.IsSet()))
	{
		return URSoccerLab::FAdminProtocol::BuildErrorReply(
			TEXT("set_pose"), TEXT("fixed_base"),
			TEXT("translation_m/rotation_quat_xyzw require a free root joint; this articulation is fixed to the world"));
	}

	const FVector Translation = Req.TranslationMeters.Get(FVector::ZeroVector);
	const FQuat Rotation = Req.RotationQuatXyzw.Get(FQuat::Identity);
	const TArray<float> JointQpos = Req.JointQpos.Get(TArray<float>());

	TArray<float> AppliedJointQpos;
	AppliedJointQpos.Reserve(Layout.NonRootQposDim);

	{
		FScopeLock Lock(&ManagerPtr->PhysicsEngine->CallbackMutex);

		if (!Layout.RootSlots.IsEmpty())
		{
			// MuJoCo free-joint qpos layout: [x, y, z, qw, qx, qy, qz].
			const int32 Adr = Layout.RootSlots[0].Adr;
			Data->qpos[Adr + 0] = Translation.X;
			Data->qpos[Adr + 1] = Translation.Y;
			Data->qpos[Adr + 2] = Translation.Z;
			Data->qpos[Adr + 3] = Rotation.W;
			Data->qpos[Adr + 4] = Rotation.X;
			Data->qpos[Adr + 5] = Rotation.Y;
			Data->qpos[Adr + 6] = Rotation.Z;
		}

		int32 JointCursor = 0;
		for (const FQposSlot& Slot : Layout.NonRootSlots)
		{
			for (int32 Idx = 0; Idx < Slot.Size; ++Idx)
			{
				const float Value = JointQpos.IsValidIndex(JointCursor) ? JointQpos[JointCursor] : 0.0f;
				Data->qpos[Slot.Adr + Idx] = static_cast<mjtNum>(Value);
				AppliedJointQpos.Add(Value);
				++JointCursor;
			}
		}

		for (UMjJoint* Joint : Articulation->GetJoints())
		{
			if (!Joint)
			{
				continue;
			}
			const int32 JointId = Joint->GetMjID();
			if (JointId < 0 || JointId >= Model->njnt)
			{
				continue;
			}
			const int32 DofAdr = Model->jnt_dofadr[JointId];
			int32 DofSize = 1;
			switch (Model->jnt_type[JointId])
			{
			case mjJNT_FREE: DofSize = 6; break;
			case mjJNT_BALL: DofSize = 3; break;
			default: break;
			}
			for (int32 Idx = 0; Idx < DofSize; ++Idx)
			{
				Data->qvel[DofAdr + Idx] = 0.0;
			}
		}

		mj_forward(Model, Data);
	}

	// Reply repacks the root rotation back to the wire's (x, y, z, w) order.
	return URSoccerLab::FAdminProtocol::BuildOkSetPoseReply(
		Endpoint.RobotName, Translation, Rotation, AppliedJointQpos, Data->time);
}

FString UURSZmqRobotBridgeComponent::HandleGetPose(FRobotRuntimeEndpoint& Endpoint)
{
	AMjArticulation* Articulation = Endpoint.Articulation.Get();
	AAMjManager* ManagerPtr = Manager.Get();
	if (!Articulation || !ManagerPtr || !ManagerPtr->PhysicsEngine)
	{
		return URSoccerLab::FAdminProtocol::BuildErrorReply(TEXT("get_pose"), TEXT("not_ready"), TEXT("articulation or physics missing"));
	}

	mjModel* Model = ManagerPtr->PhysicsEngine->GetModel();
	mjData* Data = ManagerPtr->PhysicsEngine->GetData();
	if (!Model || !Data)
	{
		return URSoccerLab::FAdminProtocol::BuildErrorReply(TEXT("get_pose"), TEXT("not_ready"), TEXT("mjModel/mjData missing"));
	}

	const FQposLayout Layout = DiscoverQposLayout(Articulation, Model);
	const int32 RootBodyId = DiscoverRootBodyId(Articulation, Model);

	FVector Translation = FVector::ZeroVector;
	FQuat Rotation = FQuat::Identity;
	TArray<float> JointQpos;
	JointQpos.Reserve(Layout.NonRootQposDim);

	{
		FScopeLock Lock(&ManagerPtr->PhysicsEngine->CallbackMutex);

		if (RootBodyId > 0 && RootBodyId < Model->nbody)
		{
			const mjtNum* Xpos = Data->xpos + RootBodyId * 3;
			const mjtNum* Xquat = Data->xquat + RootBodyId * 4;  // MuJoCo layout: w, x, y, z
			Translation = FVector(Xpos[0], Xpos[1], Xpos[2]);
			Rotation = FQuat(Xquat[1], Xquat[2], Xquat[3], Xquat[0]);
		}
		else if (!Layout.RootSlots.IsEmpty())
		{
			const int32 Adr = Layout.RootSlots[0].Adr;
			Translation = FVector(Data->qpos[Adr + 0], Data->qpos[Adr + 1], Data->qpos[Adr + 2]);
			Rotation = FQuat(Data->qpos[Adr + 4], Data->qpos[Adr + 5], Data->qpos[Adr + 6], Data->qpos[Adr + 3]);
		}

		for (const FQposSlot& Slot : Layout.NonRootSlots)
		{
			for (int32 Idx = 0; Idx < Slot.Size; ++Idx)
			{
				JointQpos.Add(static_cast<float>(Data->qpos[Slot.Adr + Idx]));
			}
		}
	}

	return URSoccerLab::FAdminProtocol::BuildOkGetPoseReply(
		Endpoint.RobotName, Translation, Rotation, JointQpos, Data->time);
}

FString UURSZmqRobotBridgeComponent::HandleReset(FRobotRuntimeEndpoint& Endpoint)
{
	UURSSceneConfigComponent* SceneComp = SceneConfig.Get();
	FVector InitialTranslation = FVector::ZeroVector;
	FQuat InitialRotation = FQuat::Identity;
	bool bHaveInitialPose = false;
	if (SceneComp)
	{
		bHaveInitialPose = SceneComp->GetInitialPose(Endpoint.RobotName, InitialTranslation, InitialRotation);
	}
	if (!bHaveInitialPose)
	{
		UE_LOG(LogTemp, Verbose, TEXT("URSoccerLab admin reset: no stashed initial pose for %s; defaulting to all-zero qpos."),
			*Endpoint.RobotName);
	}

	URSoccerLab::FAdminPoseRequest Req;
	Req.Op = URSoccerLab::EAdminOp::SetPose;

	AMjArticulation* Articulation = Endpoint.Articulation.Get();
	AAMjManager* ManagerPtr = Manager.Get();
	if (Articulation && ManagerPtr && ManagerPtr->PhysicsEngine)
	{
		mjModel* Model = ManagerPtr->PhysicsEngine->GetModel();
		if (Model)
		{
			const FQposLayout Layout = DiscoverQposLayout(Articulation, Model);
			if (bHaveInitialPose && !Layout.RootSlots.IsEmpty())
			{
				Req.TranslationMeters = InitialTranslation;
				Req.RotationQuatXyzw = InitialRotation;
			}
		}
	}

	return HandleSetPose(Endpoint, Req);
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
		Root->SetStringField(TEXT("admin_endpoint"), Endpoint.AdminEndpoint);
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

		TArray<TSharedPtr<FJsonValue>> CamerasJson;
		if (AAMjManager* ManagerPtr = Manager.Get())
		{
			if (ManagerPtr->NetworkManager)
			{
				const TArray<UMjCamera*> Cameras = ManagerPtr->NetworkManager->GetActiveCameras();
				for (UMjCamera* Camera : Cameras)
				{
					if (!Camera)
					{
						continue;
					}
					const AMjArticulation* CameraArticulation = Cast<AMjArticulation>(Camera->GetOwner());
					const FString CameraPrefix = CameraArticulation ? CameraArticulation->GetName()
																	 : (Camera->GetOwner() ? Camera->GetOwner()->GetName() : TEXT("unknown"));
					if (CameraArticulation && CameraArticulation->ActorId == Endpoint.RobotName)
					{
						// Accept stable ActorId match as equivalent to actor name below.
					}
					else if (CameraPrefix != Endpoint.RobotName)
					{
						continue;
					}

					TSharedPtr<FJsonObject> CameraJson = MakeShared<FJsonObject>();
					CameraJson->SetStringField(TEXT("name"), Camera->GetName());
					CameraJson->SetStringField(TEXT("topic"), FString::Printf(TEXT("%s/camera/%s"), *CameraPrefix, *Camera->GetName()));
					CameraJson->SetStringField(TEXT("endpoint"), Camera->GetActualZmqEndpoint());
					CameraJson->SetStringField(TEXT("format"), Camera->CaptureMode == EMjCameraMode::Depth ? TEXT("float32_depth") : TEXT("bgra8"));
					CameraJson->SetNumberField(TEXT("width"), Camera->resolution.Num() > 0 ? Camera->resolution[0] : 0);
					CameraJson->SetNumberField(TEXT("height"), Camera->resolution.Num() > 1 ? Camera->resolution[1] : 0);
					CamerasJson.Add(MakeShared<FJsonValueObject>(CameraJson));
				}
			}
		}
		Root->SetArrayField(TEXT("cameras"), CamerasJson);

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

	const double NowSec = FPlatformTime::Seconds();
	PublishStateFromData(Model, Data, NowSec);
}

void UURSZmqRobotBridgeComponent::RegisterPhysicsCallbacks()
{
	if (bCallbacksRegistered || !bUsePhysicsCallbacks)
	{
		return;
	}

	AAMjManager* ManagerPtr = Manager.Get();
	if (!ManagerPtr || !ManagerPtr->PhysicsEngine)
	{
		return;
	}

	TWeakObjectPtr<UURSZmqRobotBridgeComponent> WeakSelf(this);
	ManagerPtr->PhysicsEngine->RegisterPreStepCallback([WeakSelf](mjModel* Model, mjData* Data) {
		if (UURSZmqRobotBridgeComponent* Self = WeakSelf.Get())
		{
			Self->PreStepPhysics(Model, Data);
		}
	});
	ManagerPtr->PhysicsEngine->RegisterPostStepCallback([WeakSelf](mjModel* Model, mjData* Data) {
		if (UURSZmqRobotBridgeComponent* Self = WeakSelf.Get())
		{
			Self->PostStepPhysics(Model, Data);
		}
	});

	bCallbacksRegistered = true;
}

void UURSZmqRobotBridgeComponent::PreStepPhysics(mjModel* Model, mjData* Data)
{
	if (!bBridgeStarted)
	{
		return;
	}

	const double NowSec = FPlatformTime::Seconds();
	DrainCommandSockets();
	ApplyLatestCommands(NowSec);
}

void UURSZmqRobotBridgeComponent::PostStepPhysics(mjModel* Model, mjData* Data)
{
	if (!bBridgeStarted)
	{
		return;
	}

	const double NowSec = FPlatformTime::Seconds();
	const double StateIntervalSec = StatePublishRateHz > 0.0 ? 1.0 / StatePublishRateHz : 0.0;
	if (StateIntervalSec <= 0.0 || NowSec - LastStatePublishSec >= StateIntervalSec)
	{
		PublishStateFromData(Model, Data, NowSec);
		LastStatePublishSec = NowSec;
	}
	if (NowSec - LastMetaPublishSec >= MetaPublishIntervalSec)
	{
		PublishMetadata();
		LastMetaPublishSec = NowSec;
	}
}

void UURSZmqRobotBridgeComponent::PublishStateFromData(mjModel* Model, mjData* Data, double NowSec)
{
	if (!StatePublisher || !Model || !Data)
	{
		return;
	}

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
