#include "Core/URSRobotCoreComponent.h"

#include "Scene/URSSceneConfigComponent.h"
#include "Scene/URSRobotTypeRegistry.h"
#include "Runtime/URSRobotNames.h"
#include "MuJoCo/Components/Actuators/MjActuator.h"
#include "MuJoCo/Components/Joints/MjJoint.h"
#include "MuJoCo/Components/Sensors/MjCamera.h"
#include "MuJoCo/Core/AMjManager.h"
#include "MuJoCo/Core/MjArticulation.h"
#include "EngineUtils.h"
#include "MuJoCo/Core/MjPhysicsEngine.h"
#include "MuJoCo/Utils/MjUtils.h"
#include "Transport/NetworkManager.h"
#include "TimerManager.h"

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
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(CompiledSceneRetryTimer);
	}
	Super::EndPlay(EndPlayReason);
}

void UURSRobotCoreComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFn)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFn);
	if (!bInitialized && bAutoStart)
	{
		Initialize();
	}
	int32 EndpointCount = 0;
	{
		// no lock needed;
		EndpointCount = Endpoints.Num();
	}
	if (bInitialized && EndpointCount == 0)
	{
		TryInitializeCompiledScene();
	}
	if (bInitialized && EndpointCount > 0)
	{
		// One-time diagnostic: verify actuator weak pointers are valid
		static bool bDiagDone = false;
		if (!bDiagDone)
		{
			bDiagDone = true;
			int32 ValidAct = 0, TotalAct = 0;
			for (const auto& Ep : Endpoints)
			{
				for (const auto& Ai : Ep.Actuators)
				{
					++TotalAct;
					if (Ai.Actuator.IsValid()) ++ValidAct;
				}
			}
			UE_LOG(LogTemp, Warning, TEXT("[URS DIAG] Endpoints=%d Actuators=%d/%d valid"),
				EndpointCount, ValidAct, TotalAct);
		}
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
	if (bInitialized)
	{
		TryInitializeCompiledScene();
		return true;
	}

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
		if (!SceneConfigComp.IsValid())
		{
			// Retry: search all actors
			for (TActorIterator<AActor> It(GetWorld()); It; ++It)
			{
				SceneConfigComp = It->FindComponentByClass<UURSSceneConfigComponent>();
				if (SceneConfigComp.IsValid()) break;
			}
		}
		UE_LOG(LogTemp, Warning, TEXT("[URS Core] SceneConfigComp valid=%d"), SceneConfigComp.IsValid());
	}

	RebuildEndpointCache();

	RegisterPhysicsCallbacks();
	if (!bCallbacksRegistered)
	{
		UE_LOG(LogTemp, Log, TEXT("[URS Core] Physics engine not ready; will retry next tick."));
		{
			// no lock needed;
			Endpoints.Reset();
		}
		return false;
	}

	if (UURSSceneConfigComponent* SceneComp = Cast<UURSSceneConfigComponent>(SceneConfigComp.Get()))
	{
		SceneComp->OnSceneConfigApplied.AddDynamic(this, &UURSRobotCoreComponent::OnSceneConfigApplied);
	}

	bInitialized = true;
	TryInitializeCompiledScene();
	{
		// no lock needed;
		UE_LOG(LogTemp, Log, TEXT("[URS Core] Initialized with %d robot(s)."), Endpoints.Num());
	}
	return true;
}

void UURSRobotCoreComponent::OnSceneConfigApplied()
{
	TryInitializeCompiledScene();
}

void UURSRobotCoreComponent::TryInitializeCompiledScene()
{
	AAMjManager* ManagerPtr = Manager.Get();
	UWorld* World = GetWorld();
	if (!ManagerPtr || !ManagerPtr->PhysicsEngine || !World)
	{
		return;
	}

	if (!ManagerPtr->PhysicsEngine->GetModel() || !ManagerPtr->PhysicsEngine->GetData())
	{
		World->GetTimerManager().SetTimer(
			CompiledSceneRetryTimer, this, &UURSRobotCoreComponent::TryInitializeCompiledScene,
			0.01f, false);
		return;
	}

	RebuildEndpointCache();
	int32 EndpointCount = 0;
	{
		// no lock needed;
		EndpointCount = Endpoints.Num();
	}
	if (EndpointCount == 0)
	{
		World->GetTimerManager().SetTimer(
			CompiledSceneRetryTimer, this, &UURSRobotCoreComponent::TryInitializeCompiledScene,
			0.01f, false);
		return;
	}

	InitializeConfiguredRobotPoses();
	InitializeConfiguredObjectPoses();
	OnRobotsChanged.Broadcast();
	UE_LOG(LogTemp, Log, TEXT("[URS Core] Initialized %d compiled robot endpoint(s)."), EndpointCount);
}

void UURSRobotCoreComponent::InitializeConfiguredRobotPoses()
{
	// A scene-config spawn transform places the UE actor, but a MuJoCo
	// freejoint has independent qpos state. The endpoint cache is populated
	// only after URLab compilation, so run this after every cache rebuild.
	TArray<FString> ActorIds;
	{
		// no lock needed;
		ActorIds.Reserve(Endpoints.Num());
		for (const FRobotEndpoint& Endpoint : Endpoints)
		{
			ActorIds.Add(Endpoint.ActorId);
		}
	}
	for (const FString& ActorId : ActorIds)
	{
		const FURSPoseResult PoseResult = ResetRobot(ActorId);
		if (!PoseResult.bOk)
		{
			UE_LOG(LogTemp, Warning, TEXT("[URS Core] Failed to initialize '%s' from scene config: %s"),
				*ActorId, *PoseResult.Error);
		}
	}
}

void UURSRobotCoreComponent::InitializeConfiguredObjectPoses()
{
	AAMjManager* ManagerPtr = Manager.Get();
	UURSSceneConfigComponent* SceneComp =
		Cast<UURSSceneConfigComponent>(SceneConfigComp.Get());
	if (!ManagerPtr || !ManagerPtr->PhysicsEngine || !SceneComp)
	{
		return;
	}

	mjModel* Model = ManagerPtr->PhysicsEngine->GetModel();
	mjData* Data = ManagerPtr->PhysicsEngine->GetData();
	if (!Model || !Data)
	{
		return;
	}

	TMap<FString, AMjArticulation*> Articulations;
	for (AMjArticulation* Articulation : ManagerPtr->GetAllArticulations())
	{
		if (Articulation && !Articulation->ActorId.IsEmpty())
		{
			Articulations.Add(Articulation->ActorId, Articulation);
		}
	}

	FScopeLock PhysicsLock(&ManagerPtr->PhysicsEngine->CallbackMutex);
	bool bChanged = false;
	for (const TPair<FString, FURSSpawnedObjectInfo>& Pair : SceneComp->GetSpawnedObjects())
	{
		AMjArticulation* const* Found = Articulations.Find(Pair.Key);
		if (!Found || !*Found)
		{
			UE_LOG(LogTemp, Warning, TEXT("[URS Core] Object '%s' not found in compiled scene."), *Pair.Key);
			continue;
		}

		int32 FreeJointId = -1;
		for (UMjJoint* Joint : (*Found)->GetJoints())
		{
			const int32 JointId = Joint ? Joint->GetMjID() : -1;
			if (JointId >= 0 && JointId < Model->njnt
				&& Model->jnt_type[JointId] == mjJNT_FREE)
			{
				FreeJointId = JointId;
				break;
			}
		}
		if (FreeJointId < 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("[URS Core] Object '%s' has no free root joint."), *Pair.Key);
			continue;
		}

		const FURSSpawnedObjectInfo& Info = Pair.Value;
		FQuat Rotation = Info.InitialRotationXyzw;
		Rotation.Normalize();
		const int32 QposAdr = Model->jnt_qposadr[FreeJointId];
		Data->qpos[QposAdr + 0] = Info.InitialTranslationMeters.X;
		Data->qpos[QposAdr + 1] = Info.InitialTranslationMeters.Y;
		Data->qpos[QposAdr + 2] = Info.InitialTranslationMeters.Z;
		Data->qpos[QposAdr + 3] = Rotation.W;
		Data->qpos[QposAdr + 4] = Rotation.X;
		Data->qpos[QposAdr + 5] = Rotation.Y;
		Data->qpos[QposAdr + 6] = Rotation.Z;
		const int32 DofAdr = Model->jnt_dofadr[FreeJointId];
		for (int32 Index = 0; Index < 6; ++Index)
		{
			Data->qvel[DofAdr + Index] = 0.0;
		}
		bChanged = true;
	}
	if (bChanged)
	{
		mj_forward(Model, Data);
	}
}

void UURSRobotCoreComponent::RebuildEndpointCache()
{
	AAMjManager* ManagerPtr = Manager.Get();
	if (!ManagerPtr) return;

	UURSSceneConfigComponent* SceneComp = Cast<UURSSceneConfigComponent>(SceneConfigComp.Get());
	TArray<FRobotEndpoint> NewEndpoints;
	const mjModel* Model = ManagerPtr->PhysicsEngine
		? ManagerPtr->PhysicsEngine->GetModel()
		: nullptr;

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

		// Allocate heap triple buffers (stable across array reallocation)
		Ep.StateBuffer = new URSTripleBuffer<FRobotSnapshot>();
		Ep.CmdBuffer = new URSTripleBuffer<FCommandSet>();
		Ep.GainBuffer = new URSTripleBuffer<FGainSet>();

		TArray<UMjActuator*> Actuators = Articulation->GetActuators();
		Actuators.RemoveAll([](UMjActuator* A) { return !A || A->GetMjID() < 0; });
		Actuators.Sort([](const UMjActuator& L, const UMjActuator& R) { return L.GetMjID() < R.GetMjID(); });

		for (UMjActuator* Actuator : Actuators)
		{
			FString CleanName = URSoccerLab::FRobotNames::NormalizeRobotComponentName(Actuator->GetMjName(), ActorId);
			FActuatorInfo Info;
			Info.Actuator = Actuator;
			Info.Name = CleanName;
			Info.MjId = Actuator->GetMjID();
			Ep.ActuatorNameToIndex.Add(CleanName, Ep.Actuators.Num());
			Ep.Actuators.Add(MoveTemp(Info));
		}
		// Initialize command buffer with zeros
		FCommandSet& InitCmd = Ep.CmdBuffer->Back();
		FMemory::Memzero(InitCmd.Targets, sizeof(InitCmd.Targets));
		InitCmd.TimestampSec = 0.0;
		InitCmd.bValid = false;
		Ep.CmdBuffer->Publish();

		TArray<UMjJoint*> Joints = Articulation->GetJoints();
		Joints.RemoveAll([](UMjJoint* J) { return !J || J->GetMjID() < 0; });
		Joints.Sort([](const UMjJoint& L, const UMjJoint& R) { return L.GetMjID() < R.GetMjID(); });

		for (UMjJoint* Joint : Joints)
		{
			FString CleanName = URSoccerLab::FRobotNames::NormalizeRobotComponentName(Joint->GetMjName(), ActorId);
			FJointInfo Info;
			Info.Joint = Joint;
			Info.Name = CleanName;
			Info.MjId = Joint->GetMjID();
			if (Model && Info.MjId >= 0 && Info.MjId < Model->njnt)
			{
				Info.JointType = Model->jnt_type[Info.MjId];
				Info.QposAdr = Model->jnt_qposadr[Info.MjId];
				const int32 QposEnd = (Info.MjId + 1 < Model->njnt)
					? Model->jnt_qposadr[Info.MjId + 1]
					: Model->nq;
				Info.QposSize = QposEnd - Info.QposAdr;
				Info.DofAdr = Model->jnt_dofadr[Info.MjId];
				const int32 DofEnd = (Info.MjId + 1 < Model->njnt)
					? Model->jnt_dofadr[Info.MjId + 1]
					: Model->nv;
				Info.DofSize = DofEnd - Info.DofAdr;
			}
			Ep.Joints.Add(MoveTemp(Info));
		}
		if (Model)
		{
			Ep.RootBodyId = DiscoverRootBodyId(Articulation, Model);
			const FQposLayout Layout = DiscoverQposLayout(Articulation, Model);
			if (!Layout.RootSlots.IsEmpty())
			{
				Ep.RootQposAdr = Layout.RootSlots[0].Adr;
			}
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
			if (!Cam->IsStreamingActive())
			{
				Cam->SetStreamingEnabled(true);
			}

			FCameraEntry Entry;
			Entry.Camera = Cam;
			Entry.Name = Cam->GetName();
			Ep.Cameras.Add(MoveTemp(Entry));
		}

		NewEndpoints.Add(MoveTemp(Ep));
	};

	if (SceneComp)
	{
		for (const URSoccerLab::FURSRobotSpawn& Spawn : SceneComp->GetActiveConfig().Robots)
		{
		AMjArticulation* const* Found = ArticulationsByName.Find(Spawn.ActorId);
		if (Found && *Found)
		{
			BuildEndpoint(*Found, Spawn.ActorId);
			if (NewEndpoints.Num() > 0)
			{
				NewEndpoints.Last().Privilege = Spawn.Privilege;
				NewEndpoints.Last().Noise = Spawn.Noise;
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[URS Core] Robot '%s' not found in world."), *Spawn.ActorId);
		}
		}
	}

	if (NewEndpoints.Num() == 0)
	{
		TSet<AMjArticulation*> Seen;
		for (auto& Pair : ArticulationsByName)
		{
			if (!Pair.Value || Seen.Contains(Pair.Value)) continue;
			Seen.Add(Pair.Value);
			BuildEndpoint(Pair.Value, Pair.Value->ActorId.IsEmpty() ? Pair.Value->GetName() : Pair.Value->ActorId);
		}
	}

	const int32 NewEndpointCount = NewEndpoints.Num();
	TMap<FString, int32> NewActorRootBodyIds;
	if (Model)
	{
		TSet<AMjArticulation*> Seen;
		for (const TPair<FString, AMjArticulation*>& Pair : ArticulationsByName)
		{
			AMjArticulation* Articulation = Pair.Value;
			if (!Articulation || Seen.Contains(Articulation)) continue;
			Seen.Add(Articulation);
			FString Id = Articulation->ActorId.IsEmpty() ? Articulation->GetName() : Articulation->ActorId;
			if (Id.IsEmpty()) continue;
			const int32 RootId = DiscoverRootBodyId(Articulation, Model);
			if (RootId > 0)
			{
				NewActorRootBodyIds.Add(Id, RootId);
			}
		}
	}
	{
		uint32 OldSeq = EndpointSeq.load(std::memory_order_relaxed);
		EndpointSeq.store(OldSeq | 1, std::memory_order_release);
		Endpoints = MoveTemp(NewEndpoints);
		ActorRootBodyIds = MoveTemp(NewActorRootBodyIds);
		EndpointSeq.store(OldSeq + 2, std::memory_order_release);
	}
	UE_LOG(LogTemp, Log, TEXT("[URS Core] Endpoint cache rebuilt: %d robot(s)."), NewEndpointCount);
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

void UURSRobotCoreComponent::PreStepPhysics(mjModel* Model, mjData* Data)
{
	const uint32 Seq1 = EndpointSeq.load(std::memory_order_acquire);
	if (Seq1 & 1) return;

	const int32 N = Endpoints.Num();
	for (int32 Ri = 0; Ri < N; ++Ri)
	{
		PublishSnapshot(Model, Data, Ri);
		ApplyGains(Model);
	}
	ApplyPoseLocks(Model, Data);

	// Apply commands + write ctrl directly (URLab ApplyControls may be skipped)
	const double NowSec = FPlatformTime::Seconds();
	for (FRobotEndpoint& Ep : Endpoints)
	{
		const FCommandSet& Cmd = Ep.CmdBuffer->Front();
		const bool bTimedOut = !Cmd.bValid ||
			(NowSec - Cmd.TimestampSec > CommandTimeoutSec);

		for (int32 Idx = 0; Idx < Ep.Actuators.Num(); ++Idx)
		{
			const int32 Am = Ep.Actuators[Idx].MjId;
			if (Am >= 0 && Am < Model->nu)
			{
				float Val = bTimedOut ? 0.0f :
					(Idx < URS_MAX_ACTUATORS ? Cmd.Targets[Idx] : 0.0f);
				Data->ctrl[Am] = (mjtNum)Val;
				if (UMjActuator* Act = Ep.Actuators[Idx].Actuator.Get())
					Act->SetNetworkControl(Val);
			}
		}
	}
}

void UURSRobotCoreComponent::PublishSnapshot(mjModel* Model, mjData* Data, int32 Ri)
{
	FRobotEndpoint& Ep = Endpoints[Ri];
	FRobotSnapshot& S = Ep.StateBuffer->Back();
	S = FRobotSnapshot{};

	S.SimTime = Data->time;
	const FCommandSet& Cmd = Ep.CmdBuffer->Front();
	S.bCommandTimedOut = !Cmd.bValid || (FPlatformTime::Seconds() - Cmd.TimestampSec > CommandTimeoutSec);

		// Base pose from body world transforms
		if (Ep.RootBodyId > 0)
		{
			const int32 P = Ep.RootBodyId * 3;
			const int32 Q = Ep.RootBodyId * 4;
			S.BasePos[0] = Data->xpos[P]; S.BasePos[1] = Data->xpos[P+1]; S.BasePos[2] = Data->xpos[P+2];
			S.BaseQuat[0] = Data->xquat[Q]; S.BaseQuat[1] = Data->xquat[Q+1];
			S.BaseQuat[2] = Data->xquat[Q+2]; S.BaseQuat[3] = Data->xquat[Q+3];
		}

		// Base velocity from free joint
		for (const FJointInfo& Ji : Ep.Joints)
		{
			if (Ji.JointType == mjJNT_FREE && Ji.DofAdr >= 0 && Ji.DofSize >= 6)
			{
				for (int32 V = 0; V < 6; ++V) S.BaseVel[V] = Data->qvel[Ji.DofAdr + V];
				break;
			}
		}

		// Non-root joints
		S.JointCount = 0;
		for (const FJointInfo& Ji : Ep.Joints)
		{
			if (Ji.JointType == mjJNT_FREE || Ji.QposAdr < 0) continue;
			const int32 N = FMath::Min(Ji.QposSize, Ji.DofSize > 0 ? Ji.DofSize : 1);
			for (int32 V = 0; V < N && S.JointCount < URS_MAX_JOINTS; ++V, ++S.JointCount)
			{
				S.JointQpos[S.JointCount] = Data->qpos[Ji.QposAdr + V];
				S.JointQvel[S.JointCount] = Data->qvel[Ji.DofAdr + V];
			}
		}

		// Actuator commands
		S.ActuatorCount = FMath::Min(Ep.Actuators.Num(), URS_MAX_ACTUATORS);
		for (int32 Ai = 0; Ai < S.ActuatorCount; ++Ai)
		{
			S.ActuatorCmds[Ai] = S.bCommandTimedOut ? 0.0 :
				(Ai < URS_MAX_ACTUATORS ? Cmd.Targets[Ai] : 0.0);
		}

		// Camera IMU
		if (Ep.HeadCameraBodyId > 0)
		{
			const int32 HB = Ep.HeadCameraBodyId;
			const int32 Q = HB * 4;
			const int32 V = HB * 6;
			S.bHasCameraImu = true;
			S.HeadQuat[0] = Data->xquat[Q]; S.HeadQuat[1] = Data->xquat[Q+1];
			S.HeadQuat[2] = Data->xquat[Q+2]; S.HeadQuat[3] = Data->xquat[Q+3];
			S.HeadAngVel[0] = Data->cvel[V]; S.HeadAngVel[1] = Data->cvel[V+1]; S.HeadAngVel[2] = Data->cvel[V+2];
		}

		// Privileged positions
		S.bPrivSelfPos = Ep.Privilege.bSelfPos;
		S.bPrivBallPosRelated = Ep.Privilege.bBallPosRelated;
		S.bPrivBallVelRelated = Ep.Privilege.bBallVelRelated;
		S.bPrivAllPos = Ep.Privilege.bAllPos;

		if (S.bPrivSelfPos)
		{
			S.SelfPos[0] = S.BasePos[0]; S.SelfPos[1] = S.BasePos[1]; S.SelfPos[2] = S.BasePos[2];
		}

		// Yaw-only frame for ball-relative
		double YawSin = 0, YawCos = 1;
		{
			const double W = S.BaseQuat[0], X = S.BaseQuat[1], Y = S.BaseQuat[2], Z = S.BaseQuat[3];
			const double Yaw = FMath::Atan2(2.0*(W*Z+X*Y), 1.0-2.0*(Y*Y+Z*Z));
			YawSin = FMath::Sin(Yaw); YawCos = FMath::Cos(Yaw);
		}
		auto WorldToYaw = [&](double Wx, double Wy, double Wz, double Dx, double Dy, double Dz) {
			const double Rx = Dx, Ry = Dy, Rz = Dz;
			return FVector(
				YawCos * Rx + YawSin * Ry,
				-YawSin * Rx + YawCos * Ry,
				Rz);
		};

		if (S.bPrivBallPosRelated || S.bPrivBallVelRelated)
		{
			const int32* BallPtr = ActorRootBodyIds.Find(TEXT("ball"));
			if (BallPtr)
			{
				const int32 BP = (*BallPtr) * 3;
				const double Bx = Data->xpos[BP], By = Data->xpos[BP+1], Bz = Data->xpos[BP+2];
				if (S.bPrivBallPosRelated)
				{
					const FVector Rel = WorldToYaw(Bx, By, Bz, Bx - S.BasePos[0], By - S.BasePos[1], Bz - S.BasePos[2]);
					S.BallPosRelated[0] = Rel.X; S.BallPosRelated[1] = Rel.Y; S.BallPosRelated[2] = Rel.Z;
				}
				if (S.bPrivBallVelRelated)
				{
					const int32 BV = (*BallPtr) * 6 + 3;
					const int32 BQ = (*BallPtr) * 4;
					FQuat BQ_(Data->xquat[BQ+1], Data->xquat[BQ+2], Data->xquat[BQ+3], Data->xquat[BQ]);
					FVector BVBody(Data->cvel[BV], Data->cvel[BV+1], Data->cvel[BV+2]);
					FVector BVWorld = BQ_.RotateVector(BVBody);
					const FVector Rel = WorldToYaw(Bx, By, Bz, BVWorld.X, BVWorld.Y, BVWorld.Z);
					S.BallVelRelated[0] = Rel.X; S.BallVelRelated[1] = Rel.Y; S.BallVelRelated[2] = Rel.Z;
				}
			}
		}

		if (S.bPrivAllPos)
		{
			S.ActorCount = 0;
			for (const auto& Pair : ActorRootBodyIds)
			{
				if (S.ActorCount >= URS_MAX_ACTORS) break;
				const int32 P = Pair.Value * 3;
				S.ActorPos[S.ActorCount][0] = Data->xpos[P];
				S.ActorPos[S.ActorCount][1] = Data->xpos[P+1];
			S.ActorPos[S.ActorCount][2] = Data->xpos[P+2];
			++S.ActorCount;
		}
	}

	Ep.StateBuffer->Publish();
}

void UURSRobotCoreComponent::ApplyPoseLocks(mjModel* Model, mjData* Data)
{
	for (FRobotEndpoint& Ep : Endpoints)
	{
		if (!Ep.PoseLock.bActive) continue;
		AMjArticulation* Articulation = Ep.Articulation.Get();
		if (!Articulation) continue;

		FQposLayout Layout = DiscoverQposLayout(Articulation, Model);

		if (!Layout.RootSlots.IsEmpty())
		{
			int32 Adr = Layout.RootSlots[0].Adr;
			Data->qpos[Adr+0] = Ep.PoseLock.Translation.X;
			Data->qpos[Adr+1] = Ep.PoseLock.Translation.Y;
			Data->qpos[Adr+2] = Ep.PoseLock.Translation.Z;
			Data->qpos[Adr+3] = Ep.PoseLock.Rotation.W;
			Data->qpos[Adr+4] = Ep.PoseLock.Rotation.X;
			Data->qpos[Adr+5] = Ep.PoseLock.Rotation.Y;
			Data->qpos[Adr+6] = Ep.PoseLock.Rotation.Z;
		}

		int32 Cursor = 0;
		for (const FQposLayout::FSlot& Slot : Layout.NonRootSlots)
		{
			for (int32 Idx = 0; Idx < Slot.Size; ++Idx)
			{
				float V = Ep.PoseLock.JointQpos.IsValidIndex(Cursor) ? Ep.PoseLock.JointQpos[Cursor] : 0.0f;
				Data->qpos[Slot.Adr + Idx] = V;
				++Cursor;
			}
		}

		for (UMjJoint* Joint : Articulation->GetJoints())
		{
			if (!Joint) continue;
			int32 JId = Joint->GetMjID();
			if (JId < 0 || JId >= Model->njnt) continue;
			int32 DofAdr = Model->jnt_dofadr[JId];
			int32 DofSize = (Model->jnt_type[JId] == mjJNT_FREE) ? 6 :
			                (Model->jnt_type[JId] == mjJNT_BALL) ? 3 : 1;
			for (int32 i = 0; i < DofSize; ++i)
				Data->qvel[DofAdr + i] = 0.0;
		}

		// Sync actuator targets so PD controllers don't fight
		FCommandSet& SyncCmd = Ep.CmdBuffer->Back();
		FMemory::Memzero(SyncCmd.Targets, sizeof(SyncCmd.Targets));
		for (int32 Ai = 0; Ai < Ep.Actuators.Num(); ++Ai)
		{
			int32 Am = Ep.Actuators[Ai].MjId;
			if (Am < 0 || Am >= Model->nu) continue;
			int32 Jm = Model->actuator_trnid[Am * 2];
			if (Jm < 0 || Jm >= Model->njnt) continue;
			int32 Qa = Model->jnt_qposadr[Jm];
			Data->ctrl[Am] = Data->qpos[Qa];
			SyncCmd.Targets[Ai] = static_cast<float>(Data->qpos[Qa]);
			if (UMjActuator* Act = Ep.Actuators[Ai].Actuator.Get())
				Act->SetNetworkControl(static_cast<float>(Data->qpos[Qa]));
		}
		SyncCmd.TimestampSec = FPlatformTime::Seconds();
		SyncCmd.bValid = true;
		Ep.CmdBuffer->Publish();

		mj_forward(Model, Data);
	}
}

FURSPoseResult UURSRobotCoreComponent::SetPoseLock(const FString& ActorId, bool bLock,
	const FVector* Trans, const FQuat* Rot, const TArray<float>* JointQpos)
{
	FURSPoseResult Result;
	AAMjManager* ManagerPtr = Manager.Get();
	if (!ManagerPtr || !ManagerPtr->PhysicsEngine)
	{
		Result.Error = TEXT("not_ready");
		Result.Message = TEXT("Physics engine is not ready");
		return Result;
	}

	// ApplyPoseLocks is itself called while the worker owns CallbackMutex.
	// Serialize the game-thread update here instead of recursively taking
	// the non-recursive mutex inside the physics callback.
	FScopeLock PhysicsLock(&ManagerPtr->PhysicsEngine->CallbackMutex);
	// no lock needed;
	FRobotEndpoint* Ep = FindEndpoint(ActorId);
	if (!Ep)
	{
		Result.Error = TEXT("not_found");
		Result.Message = FString::Printf(TEXT("Robot '%s' not found"), *ActorId);
		return Result;
	}
	if (bLock)
	{
		Ep->PoseLock.bActive = true;
		if (Trans) Ep->PoseLock.Translation = *Trans;
		if (Rot) Ep->PoseLock.Rotation = *Rot;
		if (JointQpos) Ep->PoseLock.JointQpos = *JointQpos;
	}
	else
	{
		Ep->PoseLock.bActive = false;
	}
	Result.bOk = true;
	return Result;
}

void UURSRobotCoreComponent::ApplyCommands(double NowSec)
{
	static constexpr int32 MAX_CMD = 64;
	static thread_local float CmdBuf[MAX_CMD];

	for (FRobotEndpoint& Ep : Endpoints)
	{
		const FCommandSet& Cmd = Ep.CmdBuffer->Front();
		const bool bTimedOut = !Cmd.bValid ||
			(NowSec - Cmd.TimestampSec > CommandTimeoutSec);
		const int32 Count = FMath::Min(Ep.Actuators.Num(), MAX_CMD);

		for (int32 i = 0; i < Count; ++i)
			CmdBuf[i] = bTimedOut ? 0.0f : Cmd.Targets[i];

		// Write directly to mjData->ctrl (bypass URLab ApplyControls which may be skipped)
		for (int32 Idx = 0; Idx < Count; ++Idx)
		{
			const int32 Am = Ep.Actuators[Idx].MjId;
			if (Am >= 0)
			{
				// Still set NetworkControl for state reporting
				if (UMjActuator* Actuator = Ep.Actuators[Idx].Actuator.Get())
					Actuator->SetNetworkControl(CmdBuf[Idx]);
			}
		}
	}
}

void UURSRobotCoreComponent::ApplyGains(mjModel* Model)
{
	for (FRobotEndpoint& Ep : Endpoints)
	{
		const FGainSet& G = Ep.GainBuffer->Front();
		if (!G.bValid) continue;

		const bool bTorque = (G.Mode == 1);

		for (int32 Idx = 0; Idx < Ep.Actuators.Num(); ++Idx)
		{
			const int32 Am = Ep.Actuators[Idx].MjId;
			if (Am < 0 || Am >= Model->nu) continue;
			if (Idx >= URS_MAX_ACTUATORS) continue;

			mjtNum* gp = Model->actuator_gainprm + Am * mjNGAIN;
			mjtNum* bp = Model->actuator_biasprm + Am * mjNBIAS;

			if (bTorque)
			{
				gp[0] = 1.0;
				bp[1] = 0.0;
				bp[2] = 0.0;
			}
			else
			{
				gp[0] = G.Kp[Idx];
				bp[1] = -G.Kp[Idx];
				bp[2] = -(G.Kv[Idx] + G.Damping[Idx]);
			}
		}

		// Do NOT clear the gain buffer — gains persist in mjModel until
		// overwritten by a new GainSet from the network thread.
	}
}

TArray<FString> UURSRobotCoreComponent::GetRobotIds() const
{
	TArray<FString> Ids;
	Ids.Reserve(Endpoints.Num());
	for (const FRobotEndpoint& Ep : Endpoints)
		Ids.Add(Ep.ActorId);
	return Ids;
}

bool UURSRobotCoreComponent::GetRobotState(const FString& ActorId, FURSRobotState& OutState)
{
	int32 Ri = FindEndpointIndex(ActorId);
	if (Ri == INDEX_NONE) return false;

	const FRobotSnapshot& S = Endpoints[Ri].StateBuffer->Front();

	// Dynamic data from snapshot (lock-free read)
	OutState = FURSRobotState();
	OutState.ActorId = ActorId;
	OutState.SimTime = S.SimTime;
	OutState.bCommandTimedOut = S.bCommandTimedOut;

	OutState.BasePos = FVector(S.BasePos[0], S.BasePos[1], S.BasePos[2]);
	OutState.BaseQuat = FQuat(S.BaseQuat[1], S.BaseQuat[2], S.BaseQuat[3], S.BaseQuat[0]);
	for (int32 i = 0; i < 6 && i < 6; ++i) OutState.BaseVel.Add(S.BaseVel[i]);

	for (int32 i = 0; i < S.JointCount; ++i)
	{
		OutState.JointQpos.Add(S.JointQpos[i]);
		OutState.JointQvel.Add(S.JointQvel[i]);
	}

	for (int32 i = 0; i < S.ActuatorCount; ++i)
	{
		OutState.MotorCommand.Add(S.ActuatorCmds[i]);
	}

	OutState.bHasCameraImu = S.bHasCameraImu;
	if (S.bHasCameraImu)
	{
		OutState.HeadQuat = FQuat(S.HeadQuat[1], S.HeadQuat[2], S.HeadQuat[3], S.HeadQuat[0]);
		OutState.HeadAngVel = FVector(S.HeadAngVel[0], S.HeadAngVel[1], S.HeadAngVel[2]);
	}

	OutState.bPrivSelfPos = S.bPrivSelfPos;
	OutState.bPrivBallPosRelated = S.bPrivBallPosRelated;
	OutState.bPrivBallVelRelated = S.bPrivBallVelRelated;
	OutState.bPrivAllPos = S.bPrivAllPos;
	OutState.SelfPos = FVector(S.SelfPos[0], S.SelfPos[1], S.SelfPos[2]);
	OutState.BallPosRelated = FVector(S.BallPosRelated[0], S.BallPosRelated[1], S.BallPosRelated[2]);
	OutState.BallVelRelated = FVector(S.BallVelRelated[0], S.BallVelRelated[1], S.BallVelRelated[2]);

	// Static data from endpoint (brief lock)
	{
		// no lock needed;
		const FRobotEndpoint* Ep = FindEndpoint(ActorId);
		if (!Ep) return false;

		// Resolve head camera body (lazy, one-time)
		if (Ep->HeadCameraBodyId < 0 && Ep->RootBodyId > 0)
		{
			if (AAMjManager* Mgr = Manager.Get())
			{
				if (const mjModel* Mm = Mgr->PhysicsEngine ? Mgr->PhysicsEngine->GetModel() : nullptr)
				{
					auto InTree = [Mm](int32 b, int32 root) -> bool {
						for (int32 p = b; p > 0; p = Mm->body_parentid[p]) if (p == root) return true;
						return false; };
					int32 Fb = -1;
					for (int32 b = 1; b < Mm->nbody; ++b)
					{
						if (!InTree(b, Ep->RootBodyId)) continue;
						const char* Bn = mj_id2name(Mm, mjOBJ_BODY, b);
						if (!Bn) continue;
						FString Norm = URSoccerLab::FRobotNames::NormalizeRobotComponentName(FString(Bn), Ep->ActorId);
						if (Norm.EndsWith(TEXT("head_link")) || Norm.EndsWith(TEXT("head_pitch_link")))
						{ const_cast<FRobotEndpoint*>(Ep)->HeadCameraBodyId = b; break; }
						if (Fb < 0 && Norm.Contains(TEXT("head")) && !Norm.Contains(TEXT("neck")) && !Norm.Contains(TEXT("yaw")))
							Fb = b;
					}
					if (Ep->HeadCameraBodyId < 0) const_cast<FRobotEndpoint*>(Ep)->HeadCameraBodyId = Fb;
				}
			}
		}

		for (const FJointInfo& Ji : Ep->Joints)
			if (Ji.JointType != mjJNT_FREE) OutState.JointNames.Add(Ji.Name);

		for (const FActuatorInfo& Ai : Ep->Actuators)
			OutState.ActuatorNames.Add(Ai.Name);

		OutState.Noise = Ep->Noise;

		for (const FCameraEntry& Ce : Ep->Cameras)
		{
			if (const UMjCamera* Cam = Ce.Camera.Get())
			{
				FURSCameraInfo Ci;
				Ci.Name = Ce.Name;
				Ci.Format = Cam->CaptureMode == EMjCameraMode::Depth ? TEXT("float32_depth") : TEXT("bgra8");
				Ci.Width = Cam->resolution.Num() > 0 ? Cam->resolution[0] : 0;
				Ci.Height = Cam->resolution.Num() > 1 ? Cam->resolution[1] : 0;
				OutState.Cameras.Add(MoveTemp(Ci));
			}
		}

		if (S.bPrivAllPos && S.ActorCount > 0)
		{
			int32 Ai = 0;
			for (const auto& Pair : ActorRootBodyIds)
			{
				if (Ai >= S.ActorCount) break;
				OutState.AllPos.Add(Pair.Key, FVector(S.ActorPos[Ai][0], S.ActorPos[Ai][1], S.ActorPos[Ai][2]));
				++Ai;
			}
		}
	}

	return true;
}

void UURSRobotCoreComponent::SubmitCommand(const FString& ActorId, const TMap<FString, float>& NamedValues)
{
	FRobotEndpoint* Ep = FindEndpoint(ActorId);
	if (!Ep) return;
	FCommandSet& Cmd = Ep->CmdBuffer->Back();
	const FCommandSet& Old = Ep->CmdBuffer->Front(); // preserve existing targets
	FMemory::Memcpy(Cmd.Targets, Old.Targets, sizeof(Cmd.Targets));
	for (const auto& Pair : NamedValues)
	{
		if (const int32* Idx = Ep->ActuatorNameToIndex.Find(Pair.Key))
			if (*Idx < URS_MAX_ACTUATORS && FMath::IsFinite(Pair.Value))
				Cmd.Targets[*Idx] = Pair.Value;
	}
	Cmd.TimestampSec = FPlatformTime::Seconds();
	Cmd.bValid = true;
	Ep->CmdBuffer->Publish();
}

void UURSRobotCoreComponent::SubmitControllerParams(
	const FString& ActorId,
	const TMap<FString, double>* Kp,
	const TMap<FString, double>* Kv,
	const TMap<FString, double>* Damping,
	const FString* ActuatorMode)
{
	FRobotEndpoint* Ep = FindEndpoint(ActorId);
	if (!Ep) return;
	FGainSet& G = Ep->GainBuffer->Back();
	G = FGainSet{};
	const int32 N = FMath::Min(Ep->Actuators.Num(), URS_MAX_ACTUATORS);
	for (int32 i = 0; i < N; ++i)
	{
		const FString& Name = Ep->Actuators[i].Name;
		if (Kp)      G.Kp[i]      = Kp->FindRef(Name);
		if (Kv)      G.Kv[i]      = Kv->FindRef(Name);
		if (Damping) G.Damping[i] = Damping->FindRef(Name);
	}
	if (ActuatorMode)
		G.Mode = (*ActuatorMode == TEXT("torque")) ? 1 : 0;
	G.bValid = true;
	Ep->GainBuffer->Publish();
}


int32 UURSRobotCoreComponent::FindEndpointIndex(const FString& ActorId) const
{
	for (int32 i = 0; i < Endpoints.Num(); ++i)
		if (Endpoints[i].ActorId == ActorId) return i;
	return INDEX_NONE;
}

bool UURSRobotCoreComponent::RequestCameraReadback(const FString& ActorId)
{
	// no lock needed;
	FRobotEndpoint* Ep = FindEndpoint(ActorId);
	if (!Ep) return false;

	bool bAnyRequested = false;
	for (FCameraEntry& CamEntry : Ep->Cameras)
	{
		if (UMjCamera* Cam = CamEntry.Camera.Get())
		{
			if (!CamEntry.bReadbackRequested && Cam->IsReadbackReady())
			{
				if (Cam->CaptureMode == EMjCameraMode::Depth)
				{
					Cam->ConsumeFloatPixels();
				}
				else
				{
					Cam->ConsumePixels();
				}
			}
			Cam->RequestReadback();
			CamEntry.bReadbackRequested = true;
			bAnyRequested = true;
		}
	}
	return bAnyRequested;
}

bool UURSRobotCoreComponent::RequestNamedCameraReadback(const FString& ActorId, const FString& CameraName)
{
	// no lock needed;
	FRobotEndpoint* Ep = FindEndpoint(ActorId);
	if (!Ep) return false;

	for (FCameraEntry& CamEntry : Ep->Cameras)
	{
		if (CamEntry.Name != CameraName) continue;

		UMjCamera* Cam = CamEntry.Camera.Get();
		if (!Cam) return false;
		if (CamEntry.bReadbackRequested)
		{
			return false;
		}
		if (Cam->IsReadbackReady())
		{
			if (Cam->CaptureMode == EMjCameraMode::Depth)
			{
				Cam->ConsumeFloatPixels();
			}
			else
			{
				Cam->ConsumePixels();
			}
		}
		Cam->RequestReadback();
		CamEntry.bReadbackRequested = true;
		return true;
	}
	return false;
}

bool UURSRobotCoreComponent::IsCameraFrameReady(const FString& ActorId, const FString& CameraName) const
{
	// no lock needed;
	const FRobotEndpoint* Ep = FindEndpoint(ActorId);
	if (!Ep) return false;

	for (const FCameraEntry& CamEntry : Ep->Cameras)
	{
		if (CamEntry.Name == CameraName)
		{
			const UMjCamera* Cam = CamEntry.Camera.Get();
			return CamEntry.bReadbackRequested && Cam && Cam->IsReadbackReady();
		}
	}
	return false;
}

bool UURSRobotCoreComponent::ConsumeCameraFrame(const FString& ActorId, const FString& CameraName, TArray<FColor>& OutPixels)
{
	// no lock needed;
	FRobotEndpoint* Ep = FindEndpoint(ActorId);
	if (!Ep) return false;

	for (FCameraEntry& CamEntry : Ep->Cameras)
	{
		if (CamEntry.Name != CameraName) continue;

		UMjCamera* Cam = CamEntry.Camera.Get();
		if (!Cam) return false;
		if (Cam->CaptureMode == EMjCameraMode::Depth) return false;

		if (!CamEntry.bReadbackRequested) return false;
		if (!Cam->IsReadbackReady()) return false;

		OutPixels = Cam->ConsumePixels();
		CamEntry.bReadbackRequested = false;
		return OutPixels.Num() > 0;
	}
	return false;
}

bool UURSRobotCoreComponent::ConsumeDepthCameraFrame(
	const FString& ActorId,
	const FString& CameraName,
	TArray<float>& OutDepthMeters)
{
	// no lock needed;
	FRobotEndpoint* Ep = FindEndpoint(ActorId);
	if (!Ep) return false;

	for (FCameraEntry& CamEntry : Ep->Cameras)
	{
		if (CamEntry.Name != CameraName) continue;

		UMjCamera* Cam = CamEntry.Camera.Get();
		if (!Cam || Cam->CaptureMode != EMjCameraMode::Depth) return false;
		if (!CamEntry.bReadbackRequested || !Cam->IsReadbackReady()) return false;

		OutDepthMeters = Cam->ConsumeFloatPixels();
		CamEntry.bReadbackRequested = false;
		// SceneCapture SCS_SceneDepth is expressed in UE world units
		// (centimetres). The transport protocol standardises on metres.
		for (float& Depth : OutDepthMeters)
		{
			Depth *= 0.01f;
		}
		return OutDepthMeters.Num() > 0;
	}
	return false;
}

UURSRobotCoreComponent::FQposLayout UURSRobotCoreComponent::DiscoverQposLayout(AMjArticulation* Articulation, const mjModel* Model)
{
	FQposLayout Layout;
	if (!Articulation || !Model) return Layout;

	TArray<UMjJoint*> Joints = Articulation->GetJoints();
	Joints.RemoveAll([](UMjJoint* Joint) { return !Joint || Joint->GetMjID() < 0; });
	Joints.Sort([](const UMjJoint& L, const UMjJoint& R) { return L.GetMjID() < R.GetMjID(); });

	for (UMjJoint* Joint : Joints)
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

	AAMjManager* ManagerPtr = Manager.Get();
	if (!ManagerPtr || !ManagerPtr->PhysicsEngine)
	{
		Result.Error = TEXT("not_ready");
		Result.Message = TEXT("Physics engine not ready");
		return Result;
	}

	FScopeLock PhysicsLock(&ManagerPtr->PhysicsEngine->CallbackMutex);
	// no lock needed;
	FRobotEndpoint* Ep = FindEndpoint(ActorId);
	if (!Ep)
	{
		Result.Error = TEXT("not_found");
		Result.Message = FString::Printf(TEXT("Robot '%s' not found"), *ActorId);
		return Result;
	}

	AMjArticulation* Articulation = Ep->Articulation.Get();
	if (!Articulation)
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

	if (!FMath::IsFinite(AppliedTrans.X) || !FMath::IsFinite(AppliedTrans.Y) || !FMath::IsFinite(AppliedTrans.Z))
	{
		Result.Error = TEXT("invalid_translation");
		Result.Message = TEXT("translation contains NaN or infinity");
		return Result;
	}

	if (!FMath::IsFinite(AppliedRot.X) || !FMath::IsFinite(AppliedRot.Y)
		|| !FMath::IsFinite(AppliedRot.Z) || !FMath::IsFinite(AppliedRot.W))
	{
		Result.Error = TEXT("invalid_rotation");
		Result.Message = TEXT("rotation quaternion contains NaN or infinity");
		return Result;
	}

	const double QuatLenSq = AppliedRot.X * AppliedRot.X + AppliedRot.Y * AppliedRot.Y
		+ AppliedRot.Z * AppliedRot.Z + AppliedRot.W * AppliedRot.W;
	if (QuatLenSq < KINDA_SMALL_NUMBER)
	{
		Result.Error = TEXT("invalid_rotation");
		Result.Message = TEXT("rotation quaternion is zero-length");
		return Result;
	}
	if (FMath::Abs(QuatLenSq - 1.0) > KINDA_SMALL_NUMBER)
	{
		AppliedRot.Normalize();
	}

	TArray<float> AppliedJoint;
	AppliedJoint.Reserve(Layout.NonRootQposDim);

	if (JointQpos)
	{
		for (int32 i = 0; i < JointQpos->Num(); ++i)
		{
			if (!FMath::IsFinite((*JointQpos)[i]))
			{
				Result.Error = TEXT("invalid_joint_qpos");
				Result.Message = FString::Printf(TEXT("joint_qpos[%d] is NaN or infinity"), i);
				return Result;
			}
		}
	}

	{
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

		// Sync actuator controls to match joint angles so PD controllers
		// don't fight the new pose.  Must be inside the lock AND before
		// mj_forward so that derived quantities are consistent.
		if (JointQpos)
		{
			FCommandSet& SyncCmd = Ep->CmdBuffer->Back();
			FMemory::Memzero(SyncCmd.Targets, sizeof(SyncCmd.Targets));
			for (int32 ActIdx = 0; ActIdx < Ep->Actuators.Num(); ++ActIdx)
			{
				int32 ActMjId = Ep->Actuators[ActIdx].MjId;
				if (ActMjId < 0 || ActMjId >= Model->nu) continue;
				int32 JointMjId = Model->actuator_trnid[ActMjId * 2];
				if (JointMjId < 0 || JointMjId >= Model->njnt) continue;
				int32 QposAdr = Model->jnt_qposadr[JointMjId];
				float Target = static_cast<float>(Data->qpos[QposAdr]);
				Data->ctrl[ActMjId] = Target;
				SyncCmd.Targets[ActIdx] = Target;
				if (UMjActuator* Act = Ep->Actuators[ActIdx].Actuator.Get())
					Act->SetNetworkControl(Target);
			}
			SyncCmd.TimestampSec = FPlatformTime::Seconds();
			SyncCmd.bValid = true;
			Ep->CmdBuffer->Publish();
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

	AAMjManager* ManagerPtr = Manager.Get();
	if (!ManagerPtr || !ManagerPtr->PhysicsEngine)
	{
		Result.Error = TEXT("not_ready");
		return Result;
	}

	FScopeLock PhysicsLock(&ManagerPtr->PhysicsEngine->CallbackMutex);
	// no lock needed;
	const FRobotEndpoint* Ep = FindEndpoint(ActorId);
	if (!Ep)
	{
		Result.Error = TEXT("not_found");
		return Result;
	}

	AMjArticulation* Articulation = Ep->Articulation.Get();
	if (!Articulation)
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
	FURSPoseResult Result;
	Result.bOk = false;
	if (!SceneComp)
	{
		Result.Error = TEXT("not_ready");
		Result.Message = TEXT("scene config is unavailable");
		return Result;
	}

	const URSoccerLab::FURSRobotSpawn* Spawn =
		SceneComp->GetActiveConfig().Robots.FindByPredicate(
			[&ActorId](const URSoccerLab::FURSRobotSpawn& Candidate)
			{
				return Candidate.ActorId == ActorId;
			});
	if (!Spawn)
	{
		// Scene objects are MuJoCo articulations too, but intentionally are not
		// robot command/state endpoints. Let the same admin reset operation put
		// movable objects such as the ball back at their configured pose.
		const FURSSpawnedObjectInfo* ObjectInfo =
			SceneComp->GetSpawnedObjects().Find(ActorId);
		AAMjManager* ManagerPtr = Manager.Get();
		if (!ObjectInfo || !ManagerPtr || !ManagerPtr->PhysicsEngine)
		{
			Result.Error = ObjectInfo ? TEXT("not_ready") : TEXT("not_found");
			Result.Message = FString::Printf(
				TEXT("scene actor '%s' is unavailable"), *ActorId);
			return Result;
		}

		AMjArticulation* ObjectArticulation = nullptr;
		for (AMjArticulation* Articulation : ManagerPtr->GetAllArticulations())
		{
			if (Articulation
				&& (Articulation->ActorId == ActorId || Articulation->GetName() == ActorId))
			{
				ObjectArticulation = Articulation;
				break;
			}
		}
		if (!ObjectArticulation)
		{
			Result.Error = TEXT("not_ready");
			Result.Message = FString::Printf(
				TEXT("scene object '%s' is not compiled"), *ActorId);
			return Result;
		}

		FScopeLock PhysicsLock(&ManagerPtr->PhysicsEngine->CallbackMutex);
		mjModel* Model = ManagerPtr->PhysicsEngine->GetModel();
		mjData* Data = ManagerPtr->PhysicsEngine->GetData();
		if (!Model || !Data)
		{
			Result.Error = TEXT("not_ready");
			Result.Message = TEXT("mjModel/mjData missing");
			return Result;
		}

		int32 FreeJointId = -1;
		for (UMjJoint* Joint : ObjectArticulation->GetJoints())
		{
			const int32 JointId = Joint ? Joint->GetMjID() : -1;
			if (JointId >= 0 && JointId < Model->njnt
				&& Model->jnt_type[JointId] == mjJNT_FREE)
			{
				FreeJointId = JointId;
				break;
			}
		}
		if (FreeJointId < 0)
		{
			Result.Error = TEXT("fixed_base");
			Result.Message = FString::Printf(
				TEXT("scene object '%s' has no free root joint"), *ActorId);
			return Result;
		}

		FQuat Rotation = ObjectInfo->InitialRotationXyzw;
		Rotation.Normalize();
		const FVector Translation = ObjectInfo->InitialTranslationMeters;
		const int32 QposAdr = Model->jnt_qposadr[FreeJointId];
		Data->qpos[QposAdr + 0] = Translation.X;
		Data->qpos[QposAdr + 1] = Translation.Y;
		Data->qpos[QposAdr + 2] = Translation.Z;
		Data->qpos[QposAdr + 3] = Rotation.W;
		Data->qpos[QposAdr + 4] = Rotation.X;
		Data->qpos[QposAdr + 5] = Rotation.Y;
		Data->qpos[QposAdr + 6] = Rotation.Z;
		const int32 DofAdr = Model->jnt_dofadr[FreeJointId];
		for (int32 Index = 0; Index < 6; ++Index)
		{
			Data->qvel[DofAdr + Index] = 0.0;
		}
		mj_forward(Model, Data);

		Result.bOk = true;
		Result.AppliedTranslation = Translation;
		Result.AppliedRotation = Rotation;
		Result.SimTime = Data->time;
		return Result;
	}

	bool bHasEndpoint = false;
	TArray<FString> NonRootJointNames;
	{
		// no lock needed;
		if (const FRobotEndpoint* Endpoint = FindEndpoint(ActorId))
		{
			bHasEndpoint = true;
			NonRootJointNames.Reserve(Endpoint->Joints.Num());
			for (const FJointInfo& Joint : Endpoint->Joints)
			{
				if (Joint.JointType != mjJNT_FREE)
				{
					NonRootJointNames.Add(Joint.Name);
				}
			}
		}
	}

	if (!bHasEndpoint)
	{
		Result.Error = TEXT("not_ready");
		Result.Message = TEXT("robot endpoint is unavailable");
		return Result;
	}

	URSoccerLab::FURSRobotTypeRegistry& Registry = URSoccerLab::FURSRobotTypeRegistry::Get();
	Registry.RegisterDefaultTypes();
	const URSoccerLab::FURSRobotType* RobotType = Registry.Find(Spawn->Type);
	if (!RobotType)
	{
		Result.Error = TEXT("unknown_robot_type");
		Result.Message = FString::Printf(TEXT("unknown robot type '%s'"), *Spawn->Type);
		return Result;
	}

	FVector InitialTrans = FVector::ZeroVector;
	FQuat InitialRot = FQuat::Identity;
	if (!SceneComp->GetInitialPose(ActorId, InitialTrans, InitialRot))
	{
		Result.Error = TEXT("missing_initial_pose");
		Result.Message = FString::Printf(TEXT("robot '%s' has no configured initial pose"), *ActorId);
		return Result;
	}

	if (!Spawn->JointPositionsRad.IsSet())
	{
		return SetPose(ActorId, &InitialTrans, &InitialRot, nullptr);
	}

	const TMap<FString, float>& ConfiguredJointPositions = Spawn->JointPositionsRad.GetValue();
	TArray<float> InitialJointQpos;
	InitialJointQpos.Reserve(NonRootJointNames.Num());
	for (const FString& JointName : NonRootJointNames)
	{
		const float* Position = ConfiguredJointPositions.Find(JointName);
		if (!Position)
		{
			Result.Error = TEXT("incomplete_initial_joint_positions");
			Result.Message = FString::Printf(
				TEXT("robot '%s' is missing initial position for joint '%s'"),
				*ActorId,
				*JointName);
			return Result;
		}
		InitialJointQpos.Add(*Position);
	}

	if (InitialJointQpos.Num() != ConfiguredJointPositions.Num())
	{
		Result.Error = TEXT("unknown_initial_joint_position");
		Result.Message = FString::Printf(TEXT("robot '%s' contains an unknown initial joint position"), *ActorId);
		return Result;
	}

	return SetPose(ActorId, &InitialTrans, &InitialRot, &InitialJointQpos);
}
