#include "Core/URSRobotCoreComponent.h"

#include "Scene/URSSceneConfigComponent.h"
#include "Scene/URSRobotTypeRegistry.h"
#include "Runtime/URSRobotProtocol.h"
#include "Runtime/URSZmqRobotBridgeComponent.h"
#include "MuJoCo/Components/Actuators/MjActuator.h"
#include "MuJoCo/Components/Joints/MjJoint.h"
#include "MuJoCo/Components/Sensors/MjCamera.h"
#include "MuJoCo/Core/AMjManager.h"
#include "MuJoCo/Core/MjArticulation.h"
#include "MuJoCo/Core/MjPhysicsEngine.h"
#include "MuJoCo/Utils/MjUtils.h"
#include "Transport/NetworkManager.h"

struct UURSRobotCoreComponent::FQposLayout
{
	struct FSlot { int32 Adr; int32 Size; int32 JointType; };
	TArray<FSlot> RootSlots;
	TArray<FSlot> NonRootSlots;
	int32 NonRootQposDim = 0;
};

UURSRobotCoreComponent::UURSRobotCoreComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
}

void UURSRobotCoreComponent::BeginPlay()
{
	Super::BeginPlay();
	if (bAutoStart)
	{
		Initialize();
	}
}

void UURSRobotCoreComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Super::EndPlay(EndPlayReason);
}

void UURSRobotCoreComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFn)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFn);
	if (!bInitialized && bAutoStart)
	{
		Initialize();
	}
	if (bInitialized && Endpoints.Num() == 0)
	{
		AAMjManager* Mgr = Manager.Get();
		if (Mgr && Mgr->GetAllArticulations().Num() > 0)
		{
			RebuildEndpointCache();
			OnRobotsChanged.Broadcast();
			UE_LOG(LogTemp, Log, TEXT("[URS Core] Late-discovered %d robot(s)."), Endpoints.Num());
		}
	}
	if (bInitialized && Endpoints.Num() > 0)
	{
		SetComponentTickEnabled(false);
	}
}

UURSRobotCoreComponent::FRobotEndpoint* UURSRobotCoreComponent::FindEndpoint(const FString& ActorId)
{
	for (FRobotEndpoint& Ep : Endpoints)
	{
		if (Ep.ActorId == ActorId) return &Ep;
	}
	return nullptr;
}

const UURSRobotCoreComponent::FRobotEndpoint* UURSRobotCoreComponent::FindEndpoint(const FString& ActorId) const
{
	for (const FRobotEndpoint& Ep : Endpoints)
	{
		if (Ep.ActorId == ActorId) return &Ep;
	}
	return nullptr;
}

bool UURSRobotCoreComponent::Initialize()
{
	if (bInitialized) return true;

	AActor* Owner = GetOwner();
	Manager = Cast<AAMjManager>(Owner);
	if (!Manager.IsValid())
	{
		Manager = AAMjManager::GetManager();
	}
	AAMjManager* ManagerPtr = Manager.Get();
	if (!ManagerPtr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[URS Core] No AAMjManager found."));
		return false;
	}

	if (Owner)
	{
		SceneConfigComp = Owner->FindComponentByClass<UURSSceneConfigComponent>();
	}

	if (UURSZmqRobotBridgeComponent* ZmqBridge = ManagerPtr->FindComponentByClass<UURSZmqRobotBridgeComponent>())
	{
		UE_LOG(LogTemp, Log, TEXT("[URS Core] Disabling legacy ZMQ bridge."));
		ZmqBridge->StopBridge();
		ZmqBridge->bAutoStart = false;
	}

	RebuildEndpointCache();

	RegisterPhysicsCallbacks();
	if (!bCallbacksRegistered)
	{
		UE_LOG(LogTemp, Log, TEXT("[URS Core] Physics engine not ready; will retry next tick."));
		Endpoints.Reset();
		return false;
	}

	if (UURSSceneConfigComponent* SceneComp = Cast<UURSSceneConfigComponent>(SceneConfigComp.Get()))
	{
		SceneComp->OnSceneConfigApplied.AddDynamic(this, &UURSRobotCoreComponent::OnSceneConfigApplied);
	}

	bInitialized = true;
	UE_LOG(LogTemp, Log, TEXT("[URS Core] Initialized with %d robot(s)."), Endpoints.Num());
	return true;
}

void UURSRobotCoreComponent::OnSceneConfigApplied()
{
	RebuildEndpointCache();
	OnRobotsChanged.Broadcast();
}

void UURSRobotCoreComponent::RebuildEndpointCache()
{
	Endpoints.Reset();

	AAMjManager* ManagerPtr = Manager.Get();
	if (!ManagerPtr) return;

	UURSSceneConfigComponent* SceneComp = Cast<UURSSceneConfigComponent>(SceneConfigComp.Get());

	TMap<FString, AMjArticulation*> ArticulationsByName;
	for (AMjArticulation* Articulation : ManagerPtr->GetAllArticulations())
	{
		if (!Articulation) continue;
		ArticulationsByName.Add(Articulation->GetName(), Articulation);
		if (!Articulation->ActorId.IsEmpty())
		{
			ArticulationsByName.Add(Articulation->ActorId, Articulation);
		}
	}

	auto BuildEndpoint = [&](AMjArticulation* Articulation, const FString& ActorId)
	{
		FRobotEndpoint Ep;
		Ep.ActorId = ActorId;
		Ep.Articulation = Articulation;

		TArray<UMjActuator*> Actuators = Articulation->GetActuators();
		Actuators.RemoveAll([](UMjActuator* A) { return !A || A->GetMjID() < 0; });
		Actuators.Sort([](const UMjActuator& L, const UMjActuator& R) { return L.GetMjID() < R.GetMjID(); });

		for (UMjActuator* Actuator : Actuators)
		{
			FString CleanName = URSoccerLab::FRobotProtocol::NormalizeRobotComponentName(Actuator->GetMjName(), ActorId);
			FActuatorInfo Info;
			Info.Actuator = Actuator;
			Info.Name = CleanName;
			Info.MjId = Actuator->GetMjID();
			Ep.ActuatorNameToIndex.Add(CleanName, Ep.Actuators.Num());
			Ep.Actuators.Add(MoveTemp(Info));
		}
		Ep.LatestCommand.Init(0.0f, Ep.Actuators.Num());

		TArray<UMjJoint*> Joints = Articulation->GetJoints();
		Joints.RemoveAll([](UMjJoint* J) { return !J || J->GetMjID() < 0; });
		Joints.Sort([](const UMjJoint& L, const UMjJoint& R) { return L.GetMjID() < R.GetMjID(); });

		for (UMjJoint* Joint : Joints)
		{
			FString CleanName = URSoccerLab::FRobotProtocol::NormalizeRobotComponentName(Joint->GetMjName(), ActorId);
			FJointInfo Info;
			Info.Joint = Joint;
			Info.Name = CleanName;
			Info.MjId = Joint->GetMjID();
			Ep.Joints.Add(MoveTemp(Info));
		}

		TArray<UMjCamera*> Cameras;
		if (ManagerPtr->NetworkManager)
		{
			const TArray<UMjCamera*> AllCams = ManagerPtr->NetworkManager->GetActiveCameras();
			for (UMjCamera* Cam : AllCams)
			{
				if (!Cam) continue;
				const AMjArticulation* CamOwner = Cast<AMjArticulation>(Cam->GetOwner());
				if (CamOwner && (CamOwner->ActorId == ActorId || CamOwner->GetName() == ActorId))
				{
					Cameras.Add(Cam);
				}
			}
		}

		for (UMjCamera* Cam : Cameras)
		{
			Cam->bEnableZmqBroadcast = false;
			Cam->bEnableShmBroadcast = false;
			if (!Cam->IsStreamingActive())
			{
				Cam->SetStreamingEnabled(true);
			}

			FCameraEntry Entry;
			Entry.Camera = Cam;
			Entry.Name = Cam->GetName();
			Ep.Cameras.Add(MoveTemp(Entry));
		}

		Endpoints.Add(MoveTemp(Ep));
	};

	if (SceneComp)
	{
		for (const URSoccerLab::FURSRobotSpawn& Spawn : SceneComp->GetActiveConfig().Robots)
		{
			AMjArticulation* const* Found = ArticulationsByName.Find(Spawn.ActorId);
			if (Found && *Found)
			{
				BuildEndpoint(*Found, Spawn.ActorId);
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("[URS Core] Robot '%s' not found in world."), *Spawn.ActorId);
			}
		}
	}

	if (Endpoints.Num() == 0)
	{
		for (auto& Pair : ArticulationsByName)
		{
			BuildEndpoint(Pair.Value, Pair.Value->ActorId.IsEmpty() ? Pair.Value->GetName() : Pair.Value->ActorId);
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[URS Core] Endpoint cache rebuilt: %d robot(s)."), Endpoints.Num());
}

void UURSRobotCoreComponent::RegisterPhysicsCallbacks()
{
	if (bCallbacksRegistered) return;

	AAMjManager* ManagerPtr = Manager.Get();
	if (!ManagerPtr || !ManagerPtr->PhysicsEngine) return;

	TWeakObjectPtr<UURSRobotCoreComponent> WeakSelf(this);
	ManagerPtr->PhysicsEngine->RegisterPreStepCallback([WeakSelf](mjModel* Model, mjData* Data) {
		if (UURSRobotCoreComponent* Self = WeakSelf.Get())
		{
			Self->PreStepPhysics(Model, Data);
		}
	});

	bCallbacksRegistered = true;
}

void UURSRobotCoreComponent::PreStepPhysics(mjModel* /*Model*/, mjData* /*Data*/)
{
	ApplyCommands(FPlatformTime::Seconds());
}

void UURSRobotCoreComponent::ApplyCommands(double NowSec)
{
	for (FRobotEndpoint& Ep : Endpoints)
	{
		bool bTimedOut = !Ep.bHasCommand || (NowSec - Ep.LastCommandTimeSec > CommandTimeoutSec);

		TArray<float> ToApply;
		if (bTimedOut)
		{
			ToApply.Init(0.0f, Ep.Actuators.Num());
		}
		else
		{
			ToApply = Ep.LatestCommand;
		}

		const int32 Count = FMath::Min(ToApply.Num(), Ep.Actuators.Num());
		for (int32 Idx = 0; Idx < Count; ++Idx)
		{
			if (UMjActuator* Actuator = Ep.Actuators[Idx].Actuator.Get())
			{
				Actuator->SetNetworkControl(ToApply[Idx]);
			}
		}
	}
}

TArray<FString> UURSRobotCoreComponent::GetRobotIds() const
{
	TArray<FString> Ids;
	Ids.Reserve(Endpoints.Num());
	for (const FRobotEndpoint& Ep : Endpoints)
	{
		Ids.Add(Ep.ActorId);
	}
	return Ids;
}

bool UURSRobotCoreComponent::GetRobotState(const FString& ActorId, FURSRobotState& OutState)
{
	FRobotEndpoint* Ep = FindEndpoint(ActorId);
	if (!Ep) return false;

	AAMjManager* ManagerPtr = Manager.Get();
	if (!ManagerPtr || !ManagerPtr->PhysicsEngine) return false;

	mjModel* Model = ManagerPtr->PhysicsEngine->GetModel();
	mjData* Data = ManagerPtr->PhysicsEngine->GetData();
	if (!Model || !Data) return false;

	const double NowSec = FPlatformTime::Seconds();

	OutState = FURSRobotState();
	OutState.ActorId = ActorId;
	OutState.SimTime = Data->time;
	OutState.bCommandTimedOut = !Ep->bHasCommand || (NowSec - Ep->LastCommandTimeSec > CommandTimeoutSec);

	// Find root joint for base pose
	int32 RootBodyId = DiscoverRootBodyId(Ep->Articulation.Get(), Model);
	if (RootBodyId > 0 && RootBodyId < Model->nbody)
	{
		const mjtNum* Xpos = Data->xpos + RootBodyId * 3;
		const mjtNum* Xquat = Data->xquat + RootBodyId * 4;
		OutState.BasePos = FVector(Xpos[0], Xpos[1], Xpos[2]);
		OutState.BaseQuat = FQuat(Xquat[1], Xquat[2], Xquat[3], Xquat[0]);
	}
	else
	{
		FQposLayout Layout = DiscoverQposLayout(Ep->Articulation.Get(), Model);
		if (!Layout.RootSlots.IsEmpty())
		{
			int32 Adr = Layout.RootSlots[0].Adr;
			OutState.BasePos = FVector(Data->qpos[Adr], Data->qpos[Adr+1], Data->qpos[Adr+2]);
			OutState.BaseQuat = FQuat(Data->qpos[Adr+4], Data->qpos[Adr+5], Data->qpos[Adr+6], Data->qpos[Adr+3]);
		}
	}

	// Base velocity from free joint qvel
	for (const FJointInfo& JointInfo : Ep->Joints)
	{
		int32 JId = JointInfo.MjId;
		if (JId < 0 || JId >= Model->njnt) continue;
		if (Model->jnt_type[JId] != mjJNT_FREE) continue;
		int32 DofAdr = Model->jnt_dofadr[JId];
		for (int32 i = 0; i < 6; ++i)
		{
			OutState.BaseVel.Add(Data->qvel[DofAdr + i]);
		}
		break;
	}

	// Joints (non-root only — free joint data is in base)
	for (const FJointInfo& JointInfo : Ep->Joints)
	{
		int32 JId = JointInfo.MjId;
		if (JId < 0 || JId >= Model->njnt) continue;
		if (Model->jnt_type[JId] == mjJNT_FREE) continue;

		OutState.JointNames.Add(JointInfo.Name);

		int32 QposBegin = Model->jnt_qposadr[JId];
		int32 QposEnd = (JId + 1 < Model->njnt) ? Model->jnt_qposadr[JId + 1] : Model->nq;
		for (int32 Idx = QposBegin; Idx < QposEnd; ++Idx)
		{
			OutState.JointQpos.Add(Data->qpos[Idx]);
		}

		int32 QvelBegin = Model->jnt_dofadr[JId];
		int32 QvelEnd = (JId + 1 < Model->njnt) ? Model->jnt_dofadr[JId + 1] : Model->nv;
		for (int32 Idx = QvelBegin; Idx < QvelEnd; ++Idx)
		{
			OutState.JointQvel.Add(Data->qvel[Idx]);
		}
	}

	// Actuators
	for (const FActuatorInfo& ActInfo : Ep->Actuators)
	{
		OutState.ActuatorNames.Add(ActInfo.Name);
	}
	const TArray<float> CurrentCmd = OutState.bCommandTimedOut
		? TArray<float>() : Ep->LatestCommand;
	for (int32 Idx = 0; Idx < Ep->Actuators.Num(); ++Idx)
	{
		OutState.MotorCommand.Add(OutState.bCommandTimedOut ? 0.0 : (Idx < CurrentCmd.Num() ? CurrentCmd[Idx] : 0.0));
	}

	// Cameras
	for (const FCameraEntry& CamEntry : Ep->Cameras)
	{
		if (const UMjCamera* Cam = CamEntry.Camera.Get())
		{
			FURSCameraInfo CamInfo;
			CamInfo.Name = CamEntry.Name;
			CamInfo.Format = Cam->CaptureMode == EMjCameraMode::Depth ? TEXT("float32_depth") : TEXT("bgra8");
			CamInfo.Width = Cam->resolution.Num() > 0 ? Cam->resolution[0] : 0;
			CamInfo.Height = Cam->resolution.Num() > 1 ? Cam->resolution[1] : 0;
			OutState.Cameras.Add(MoveTemp(CamInfo));
		}
	}

	return true;
}

void UURSRobotCoreComponent::SubmitCommand(const FString& ActorId, const TMap<FString, float>& NamedValues)
{
	FRobotEndpoint* Ep = FindEndpoint(ActorId);
	if (!Ep) return;

	Ep->LastNamedValues = NamedValues;
	for (const auto& Pair : NamedValues)
	{
		if (const int32* Idx = Ep->ActuatorNameToIndex.Find(Pair.Key))
		{
			if (FMath::IsFinite(Pair.Value))
			{
				Ep->LatestCommand[*Idx] = Pair.Value;
			}
		}
	}

	Ep->LastCommandTimeSec = FPlatformTime::Seconds();
	Ep->bHasCommand = true;
}

bool UURSRobotCoreComponent::RequestCameraReadback(const FString& ActorId)
{
	FRobotEndpoint* Ep = FindEndpoint(ActorId);
	if (!Ep) return false;

	bool bAnyRequested = false;
	for (FCameraEntry& CamEntry : Ep->Cameras)
	{
		if (UMjCamera* Cam = CamEntry.Camera.Get())
		{
			if (!CamEntry.bReadbackRequested && Cam->IsReadbackReady())
			{
				Cam->ConsumePixels();
			}
			Cam->RequestReadback();
			CamEntry.bReadbackRequested = true;
			bAnyRequested = true;
		}
	}
	return bAnyRequested;
}

bool UURSRobotCoreComponent::ConsumeCameraFrame(const FString& ActorId, const FString& CameraName, TArray<FColor>& OutPixels)
{
	FRobotEndpoint* Ep = FindEndpoint(ActorId);
	if (!Ep) return false;

	for (FCameraEntry& CamEntry : Ep->Cameras)
	{
		if (CamEntry.Name != CameraName) continue;

		UMjCamera* Cam = CamEntry.Camera.Get();
		if (!Cam) return false;

		if (!CamEntry.bReadbackRequested) return false;
		if (!Cam->IsReadbackReady()) return false;

		OutPixels = Cam->ConsumePixels();
		CamEntry.bReadbackRequested = false;
		return OutPixels.Num() > 0;
	}
	return false;
}

UURSRobotCoreComponent::FQposLayout UURSRobotCoreComponent::DiscoverQposLayout(AMjArticulation* Articulation, const mjModel* Model)
{
	FQposLayout Layout;
	if (!Articulation || !Model) return Layout;

	for (UMjJoint* Joint : Articulation->GetJoints())
	{
		if (!Joint) continue;
		int32 JointId = Joint->GetMjID();
		if (JointId < 0 || JointId >= Model->njnt) continue;

		int32 Size = 1;
		switch (Model->jnt_type[JointId])
		{
		case mjJNT_FREE: Size = 7; break;
		case mjJNT_BALL: Size = 4; break;
		case mjJNT_SLIDE:
		case mjJNT_HINGE: Size = 1; break;
		default: break;
		}

		FQposLayout::FSlot Slot{Model->jnt_qposadr[JointId], Size, Model->jnt_type[JointId]};
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

int32 UURSRobotCoreComponent::DiscoverRootBodyId(AMjArticulation* Articulation, const mjModel* Model)
{
	if (!Articulation || !Model) return -1;

	for (UMjJoint* Joint : Articulation->GetJoints())
	{
		if (!Joint) continue;
		int32 JointId = Joint->GetMjID();
		if (JointId < 0 || JointId >= Model->njnt) continue;

		int32 JointBodyId = Model->jnt_bodyid[JointId];
		if (JointBodyId <= 0) continue;
		return Model->body_rootid[JointBodyId];
	}
	return -1;
}

FURSPoseResult UURSRobotCoreComponent::SetPose(const FString& ActorId, const FVector* Translation, const FQuat* Rotation, const TArray<float>* JointQpos)
{
	FURSPoseResult Result;
	Result.bOk = false;

	FRobotEndpoint* Ep = FindEndpoint(ActorId);
	if (!Ep)
	{
		Result.Error = TEXT("not_found");
		Result.Message = FString::Printf(TEXT("Robot '%s' not found"), *ActorId);
		return Result;
	}

	AMjArticulation* Articulation = Ep->Articulation.Get();
	AAMjManager* ManagerPtr = Manager.Get();
	if (!Articulation || !ManagerPtr || !ManagerPtr->PhysicsEngine)
	{
		Result.Error = TEXT("not_ready");
		Result.Message = TEXT("Physics engine not ready");
		return Result;
	}

	mjModel* Model = ManagerPtr->PhysicsEngine->GetModel();
	mjData* Data = ManagerPtr->PhysicsEngine->GetData();
	if (!Model || !Data)
	{
		Result.Error = TEXT("not_ready");
		Result.Message = TEXT("mjModel/mjData missing");
		return Result;
	}

	FQposLayout Layout = DiscoverQposLayout(Articulation, Model);

	if (JointQpos && JointQpos->Num() != Layout.NonRootQposDim)
	{
		Result.Error = TEXT("dim_mismatch");
		Result.Message = FString::Printf(TEXT("joint_qpos length %d != non-root qpos dim %d"),
			JointQpos->Num(), Layout.NonRootQposDim);
		return Result;
	}

	if (Layout.RootSlots.IsEmpty() && (Translation || Rotation))
	{
		Result.Error = TEXT("fixed_base");
		Result.Message = TEXT("translation/rotation require a free root joint");
		return Result;
	}

	FVector AppliedTrans = Translation ? *Translation : FVector::ZeroVector;
	FQuat AppliedRot = Rotation ? *Rotation : FQuat::Identity;
	TArray<float> AppliedJoint;
	AppliedJoint.Reserve(Layout.NonRootQposDim);

	{
		FScopeLock Lock(&ManagerPtr->PhysicsEngine->CallbackMutex);

		if (!Layout.RootSlots.IsEmpty())
		{
			int32 Adr = Layout.RootSlots[0].Adr;
			Data->qpos[Adr + 0] = AppliedTrans.X;
			Data->qpos[Adr + 1] = AppliedTrans.Y;
			Data->qpos[Adr + 2] = AppliedTrans.Z;
			Data->qpos[Adr + 3] = AppliedRot.W;
			Data->qpos[Adr + 4] = AppliedRot.X;
			Data->qpos[Adr + 5] = AppliedRot.Y;
			Data->qpos[Adr + 6] = AppliedRot.Z;
		}

		int32 Cursor = 0;
		for (const FQposLayout::FSlot& Slot : Layout.NonRootSlots)
		{
			for (int32 Idx = 0; Idx < Slot.Size; ++Idx)
			{
				float Value = (JointQpos && JointQpos->IsValidIndex(Cursor)) ? (*JointQpos)[Cursor] : 0.0f;
				Data->qpos[Slot.Adr + Idx] = static_cast<mjtNum>(Value);
				AppliedJoint.Add(Value);
				++Cursor;
			}
		}

		for (UMjJoint* Joint : Articulation->GetJoints())
		{
			if (!Joint) continue;
			int32 JointId = Joint->GetMjID();
			if (JointId < 0 || JointId >= Model->njnt) continue;

			int32 DofAdr = Model->jnt_dofadr[JointId];
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

	Result.bOk = true;
	Result.AppliedTranslation = AppliedTrans;
	Result.AppliedRotation = AppliedRot;
	Result.AppliedJointQpos = MoveTemp(AppliedJoint);
	Result.SimTime = Data->time;
	return Result;
}

FURSPoseResult UURSRobotCoreComponent::GetPose(const FString& ActorId) const
{
	FURSPoseResult Result;
	Result.bOk = false;

	const FRobotEndpoint* Ep = FindEndpoint(ActorId);
	if (!Ep)
	{
		Result.Error = TEXT("not_found");
		return Result;
	}

	AMjArticulation* Articulation = Ep->Articulation.Get();
	AAMjManager* ManagerPtr = Manager.Get();
	if (!Articulation || !ManagerPtr || !ManagerPtr->PhysicsEngine)
	{
		Result.Error = TEXT("not_ready");
		return Result;
	}

	mjModel* Model = ManagerPtr->PhysicsEngine->GetModel();
	mjData* Data = ManagerPtr->PhysicsEngine->GetData();
	if (!Model || !Data)
	{
		Result.Error = TEXT("not_ready");
		return Result;
	}

	FQposLayout Layout = DiscoverQposLayout(Articulation, Model);
	int32 RootBodyId = DiscoverRootBodyId(Articulation, Model);

	{
		FScopeLock Lock(&ManagerPtr->PhysicsEngine->CallbackMutex);

		if (RootBodyId > 0 && RootBodyId < Model->nbody)
		{
			const mjtNum* Xpos = Data->xpos + RootBodyId * 3;
			const mjtNum* Xquat = Data->xquat + RootBodyId * 4;
			Result.AppliedTranslation = FVector(Xpos[0], Xpos[1], Xpos[2]);
			Result.AppliedRotation = FQuat(Xquat[1], Xquat[2], Xquat[3], Xquat[0]);
		}
		else if (!Layout.RootSlots.IsEmpty())
		{
			int32 Adr = Layout.RootSlots[0].Adr;
			Result.AppliedTranslation = FVector(Data->qpos[Adr], Data->qpos[Adr+1], Data->qpos[Adr+2]);
			Result.AppliedRotation = FQuat(Data->qpos[Adr+4], Data->qpos[Adr+5], Data->qpos[Adr+6], Data->qpos[Adr+3]);
		}

		for (const FQposLayout::FSlot& Slot : Layout.NonRootSlots)
		{
			for (int32 Idx = 0; Idx < Slot.Size; ++Idx)
			{
				Result.AppliedJointQpos.Add(static_cast<float>(Data->qpos[Slot.Adr + Idx]));
			}
		}

		Result.SimTime = Data->time;
	}

	Result.bOk = true;
	return Result;
}

FURSPoseResult UURSRobotCoreComponent::ResetRobot(const FString& ActorId)
{
	UURSSceneConfigComponent* SceneComp = Cast<UURSSceneConfigComponent>(SceneConfigComp.Get());

	FVector InitialTrans = FVector::ZeroVector;
	FQuat InitialRot = FQuat::Identity;
	bool bHaveInitial = false;

	if (SceneComp)
	{
		bHaveInitial = SceneComp->GetInitialPose(ActorId, InitialTrans, InitialRot);
	}

	FURSPoseResult Result;
	if (bHaveInitial)
	{
		Result = SetPose(ActorId, &InitialTrans, &InitialRot, nullptr);
	}
	else
	{
		Result = SetPose(ActorId, nullptr, nullptr, nullptr);
	}

	return Result;
}
