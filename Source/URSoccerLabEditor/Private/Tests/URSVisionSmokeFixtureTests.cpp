#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "EngineUtils.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/FileHelper.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Components/LightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "MuJoCo/Components/Geometry/MjGeom.h"
#include "MuJoCo/Components/Sensors/MjCamera.h"
#include "MuJoCo/Core/AMjManager.h"
#include "MuJoCo/Core/MjArticulation.h"
#include "MjLevelOps.h"
#include "Runtime/URSZmqRobotBridgeComponent.h"
#include "Transport/NetworkManager.h"
#include "UObject/SavePackage.h"

namespace
{
constexpr const TCHAR* VisionSmokeLevelName = TEXT("URS_VisionSmoke");
constexpr const TCHAR* VisionSmokeRobotId = TEXT("robot_rp0");
constexpr const TCHAR* VisionSmokeAssetPath = TEXT("/Game/URSoccerLab/VisionSmoke");

bool SavePackageForObject(UObject* Object, FString& OutError)
{
	if (!Object)
	{
		OutError = TEXT("cannot save null object");
		return false;
	}

	UPackage* Package = Object->GetOutermost();
	if (!Package)
	{
		OutError = FString::Printf(TEXT("object %s has no outer package"), *Object->GetName());
		return false;
	}

	const FString PackageFileName = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(Package, Object, *PackageFileName, SaveArgs))
	{
		OutError = FString::Printf(TEXT("failed to save package %s"), *Package->GetName());
		return false;
	}

	return true;
}

bool SaveImportedBlueprint(const FString& BlueprintShortName, FString& OutError)
{
	const FString BlueprintObjectPath = FString::Printf(
		TEXT("/Game/MuJoCoImports/%s.%s"), *BlueprintShortName, *BlueprintShortName);
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintObjectPath);
	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("failed to load imported blueprint %s"), *BlueprintObjectPath);
		return false;
	}

	return SavePackageForObject(Blueprint, OutError);
}

UTexture2D* ImportPitchTexture(const FString& SourcePath, FString& OutError)
{
	const FString PackageName = FPaths::Combine(VisionSmokeAssetPath, TEXT("T_VisionSmoke_Pitch"));
	UTexture2D* ExistingTexture = LoadObject<UTexture2D>(nullptr, *PackageName);
	if (ExistingTexture)
	{
		return ExistingTexture;
	}

	TArray64<uint8> CompressedData;
	if (!FFileHelper::LoadFileToArray(CompressedData, *SourcePath))
	{
		OutError = FString::Printf(TEXT("failed to load pitch texture file: %s"), *SourcePath);
		return nullptr;
	}

	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
	if (!ImageWrapper.IsValid() || !ImageWrapper->SetCompressed(CompressedData.GetData(), CompressedData.Num()))
	{
		OutError = FString::Printf(TEXT("failed to decode pitch texture file: %s"), *SourcePath);
		return nullptr;
	}

	TArray<uint8> RawData;
	if (!ImageWrapper->GetRaw(ERGBFormat::BGRA, 8, RawData))
	{
		OutError = FString::Printf(TEXT("failed to extract pitch texture pixels: %s"), *SourcePath);
		return nullptr;
	}

	UPackage* Package = CreatePackage(*PackageName);
	Package->FullyLoad();

	UTexture2D* Texture = NewObject<UTexture2D>(
		Package, TEXT("T_VisionSmoke_Pitch"), RF_Public | RF_Standalone);
	Texture->Source.Init(ImageWrapper->GetWidth(), ImageWrapper->GetHeight(), 1, 1, TSF_BGRA8, RawData.GetData());
	Texture->SRGB = true;
	Texture->CompressionSettings = TextureCompressionSettings::TC_Default;
	Texture->MipGenSettings = TextureMipGenSettings::TMGS_FromTextureGroup;
	Texture->UpdateResource();

	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(Texture);
	if (!SavePackageForObject(Texture, OutError))
	{
		return nullptr;
	}

	return Texture;
}

UMaterialInstanceConstant* CreatePitchMaterial(UTexture2D* PitchTexture, FString& OutError)
{
	if (!PitchTexture)
	{
		OutError = TEXT("cannot create pitch material without a texture");
		return nullptr;
	}

	const FString PackageName = FPaths::Combine(VisionSmokeAssetPath, TEXT("MI_VisionSmoke_Pitch"));
	UMaterialInstanceConstant* ExistingMaterial = LoadObject<UMaterialInstanceConstant>(nullptr, *PackageName);
	if (ExistingMaterial)
	{
		return ExistingMaterial;
	}

	UMaterial* MasterMaterial = LoadObject<UMaterial>(
		nullptr, TEXT("/UnrealRoboticsLab/Materials/M_MuJoCo_Master.M_MuJoCo_Master"));
	if (!MasterMaterial)
	{
		OutError = TEXT("failed to load URLab master material");
		return nullptr;
	}

	UPackage* Package = CreatePackage(*PackageName);
	Package->FullyLoad();

	UMaterialInstanceConstant* Material = NewObject<UMaterialInstanceConstant>(
		Package, TEXT("MI_VisionSmoke_Pitch"), RF_Public | RF_Standalone);
	Material->SetParentEditorOnly(MasterMaterial);
	Material->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("BaseColor")), FLinearColor::White);
	Material->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("BaseColorTexture")), PitchTexture);

	FTextureParameterValue TextureParam;
	TextureParam.ParameterInfo = FMaterialParameterInfo(TEXT("BaseColorTexture"));
	TextureParam.ParameterValue = PitchTexture;
	TextureParam.ExpressionGUID = FGuid();
	Material->TextureParameterValues.Add(TextureParam);

	FStaticParameterSet StaticParams;
	Material->GetStaticParameterValues(StaticParams);
	for (FStaticSwitchParameter& Param : StaticParams.StaticSwitchParameters)
	{
		if (Param.ParameterInfo.Name == TEXT("bUseTexture"))
		{
			Param.Value = true;
			Param.bOverride = true;
			break;
		}
	}
	Material->UpdateStaticPermutation(StaticParams);
	Material->PostEditChange();

	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(Material);
	if (!SavePackageForObject(Material, OutError))
	{
		return nullptr;
	}

	return Material;
}

bool SpawnManagerWithBridge(UWorld* World, FString& OutError)
{
	if (!World)
	{
		OutError = TEXT("editor world unavailable");
		return false;
	}

	FActorSpawnParameters Params;
	Params.Name = TEXT("URS_VisionSmoke_Manager");
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AAMjManager* Manager = World->SpawnActor<AAMjManager>(
		AAMjManager::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params);
	if (!Manager)
	{
		OutError = TEXT("failed to spawn AAMjManager");
		return false;
	}

	Manager->bAutoCreateSimulateWidget = false;
	if (Manager->NetworkManager)
	{
		Manager->NetworkManager->bEnableAllCameras = true;
	}

	UURSZmqRobotBridgeComponent* Bridge = NewObject<UURSZmqRobotBridgeComponent>(
		Manager, UURSZmqRobotBridgeComponent::StaticClass(), TEXT("URSZmqRobotBridge"));
	if (!Bridge)
	{
		OutError = TEXT("failed to create UURSZmqRobotBridgeComponent");
		return false;
	}

	Bridge->CreationMethod = EComponentCreationMethod::Instance;
	Bridge->RobotNames = {VisionSmokeRobotId};
	Bridge->CommandBasePort = 10000;
	Bridge->StatePort = 10100;
	Bridge->MetaPort = 10101;
	Bridge->StatePublishRateHz = 30.0;
	Manager->AddInstanceComponent(Bridge);
	Bridge->RegisterComponent();

	Manager->MarkPackageDirty();
	return true;
}

bool SpawnVisualFixture(UWorld* World, FString& OutError)
{
	if (!World)
	{
		OutError = TEXT("editor world unavailable");
		return false;
	}

	UStaticMesh* PlaneMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	if (!PlaneMesh)
	{
		OutError = TEXT("failed to load engine plane mesh");
		return false;
	}

	const FString PitchTexturePath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectDir(),
			TEXT("refs/mos-brain/simulation/mujoco/assets/environments/soccer/assets/pitch_nologo_l.png")));
	if (!FPaths::FileExists(PitchTexturePath))
	{
		OutError = FString::Printf(TEXT("soccer pitch texture is missing: %s"), *PitchTexturePath);
		return false;
	}

	UTexture2D* PitchTexture = ImportPitchTexture(PitchTexturePath, OutError);
	if (!PitchTexture)
	{
		return false;
	}

	UMaterialInstanceConstant* PitchMaterial = CreatePitchMaterial(PitchTexture, OutError);
	if (!PitchMaterial)
	{
		return false;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AStaticMeshActor* Floor = World->SpawnActor<AStaticMeshActor>(
		AStaticMeshActor::StaticClass(), FVector(0.0, 0.0, -0.5), FRotator::ZeroRotator, Params);
	if (!Floor || !Floor->GetStaticMeshComponent())
	{
		OutError = TEXT("failed to spawn vision floor");
		return false;
	}
	Floor->SetActorLabel(TEXT("URS_VisionSmoke_Floor"));
	Floor->Tags.AddUnique(FName(TEXT("URLab.ActorId=vision_floor_visual")));
	Floor->GetStaticMeshComponent()->SetStaticMesh(PlaneMesh);
	Floor->GetStaticMeshComponent()->SetMaterial(0, PitchMaterial);
	Floor->SetActorScale3D(FVector(9.0, 6.0, 1.0));

	return true;
}

bool ConfigureRobotCameras(UWorld* World, FString& OutError)
{
	if (!World)
	{
		OutError = TEXT("editor world unavailable");
		return false;
	}

	AMjArticulation* Robot = nullptr;
	for (TActorIterator<AMjArticulation> It(World); It; ++It)
	{
		if (It->ActorId == VisionSmokeRobotId)
		{
			Robot = *It;
			break;
		}
	}

	if (!Robot)
	{
		OutError = TEXT("spawned robot_rp0 articulation not found");
		return false;
	}

	TArray<UMjCamera*> Cameras;
	Robot->GetComponents<UMjCamera>(Cameras);
	if (Cameras.IsEmpty())
	{
		OutError = TEXT("spawned robot_rp0 has no UMjCamera components");
		return false;
	}

	for (int32 Index = 0; Index < Cameras.Num(); ++Index)
	{
		UMjCamera* Camera = Cameras[Index];
		if (!Camera)
			continue;

		Camera->bEnableZmqBroadcast = true;
		Camera->bEnableShmBroadcast = false;
		Camera->ZmqEndpoint = FString::Printf(TEXT("tcp://0.0.0.0:%d"), 5558 + Index);
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

	Robot->MarkPackageDirty();
	return true;
}

bool HideImportedFieldGeoms(UWorld* World, FString& OutError)
{
	if (!World)
	{
		OutError = TEXT("editor world unavailable");
		return false;
	}

	AMjArticulation* Robot = nullptr;
	for (TActorIterator<AMjArticulation> It(World); It; ++It)
	{
		if (It->ActorId == VisionSmokeRobotId)
		{
			Robot = *It;
			break;
		}
	}

	if (!Robot)
	{
		OutError = TEXT("spawned robot_rp0 articulation not found");
		return false;
	}

	TArray<UMjGeom*> Geoms;
	Robot->GetComponents<UMjGeom>(Geoms);
	for (UMjGeom* Geom : Geoms)
	{
		if (!Geom)
			continue;

		const FString MjName = Geom->MjName.IsEmpty() ? Geom->GetName() : Geom->MjName;
		if (MjName == TEXT("floor") || MjName == TEXT("vision_floor") || MjName == TEXT("vision_marker"))
		{
			Geom->SetGeomVisibility(false);
		}
	}

	Robot->MarkPackageDirty();
	return true;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FURSVisionSmokeCreateMap,
	"URSoccerLab.E2E.CreateVisionSmokeMap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FURSVisionSmokeCreateMap::RunTest(const FString& Parameters)
{
	const FString XmlPath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectDir(),
			TEXT("Assets/MosBrainCameraTest/pi_plus/pi_plus_urlab_origin_camera.xml")));

	if (!FPaths::FileExists(XmlPath))
	{
		AddError(FString::Printf(TEXT("vision smoke XML is missing: %s"), *XmlPath));
		return false;
	}

	FString BlueprintClassPath;
	FString BlueprintShortName;
	FString ImportError;
	bool bImportedNow = false;
	if (!URLabLevelOps::ImportXmlSync(
			XmlPath, true, BlueprintClassPath, BlueprintShortName, bImportedNow, ImportError))
	{
		AddError(FString::Printf(TEXT("ImportXmlSync failed: %s"), *ImportError));
		return false;
	}
	if (!SaveImportedBlueprint(BlueprintShortName, ImportError))
	{
		AddError(FString::Printf(TEXT("SaveImportedBlueprint failed: %s"), *ImportError));
		return false;
	}

	FString LevelPath;
	FString LevelError;
	if (!URLabLevelOps::CreateLevelSync(VisionSmokeLevelName, true, LevelPath, LevelError))
	{
		AddError(FString::Printf(TEXT("CreateLevelSync failed: %s"), *LevelError));
		return false;
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	FString SetupError;
	if (!SpawnManagerWithBridge(World, SetupError))
	{
		AddError(SetupError);
		return false;
	}

	if (!SpawnVisualFixture(World, SetupError))
	{
		AddError(SetupError);
		return false;
	}

	FString LightName;
	FString LightPath;
	FString LightKind;
	FString LightError;
	if (!URLabLevelOps::SpawnLightSync(
			TEXT("point"), TEXT("vision_key_light"),
			FVector(0.0, 0.0, 4.0), FVector::ZeroVector,
			50000.0f, FLinearColor::White,
			LightName, LightPath, LightKind, LightError))
	{
		AddError(FString::Printf(TEXT("SpawnLightSync failed: %s"), *LightError));
		return false;
	}

	FString ActorName;
	FString ActorPath;
	FString SpawnClassPath;
	FString SpawnError;
	bool bWasExisting = false;
	if (!URLabLevelOps::SpawnActorSync(
			BlueprintClassPath, VisionSmokeRobotId,
			FVector::ZeroVector, FQuat::Identity, FVector::OneVector,
			ActorName, ActorPath, SpawnClassPath, bWasExisting, SpawnError))
	{
		AddError(FString::Printf(TEXT("SpawnActorSync failed: %s"), *SpawnError));
		return false;
	}

	if (!HideImportedFieldGeoms(World, SetupError))
	{
		AddError(SetupError);
		return false;
	}

	if (!ConfigureRobotCameras(World, SetupError))
	{
		AddError(SetupError);
		return false;
	}

	FString SavedLevelPath;
	FString SaveError;
	if (!URLabLevelOps::SaveCurrentLevelSync(SavedLevelPath, SaveError))
	{
		AddError(FString::Printf(TEXT("SaveCurrentLevelSync failed: %s"), *SaveError));
		return false;
	}

	TestEqual(TEXT("saved level path"), SavedLevelPath, FString(TEXT("/Game/Levels/URS_VisionSmoke")));
	UE_LOG(LogTemp, Display, TEXT("URSoccerLab vision smoke map ready at %s"), *SavedLevelPath);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
