#include "Scene/URSSoccerFieldSceneBuilder.h"

#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Components/LightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TextureCube.h"
#include "Misc/Paths.h"
#include "MjLevelOps.h"

namespace
{
struct FFieldNodeTransform
{
	FVector LocationCm = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	FVector Scale = FVector::OneVector;
};

bool GetFieldNodeTransform(const FString& MeshName, FFieldNodeTransform& OutTransform)
{
	if (MeshName == TEXT("Plane"))
	{
		OutTransform.Scale = FVector(5.3000002f, 3.9000001f, 1.0f);
		return true;
	}

	if (MeshName == TEXT("goal_00"))
	{
		OutTransform.LocationCm = FVector(-450.0f, 94.0f, 50.0f);
		OutTransform.Scale = FVector(0.05f, 0.05f, 0.5f);
		return true;
	}
	if (MeshName == TEXT("goal_01"))
	{
		OutTransform.LocationCm = FVector(-450.0f, -94.0f, 50.0f);
		OutTransform.Scale = FVector(0.05f, 0.05f, 0.5f);
		return true;
	}
	if (MeshName == TEXT("goal_10"))
	{
		OutTransform.LocationCm = FVector(450.0f, 94.0f, 50.0f);
		OutTransform.Scale = FVector(0.05f, 0.05f, 0.5f);
		return true;
	}
	if (MeshName == TEXT("goal_11"))
	{
		OutTransform.LocationCm = FVector(450.0f, -94.0f, 50.0f);
		OutTransform.Scale = FVector(0.05f, 0.05f, 0.5f);
		return true;
	}
	if (MeshName == TEXT("goal_0"))
	{
		OutTransform.LocationCm = FVector(450.0f, 0.0f, 96.0f);
		OutTransform.Rotation = FRotator(0.0f, 0.0f, 90.0f);
		OutTransform.Scale = FVector(0.05f, 0.05f, 0.95f);
		return true;
	}
	if (MeshName == TEXT("goal_1"))
	{
		OutTransform.LocationCm = FVector(-450.0f, 0.0f, 96.0f);
		OutTransform.Rotation = FRotator(0.0f, 0.0f, 90.0f);
		OutTransform.Scale = FVector(0.05f, 0.05f, 0.95f);
		return true;
	}

	return false;
}

bool FindImportedStaticMeshes(const FString& ImportPath, TArray<UStaticMesh*>& OutMeshes)
{
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	TArray<FAssetData> Assets;
	AssetRegistry.GetAssetsByPath(FName(ImportPath), Assets, true);
	for (const FAssetData& Asset : Assets)
	{
		if (Asset.AssetClassPath.GetAssetName() == TEXT("StaticMesh"))
		{
			if (UStaticMesh* Mesh = Cast<UStaticMesh>(Asset.GetAsset()))
			{
				OutMeshes.AddUnique(Mesh);
			}
		}
	}
	return !OutMeshes.IsEmpty();
}

bool ImportFieldAssetsIfNeeded(const FURSSoccerFieldSceneBuildOptions& Options, TArray<UStaticMesh*>& OutMeshes, FString& OutError)
{
	const FString FieldSourcePath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectDir(), Options.FieldSourceRelativePath));
	if (!FPaths::FileExists(FieldSourcePath))
	{
		OutError = FString::Printf(TEXT("soccer field GLB is missing: %s"), *FieldSourcePath);
		return false;
	}

	if (!Options.bForceReimportField && FindImportedStaticMeshes(Options.FieldImportPath, OutMeshes))
	{
		return true;
	}

	UAssetImportTask* ImportTask = NewObject<UAssetImportTask>();
	ImportTask->Filename = FieldSourcePath;
	ImportTask->DestinationPath = Options.FieldImportPath;
	ImportTask->bAutomated = true;
	ImportTask->bSave = true;
	ImportTask->bReplaceExisting = true;
	ImportTask->bReplaceExistingSettings = true;

	TArray<UAssetImportTask*> ImportTasks;
	ImportTasks.Add(ImportTask);
	FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get().ImportAssetTasks(ImportTasks);

	OutMeshes.Reset();
	if (!FindImportedStaticMeshes(Options.FieldImportPath, OutMeshes))
	{
		OutError = FString::Printf(TEXT("failed to import static meshes from soccer field GLB: %s"), *FieldSourcePath);
		return false;
	}
	return true;
}
} // namespace

bool FURSSoccerFieldSceneBuilder::BuildScene(
	const FURSSoccerFieldSceneBuildOptions& Options, FString& OutLevelPath, FString& OutError)
{
	if (!URLabLevelOps::CreateLevelSync(Options.LevelName, Options.bForceOverwriteLevel, OutLevelPath, OutError))
	{
		return false;
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!SpawnVisualFixture(World, Options, OutError))
	{
		return false;
	}

	if (Options.bAddDefaultSkyLight && !SpawnDefaultSkyLight(World, Options, OutError))
	{
		return false;
	}

	return URLabLevelOps::SaveCurrentLevelSync(OutLevelPath, OutError);
}

bool FURSSoccerFieldSceneBuilder::SpawnVisualFixture(
	UWorld* World, const FURSSoccerFieldSceneBuildOptions& Options, FString& OutError)
{
	if (!World)
	{
		OutError = TEXT("editor world unavailable");
		return false;
	}

	TArray<UStaticMesh*> FieldMeshes;
	if (!ImportFieldAssetsIfNeeded(Options, FieldMeshes, OutError))
	{
		return false;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	for (int32 MeshIndex = 0; MeshIndex < FieldMeshes.Num(); ++MeshIndex)
	{
		UStaticMesh* FieldMesh = FieldMeshes[MeshIndex];
		if (!FieldMesh)
		{
			continue;
		}

		AStaticMeshActor* FieldActor = World->SpawnActor<AStaticMeshActor>(
			AStaticMeshActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params);
		if (!FieldActor || !FieldActor->GetStaticMeshComponent())
		{
			OutError = TEXT("failed to spawn soccer field mesh");
			return false;
		}

		FieldActor->SetActorLabel(FString::Printf(TEXT("URS_SoccerField_%d"), MeshIndex));
		FieldActor->Tags.AddUnique(FName(TEXT("URLab.ActorId=soccer_field_visual")));
		FieldActor->GetStaticMeshComponent()->SetStaticMesh(FieldMesh);

		FFieldNodeTransform NodeTransform;
		if (GetFieldNodeTransform(FieldMesh->GetName(), NodeTransform))
		{
			FieldActor->SetActorLocation(NodeTransform.LocationCm);
			FieldActor->SetActorRotation(NodeTransform.Rotation);
			FieldActor->SetActorScale3D(NodeTransform.Scale);
		}
	}

	return true;
}

bool FURSSoccerFieldSceneBuilder::SpawnDefaultSkyLight(
	UWorld* World, const FURSSoccerFieldSceneBuildOptions& Options, FString& OutError)
{
	if (!World)
	{
		OutError = TEXT("editor world unavailable");
		return false;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ASkyLight* Sky = World->SpawnActor<ASkyLight>(
		ASkyLight::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params);
	if (!Sky || !Sky->GetLightComponent())
	{
		OutError = TEXT("failed to spawn sky light");
		return false;
	}

	Sky->SetActorLabel(TEXT("URS_DefaultSkyLight"));
	Sky->Tags.AddUnique(FName(TEXT("URLab.ActorId=default_sky_light")));
	USkyLightComponent* SkyComponent = Sky->GetLightComponent();
	SkyComponent->SourceType = SLS_SpecifiedCubemap;
	SkyComponent->SetCubemap(LoadObject<UTextureCube>(
		nullptr, TEXT("/Engine/MapTemplates/Sky/DaylightAmbientCubemap.DaylightAmbientCubemap")));
	SkyComponent->bLowerHemisphereIsBlack = false;
	SkyComponent->SetLowerHemisphereColor(FLinearColor::White);
	SkyComponent->SetIntensity(Options.SkyLightIntensity);
	SkyComponent->SetMobility(EComponentMobility::Movable);
	SkyComponent->RecaptureSky();
	return true;
}
