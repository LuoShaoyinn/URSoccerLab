#include "Scene/URSSceneBakeLibrary.h"

#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TextureCube.h"
#include "MjLevelOps.h"

namespace
{
UWorld* GetEditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

void AddActorIdTag(AActor* Actor, const FString& ActorIdTag)
{
	if (Actor && !ActorIdTag.IsEmpty())
	{
		Actor->Tags.AddUnique(FName(*ActorIdTag));
	}
}
} // namespace

bool UURSSceneBakeLibrary::CreateOrReplaceLevel(const FString& LevelName)
{
	FString LevelPath;
	FString Error;
	constexpr bool bForceOverwriteLevel = true;
	if (!URLabLevelOps::CreateLevelSync(LevelName, bForceOverwriteLevel, LevelPath, Error))
	{
		UE_LOG(LogTemp, Error, TEXT("CreateOrReplaceLevel failed: %s"), *Error);
		return false;
	}
	return true;
}

bool UURSSceneBakeLibrary::SaveCurrentLevel(const FString& LevelPath)
{
	FString SavedLevelPath = LevelPath;
	FString Error;
	if (!URLabLevelOps::SaveCurrentLevelSync(SavedLevelPath, Error))
	{
		UE_LOG(LogTemp, Error, TEXT("SaveCurrentLevel failed: %s"), *Error);
		return false;
	}
	return true;
}

bool UURSSceneBakeLibrary::SpawnStaticMeshActor(
	UStaticMesh* Mesh,
	const FString& Label,
	const FVector& Location,
	const FRotator& Rotation,
	const FVector& Scale,
	const FString& ActorIdTag)
{
	UWorld* World = GetEditorWorld();
	if (!World || !Mesh)
	{
		UE_LOG(LogTemp, Error, TEXT("SpawnStaticMeshActor failed: missing editor world or mesh"));
		return false;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
		AStaticMeshActor::StaticClass(), Location, Rotation, Params);
	if (!Actor || !Actor->GetStaticMeshComponent())
	{
		UE_LOG(LogTemp, Error, TEXT("SpawnStaticMeshActor failed: spawn returned null"));
		return false;
	}

	Actor->SetActorLabel(Label);
	AddActorIdTag(Actor, ActorIdTag);
	Actor->SetActorScale3D(Scale);
	Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
	return true;
}

bool UURSSceneBakeLibrary::SpawnSpecifiedCubemapSkyLight(
	UTextureCube* Cubemap,
	const FString& Label,
	float Intensity,
	const FString& ActorIdTag)
{
	UWorld* World = GetEditorWorld();
	if (!World)
	{
		UE_LOG(LogTemp, Error, TEXT("SpawnSpecifiedCubemapSkyLight failed: missing editor world"));
		return false;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ASkyLight* Sky = World->SpawnActor<ASkyLight>(
		ASkyLight::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params);
	if (!Sky || !Sky->GetLightComponent())
	{
		UE_LOG(LogTemp, Error, TEXT("SpawnSpecifiedCubemapSkyLight failed: spawn returned null"));
		return false;
	}

	Sky->SetActorLabel(Label);
	AddActorIdTag(Sky, ActorIdTag);
	USkyLightComponent* SkyComponent = Sky->GetLightComponent();
	SkyComponent->SourceType = SLS_SpecifiedCubemap;
	SkyComponent->SetCubemap(Cubemap);
	SkyComponent->bLowerHemisphereIsBlack = false;
	SkyComponent->SetLowerHemisphereColor(FLinearColor::White);
	SkyComponent->SetIntensity(Intensity);
	SkyComponent->SetMobility(EComponentMobility::Movable);
	SkyComponent->RecaptureSky();
	return true;
}
