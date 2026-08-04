#include "Core/URSRobotCoreComponent.h"

#include "Scene/URSSceneConfigComponent.h"
#include "Scene/URSRobotTypeRegistry.h"
#include "Runtime/URSRobotNames.h"
#include "MuJoCo/Components/Actuators/MjActuator.h"
#include "MuJoCo/Components/Joints/MjJoint.h"
#include "MuJoCo/Components/Sensors/MjCamera.h"
#include "MuJoCo/Core/AMjManager.h"
#include "MuJoCo/Core/MjArticulation.h"
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
		FScopeLock Lock(&EndpointMutex);
		EndpointCount = Endpoints.Num();
	}
	if (bInitialized && EndpointCount == 0)
	{
		TryInitializeCompiledScene();
	}
	if (bInitialized && EndpointCount > 0)
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
	}

	RebuildEndpointCache();

	RegisterPhysicsCallbacks();
	if (!bCallbacksRegistered)
	{
		UE_LOG(LogTemp, Log, TEXT("[URS Core] Physics engine not ready; will retry next tick."));
		{
			FScopeLock Lock(&EndpointMutex);
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
		FScopeLock Lock(&EndpointMutex);
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
		FScopeLock Lock(&EndpointMutex);
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
		FScopeLock Lock(&EndpointMutex);
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
		Ep.LatestCommand.Init(0.0f, Ep.Actuators.Num());

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
	{
		FScopeLock Lock(&EndpointMutex);
		Endpoints = MoveTemp(NewEndpoints);
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
	FScopeLock Lock(&EndpointMutex);
	ApplyPoseLocks(Model, Data);
	ApplyCommands(FPlatformTime::Seconds());
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
 				if (Ep.PoseLock.bLockJoints)
 				{
 					float V = Ep.PoseLock.JointQpos.IsValidIndex(Cursor) ? Ep.PoseLock.JointQpos[Cursor] : 0.0f;
 					Data->qpos[Slot.Adr + Idx] = V;
 				}
 				++Cursor;
 			}
 		}
 
 		if (Ep.PoseLock.bLockJoints)
 		{
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
 		}
 		else
 		{
 			// Base-only lock: zero freejoint velocity so the base stays
 			// pinned, but leave hinge joints free to move.
 			for (UMjJoint* Joint : Articulation->GetJoints())
 			{
 				if (!Joint) continue;
 				int32 JId = Joint->GetMjID();
 				if (JId < 0 || JId >= Model->njnt) continue;
 				if (Model->jnt_type[JId] != mjJNT_FREE) continue;
 				int32 DofAdr = Model->jnt_dofadr[JId];
 				for (int32 i = 0; i < 6; ++i)
 					Data->qvel[DofAdr + i] = 0.0;
 			}
 		}
 
 		if (Ep.PoseLock.bLockJoints)
 		{
 			// Sync actuator targets so PD controllers don't fight
 			for (int32 Ai = 0; Ai < Ep.Actuators.Num(); ++Ai)
 			{
 				int32 Am = Ep.Actuators[Ai].MjId;
 				if (Am < 0 || Am >= Model->nu) continue;
 				int32 Jm = Model->actuator_trnid[Am * 2];
 				if (Jm < 0 || Jm >= Model->njnt) continue;
 				int32 Qa = Model->jnt_qposadr[Jm];
 				Data->ctrl[Am] = Data->qpos[Qa];
 				Ep.LatestCommand[Ai] = static_cast<float>(Data->qpos[Qa]);
 				if (UMjActuator* Act = Ep.Actuators[Ai].Actuator.Get())
 					Act->SetNetworkControl(static_cast<float>(Data->qpos[Qa]));
 			}
 			Ep.LastCommandTimeSec = FPlatformTime::Seconds();
 			Ep.bHasCommand = true;
 		}

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
	FScopeLock EndpointLock(&EndpointMutex);
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
		if (JointQpos)
		{
			Ep->PoseLock.JointQpos = *JointQpos;
			Ep->PoseLock.bLockJoints = true;
		}
		else
		{
			Ep->PoseLock.bLockJoints = false;
		}
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
	FScopeLock Lock(&EndpointMutex);
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
	FScopeLock EndpointLock(&EndpointMutex);
	FRobotEndpoint* Ep = FindEndpoint(ActorId);
	if (!Ep) return false;

	AAMjManager* ManagerPtr = Manager.Get();
	if (!ManagerPtr || !ManagerPtr->PhysicsEngine) return false;

	const double NowSec = FPlatformTime::Seconds();

	OutState = FURSRobotState();
	OutState.ActorId = ActorId;
	OutState.bCommandTimedOut = !Ep->bHasCommand || (NowSec - Ep->LastCommandTimeSec > CommandTimeoutSec);

	bool bHaveSnapshot = false;
	ManagerPtr->PhysicsEngine->WithRenderState(
		[&](const FMjRenderSnapshot& Snapshot)
		{
			if (Snapshot.FrameId == 0)
			{
				return;
			}
			bHaveSnapshot = true;
			OutState.SimTime = Snapshot.SimTime;

			// Find root joint for base pose.
			if (Ep->RootBodyId > 0
				&& Snapshot.XPos.IsValidIndex(Ep->RootBodyId * 3 + 2)
				&& Snapshot.XQuat.IsValidIndex(Ep->RootBodyId * 4 + 3))
			{
				const int32 PosAdr = Ep->RootBodyId * 3;
				const int32 QuatAdr = Ep->RootBodyId * 4;
				OutState.BasePos = FVector(
					Snapshot.XPos[PosAdr],
					Snapshot.XPos[PosAdr + 1],
					Snapshot.XPos[PosAdr + 2]);
				OutState.BaseQuat = FQuat(
					Snapshot.XQuat[QuatAdr + 1],
					Snapshot.XQuat[QuatAdr + 2],
					Snapshot.XQuat[QuatAdr + 3],
					Snapshot.XQuat[QuatAdr]);
			}
			else if (Ep->RootQposAdr >= 0)
			{
				const int32 Adr = Ep->RootQposAdr;
				if (Snapshot.QPos.IsValidIndex(Adr + 6))
				{
					OutState.BasePos = FVector(
						Snapshot.QPos[Adr],
						Snapshot.QPos[Adr + 1],
						Snapshot.QPos[Adr + 2]);
					OutState.BaseQuat = FQuat(
						Snapshot.QPos[Adr + 4],
						Snapshot.QPos[Adr + 5],
						Snapshot.QPos[Adr + 6],
						Snapshot.QPos[Adr + 3]);
				}
			}

			// Base velocity from the free joint.
			for (const FJointInfo& JointInfo : Ep->Joints)
			{
				if (JointInfo.JointType != mjJNT_FREE
					|| JointInfo.DofAdr < 0
					|| JointInfo.DofSize < 6)
				{
					continue;
				}
				const int32 DofAdr = JointInfo.DofAdr;
				if (Snapshot.QVel.IsValidIndex(DofAdr + 5))
				{
					for (int32 Idx = 0; Idx < 6; ++Idx)
					{
						OutState.BaseVel.Add(Snapshot.QVel[DofAdr + Idx]);
					}
				}
				break;
			}

			// Joints (non-root only — free-joint data is in base).
			for (const FJointInfo& JointInfo : Ep->Joints)
			{
				if (JointInfo.JointType == mjJNT_FREE
					|| JointInfo.QposAdr < 0
					|| JointInfo.DofAdr < 0)
				{
					continue;
				}

				OutState.JointNames.Add(JointInfo.Name);
				const int32 QposBegin = JointInfo.QposAdr;
				const int32 QposEnd = QposBegin + JointInfo.QposSize;
				for (int32 Idx = QposBegin;
					Idx < QposEnd && Snapshot.QPos.IsValidIndex(Idx);
					++Idx)
				{
					OutState.JointQpos.Add(Snapshot.QPos[Idx]);
				}

				const int32 QvelBegin = JointInfo.DofAdr;
				const int32 QvelEnd = QvelBegin + JointInfo.DofSize;
				for (int32 Idx = QvelBegin;
					Idx < QvelEnd && Snapshot.QVel.IsValidIndex(Idx);
					++Idx)
				{
					OutState.JointQvel.Add(Snapshot.QVel[Idx]);
				}
			}
		});
	if (!bHaveSnapshot)
	{
		return false;
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
	FScopeLock Lock(&EndpointMutex);
	FRobotEndpoint* Ep = FindEndpoint(ActorId);
	if (!Ep) return;

	Ep->LastNamedValues = NamedValues;

	bool bChangedAny = false;
	for (const auto& Pair : NamedValues)
	{
		if (const int32* Idx = Ep->ActuatorNameToIndex.Find(Pair.Key))
		{
			if (FMath::IsFinite(Pair.Value))
			{
				Ep->LatestCommand[*Idx] = Pair.Value;
				bChangedAny = true;
			}
		}
	}

	if (bChangedAny)
	{
		Ep->LastCommandTimeSec = FPlatformTime::Seconds();
		Ep->bHasCommand = true;
	}
}

bool UURSRobotCoreComponent::RequestCameraReadback(const FString& ActorId)
{
	FScopeLock Lock(&EndpointMutex);
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
	FScopeLock Lock(&EndpointMutex);
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
	FScopeLock Lock(&EndpointMutex);
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
	FScopeLock Lock(&EndpointMutex);
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
	FScopeLock Lock(&EndpointMutex);
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
	FScopeLock EndpointLock(&EndpointMutex);
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
			for (int32 ActIdx = 0; ActIdx < Ep->Actuators.Num(); ++ActIdx)
			{
				int32 ActMjId = Ep->Actuators[ActIdx].MjId;
				if (ActMjId < 0 || ActMjId >= Model->nu) continue;
				int32 JointMjId = Model->actuator_trnid[ActMjId * 2];
				if (JointMjId < 0 || JointMjId >= Model->njnt) continue;
				int32 QposAdr = Model->jnt_qposadr[JointMjId];
				float Target = static_cast<float>(Data->qpos[QposAdr]);
				Data->ctrl[ActMjId] = Target;
				Ep->LatestCommand[ActIdx] = Target;
				if (UMjActuator* Act = Ep->Actuators[ActIdx].Actuator.Get())
				{
					Act->SetNetworkControl(Target);
				}
			}
			Ep->LastCommandTimeSec = FPlatformTime::Seconds();
			Ep->bHasCommand = true;
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
	FScopeLock EndpointLock(&EndpointMutex);
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
		FScopeLock Lock(&EndpointMutex);
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
