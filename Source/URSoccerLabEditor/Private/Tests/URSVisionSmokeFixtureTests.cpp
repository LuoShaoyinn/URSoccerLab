#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Components/LightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
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
	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!PlaneMesh || !CubeMesh)
	{
		OutError = TEXT("failed to load engine basic shape meshes");
		return false;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AStaticMeshActor* Floor = World->SpawnActor<AStaticMeshActor>(
		AStaticMeshActor::StaticClass(), FVector(0.0, 0.0, -1.0), FRotator::ZeroRotator, Params);
	if (!Floor || !Floor->GetStaticMeshComponent())
	{
		OutError = TEXT("failed to spawn vision floor");
		return false;
	}
	Floor->SetActorLabel(TEXT("URS_VisionSmoke_Floor"));
	Floor->Tags.AddUnique(FName(TEXT("URLab.ActorId=vision_floor_visual")));
	Floor->GetStaticMeshComponent()->SetStaticMesh(PlaneMesh);
	Floor->SetActorScale3D(FVector(0.08, 0.08, 1.0));

	AStaticMeshActor* Marker = World->SpawnActor<AStaticMeshActor>(
		AStaticMeshActor::StaticClass(), FVector(15.0, 0.0, 4.0), FRotator::ZeroRotator, Params);
	if (!Marker || !Marker->GetStaticMeshComponent())
	{
		OutError = TEXT("failed to spawn vision marker");
		return false;
	}
	Marker->SetActorLabel(TEXT("URS_VisionSmoke_Marker"));
	Marker->Tags.AddUnique(FName(TEXT("URLab.ActorId=vision_marker_visual")));
	Marker->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);
	Marker->SetActorScale3D(FVector(0.08, 0.08, 0.08));

	ASkyLight* Sky = World->SpawnActor<ASkyLight>(
		ASkyLight::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params);
	if (Sky && Sky->GetLightComponent())
	{
		Sky->GetLightComponent()->SetIntensity(1.5f);
	}

	APointLight* Fill = World->SpawnActor<APointLight>(
		APointLight::StaticClass(), FVector(80.0, -120.0, 180.0), FRotator::ZeroRotator, Params);
	if (Fill && Fill->GetLightComponent())
	{
		Fill->GetLightComponent()->SetIntensity(8000.0f);
		Fill->GetLightComponent()->SetLightColor(FLinearColor::White);
	}

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
			TEXT("directional"), TEXT("vision_key_light"),
			FVector(0.0, 0.0, 3.0), FVector(-45.0, 0.0, 45.0),
			10.0f, FLinearColor::White,
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
