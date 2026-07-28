#include "Scene/URSSceneConfigComponent.h"

#include "MuJoCo/Components/Geometry/MjGeom.h"
#include "MuJoCo/Components/Sensors/MjCamera.h"
#include "MuJoCo/Core/AMjManager.h"
#include "MuJoCo/Core/MjArticulation.h"
#include "MuJoCo/Utils/MjUtils.h"
#include "Engine/World.h"
#include "EngineUtils.h"

using namespace URSoccerLab;

UURSSceneConfigComponent::UURSSceneConfigComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UURSSceneConfigComponent::BeginPlay()
{
	Super::BeginPlay();
	// Robot spawning is owned by AURSSoccerGameMode::InitGame, which runs
	// BEFORE BeginPlay to guarantee robots are in the compiled MuJoCo model.
}

bool UURSSceneConfigComponent::ReloadConfig(FString& OutError)
{
	const FString AbsPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / ConfigPath);
	if (!FURSSceneConfigIo::LoadFromFile(AbsPath, ActiveConfig, OutError))
	{
		return false;
	}
	const FURSSceneConfigValidationResult Validation = FURSSceneConfigIo::Validate(ActiveConfig);
	if (!Validation.bOk)
	{
		OutError = Validation.Errors.Num() > 0 ? Validation.Errors[0] : TEXT("scene config invalid");
		return false;
	}
	return true;
}

bool UURSSceneConfigComponent::ApplyConfig(FString& OutError)
{
	if (!ReloadConfig(OutError))
	{
		return false;
	}
	return ApplyConfig(ActiveConfig, OutError);
}

bool UURSSceneConfigComponent::ApplyConfig(const URSoccerLab::FURSSceneConfig& Config, FString& OutError)
{
	const URSoccerLab::FURSSceneConfigValidationResult Validation = URSoccerLab::FURSSceneConfigIo::Validate(Config);
	if (!Validation.bOk)
	{
		OutError = Validation.Errors.Num() > 0 ? Validation.Errors[0] : TEXT("scene config invalid");
		return false;
	}

	ActiveConfig = Config;

	AActor* Owner = GetOwner();
	UWorld* World = Owner ? Owner->GetWorld() : nullptr;
	AAMjManager* Manager = Cast<AAMjManager>(Owner);
	if (!Manager || !World)
	{
		OutError = TEXT("UURSSceneConfigComponent must be owned by an AAMjManager");
		return false;
	}

	DestroyConfiguredRobots();

	TSet<FString> NewActorIds;
	NewActorIds.Reserve(ActiveConfig.Robots.Num());
	for (const URSoccerLab::FURSRobotSpawn& Spawn : ActiveConfig.Robots)
	{
		NewActorIds.Add(Spawn.ActorId);
	}

	// Destroy any actor we previously spawned whose id was removed from the
	// current config. This is what makes ApplyConfig idempotent across
	// reloads even when ids disappear from the file.
	{
		TSet<FString> Stale = KnownActorIds.Difference(NewActorIds);
		if (Stale.Num() > 0)
		{
			DestroyActorsWithIds(Stale);
			for (const FString& Id : Stale)
			{
				KnownActorIds.Remove(Id);
				SpawnedRobots.Remove(Id);
			}
		}
	}

	TArray<FString> SpawnedInThisCall;
	for (const URSoccerLab::FURSRobotSpawn& Spawn : ActiveConfig.Robots)
	{
		if (!SpawnOneRobot(Manager, Spawn, OutError))
		{
			// Rollback: destroy everything we spawned in this call so the
			// scene is not left in a partial state.
			UE_LOG(LogTemp, Error, TEXT("URSoccerLab scene config: spawn failed for '%s', rolling back %d robot(s)."),
				*Spawn.ActorId, SpawnedInThisCall.Num());
			DestroyActorsWithIds(TSet<FString>(SpawnedInThisCall));
			for (const FString& Id : SpawnedInThisCall)
			{
				KnownActorIds.Remove(Id);
				SpawnedRobots.Remove(Id);
			}
			return false;
		}
		KnownActorIds.Add(Spawn.ActorId);
		SpawnedInThisCall.Add(Spawn.ActorId);
	}

	UE_LOG(LogTemp, Log, TEXT("URSoccerLab scene config applied: %d robot(s)."), SpawnedRobots.Num());
	OnSceneConfigApplied.Broadcast();
	return true;
}

void UURSSceneConfigComponent::DestroyConfiguredRobots()
{
	TSet<FString> IdsToDestroy;
	for (const URSoccerLab::FURSRobotSpawn& Spawn : ActiveConfig.Robots)
	{
		IdsToDestroy.Add(Spawn.ActorId);
	}
	if (IdsToDestroy.Num() == 0)
	{
		return;
	}
	DestroyActorsWithIds(IdsToDestroy);
}

void UURSSceneConfigComponent::DestroyActorsWithIds(const TSet<FString>& ActorIds)
{
	AActor* Owner = GetOwner();
	UWorld* World = Owner ? Owner->GetWorld() : nullptr;
	if (!World)
	{
		return;
	}

	TArray<AMjArticulation*> ToDestroy;
	for (AMjArticulation* Articulation : TActorRange<AMjArticulation>(World))
	{
		if (Articulation && ActorIds.Contains(Articulation->ActorId))
		{
			ToDestroy.Add(Articulation);
		}
	}

	for (AMjArticulation* Articulation : ToDestroy)
	{
		const FName StaleName = MakeUniqueObjectName(
			Articulation->GetOuter(), Articulation->GetClass(),
			FName(*FString::Printf(TEXT("stale_%s"), *Articulation->GetName())));
		Articulation->Rename(*StaleName.ToString(), Articulation->GetOuter(),
			REN_DontCreateRedirectors | REN_NonTransactional);
		World->DestroyActor(Articulation);
	}
}

bool UURSSceneConfigComponent::SpawnOneRobot(
	AAMjManager* Manager,
	const URSoccerLab::FURSRobotSpawn& Spawn,
	FString& OutError)
{
	const URSoccerLab::FURSRobotType* Type = URSoccerLab::FURSRobotTypeRegistry::Get().Find(Spawn.Type);
	if (!Type)
	{
		OutError = FString::Printf(TEXT("unknown robot type '%s'"), *Spawn.Type);
		return false;
	}

	const FString GeneratedClassPath = Type->BlueprintAssetPath + TEXT("_C");
	TSubclassOf<AActor> BlueprintClass = LoadClass<AActor>(nullptr, *GeneratedClassPath);
	if (!BlueprintClass)
	{
		OutError = FString::Printf(TEXT("failed to load blueprint class %s"), *GeneratedClassPath);
		return false;
	}

	const FVector TranslationMeters = Spawn.TranslationMeters.Get(
		FVector(0.0, 0.0, Type->DefaultBaseHeightM));
	const FQuat RotationXyzw = Spawn.RotationQuatXyzw.Get(FQuat::Identity);

	double MjPos[3] = {TranslationMeters.X, TranslationMeters.Y, TranslationMeters.Z};
	const FVector UELocation = MjUtils::MjToUEPosition(MjPos);

	double MjQuatWxyz[4] = {RotationXyzw.W, RotationXyzw.X, RotationXyzw.Y, RotationXyzw.Z};
	const FQuat UERotation = MjUtils::MjToUERotation(MjQuatWxyz);
	const FRotator UERotator = UERotation.Rotator();

	FActorSpawnParameters Params;
	Params.Name = FName(*Spawn.ActorId);
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AMjArticulation* Articulation = Manager->GetWorld()->SpawnActor<AMjArticulation>(
		BlueprintClass, UELocation, UERotator, Params);
	if (!Articulation)
	{
		OutError = FString::Printf(TEXT("SpawnActor returned null for actor_id '%s'"), *Spawn.ActorId);
		return false;
	}

	Articulation->ActorId = Spawn.ActorId;
	if (Articulation->GetName() != Spawn.ActorId)
	{
		const FName DesiredName(*Spawn.ActorId);
		if (!Articulation->Rename(*Spawn.ActorId, nullptr, REN_DontCreateRedirectors | REN_NonTransactional))
		{
			UE_LOG(LogTemp, Warning, TEXT("URSoccerLab scene config: could not rename actor to '%s'."), *Spawn.ActorId);
		}
	}
#if WITH_EDITOR
	Articulation->SetActorLabel(Spawn.ActorId);
#endif

	ConfigureRobotCameras(Articulation, Spawn.ActorId);
	HideImportedFieldGeoms(Articulation);

	FURSSpawnedRobotInfo Info;
	Info.ActorId = Spawn.ActorId;
	Info.TypeName = Spawn.Type;
	Info.InitialTranslationMeters = TranslationMeters;
	Info.InitialRotationXyzw = RotationXyzw;
	SpawnedRobots.Add(Spawn.ActorId, MoveTemp(Info));
	return true;
}

bool UURSSceneConfigComponent::GetInitialPose(
	const FString& ActorId,
	FVector& OutTranslationMeters,
	FQuat& OutRotationXyzw) const
{
	const FURSSpawnedRobotInfo* Info = SpawnedRobots.Find(ActorId);
	if (!Info)
	{
		return false;
	}
	OutTranslationMeters = Info->InitialTranslationMeters;
	OutRotationXyzw = Info->InitialRotationXyzw;
	return true;
}

void UURSSceneConfigComponent::ConfigureRobotCameras(AMjArticulation* Articulation, const FString& ActorId)
{
	if (!Articulation)
	{
		return;
	}

	TArray<UMjCamera*> Cameras;
	Articulation->GetComponents<UMjCamera>(Cameras);
	for (int32 CamIdx = 0; CamIdx < Cameras.Num(); ++CamIdx)
	{
		UMjCamera* Camera = Cameras[CamIdx];
		if (!Camera)
		{
			continue;
		}
		if (Camera->CaptureComponent)
		{
			Camera->CaptureComponent->bUseRayTracingIfEnabled = true;
		}
		if (Camera->resolution.Num() < 2)
		{
			Camera->bOverride_resolution = true;
			Camera->resolution = {640, 480};
		}
		if (Camera->fovy <= 0.0f)
		{
			Camera->bOverride_fovy = true;
			Camera->fovy = 90.0f;
		}
		Camera->Modify();
	}
}

void UURSSceneConfigComponent::HideImportedFieldGeoms(AMjArticulation* Articulation)
{
	if (!Articulation)
	{
		return;
	}

	TArray<UMjGeom*> Geoms;
	Articulation->GetComponents<UMjGeom>(Geoms);
	for (UMjGeom* Geom : Geoms)
	{
		if (!Geom)
		{
			continue;
		}
		const FString MjName = Geom->MjName.IsEmpty() ? Geom->GetName() : Geom->MjName;
		if (MjName == TEXT("floor") || MjName == TEXT("vision_floor") || MjName == TEXT("vision_marker"))
		{
			Geom->SetGeomVisibility(false);
		}
	}
}
