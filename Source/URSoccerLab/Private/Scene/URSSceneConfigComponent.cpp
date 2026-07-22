#include "Scene/URSSceneConfigComponent.h"

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
	if (bAutoApplyOnBeginPlay)
	{
		FString Error;
		if (!ApplyConfig(Error))
		{
			UE_LOG(LogTemp, Warning, TEXT("URSoccerLab scene config auto-apply failed: %s"), *Error);
		}
	}
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

	AActor* Owner = GetOwner();
	UWorld* World = Owner ? Owner->GetWorld() : nullptr;
	AAMjManager* Manager = Cast<AAMjManager>(Owner);
	if (!Manager || !World)
	{
		OutError = TEXT("UURSSceneConfigComponent must be owned by an AAMjManager");
		return false;
	}

	DestroyConfiguredRobots();
	SpawnedRobots.Reset();

	for (const URSoccerLab::FURSRobotSpawn& Spawn : ActiveConfig.Robots)
	{
		if (!SpawnOneRobot(Manager, Spawn, OutError))
		{
			return false;
		}
	}

	UE_LOG(LogTemp, Log, TEXT("URSoccerLab scene config applied: %d robot(s)."), SpawnedRobots.Num());
	OnSceneConfigApplied.Broadcast();
	return true;
}

void UURSSceneConfigComponent::DestroyConfiguredRobots()
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
		if (!Articulation)
		{
			continue;
		}
		for (const URSoccerLab::FURSRobotSpawn& Spawn : ActiveConfig.Robots)
		{
			if (Articulation->ActorId == Spawn.ActorId)
			{
				ToDestroy.Add(Articulation);
				break;
			}
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
	Articulation->SetActorLabel(Spawn.ActorId);

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
