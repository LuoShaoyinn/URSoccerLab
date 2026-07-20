#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "EngineUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
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
constexpr const TCHAR* VisionSmokeRobotId = TEXT("robot_rp0");
constexpr const TCHAR* SoccerFieldLevelName = TEXT("URS_SoccerField");

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

bool SpawnManagerWithBridge(UWorld* World, FString& OutError)
{
	if (!World)
	{
		OutError = TEXT("editor world unavailable");
		return false;
	}

	AAMjManager* Manager = nullptr;
	for (TActorIterator<AAMjManager> It(World); It; ++It)
	{
		Manager = *It;
		break;
	}

	if (!Manager)
	{
		FActorSpawnParameters Params;
		Params.Name = TEXT("URS_VisionSmoke_Manager");
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Manager = World->SpawnActor<AAMjManager>(
			AAMjManager::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params);
	}
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

	UURSZmqRobotBridgeComponent* Bridge = FindObject<UURSZmqRobotBridgeComponent>(Manager, TEXT("URSZmqRobotBridge"));
	if (!Bridge)
	{
		Bridge = NewObject<UURSZmqRobotBridgeComponent>(
			Manager, UURSZmqRobotBridgeComponent::StaticClass(), TEXT("URSZmqRobotBridge"));
	}
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
	if (!Bridge->IsRegistered())
	{
		Manager->AddInstanceComponent(Bridge);
		Bridge->RegisterComponent();
	}

	Manager->MarkPackageDirty();
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
	if (!URLabLevelOps::LoadLevelSync(SoccerFieldLevelName, LevelPath, LevelError))
	{
		AddError(FString::Printf(TEXT("LoadLevelSync failed: %s"), *LevelError));
		return false;
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	FString SetupError;
	if (!SpawnManagerWithBridge(World, SetupError))
	{
		AddError(SetupError);
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

	TestEqual(TEXT("saved level path"), SavedLevelPath, FString(TEXT("/Game/Levels/URS_SoccerField")));
	UE_LOG(LogTemp, Display, TEXT("URSoccerLab vision smoke map ready at %s"), *SavedLevelPath);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
