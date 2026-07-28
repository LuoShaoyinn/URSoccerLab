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
#include "Scene/URSSceneConfig.h"
#include "Scene/URSSceneConfigComponent.h"
#include "Scene/URSRobotTypeRegistry.h"
#include "Transport/NetworkManager.h"
#include "UObject/SavePackage.h"

namespace
{
constexpr const TCHAR* TwoRobotLevelName = TEXT("URS_TwoRobotFacing");
constexpr const TCHAR* TwoRobotSceneConfigPath = TEXT("Config/URS_two_robot_scene.json");

bool HideImportedFieldGeoms(UWorld* World, const FString& ActorId, FString& OutError)
{
	AMjArticulation* Robot = nullptr;
	for (TActorIterator<AMjArticulation> It(World); It; ++It)
	{
		if (It->ActorId == ActorId)
		{
			Robot = *It;
			break;
		}
	}
	if (!Robot)
	{
		OutError = FString::Printf(TEXT("spawned %s articulation not found"), *ActorId);
		return false;
	}

	TArray<UMjGeom*> Geoms;
	Robot->GetComponents<UMjGeom>(Geoms);
	for (UMjGeom* Geom : Geoms)
	{
		if (!Geom) continue;
		const FString MjName = Geom->MjName.IsEmpty() ? Geom->GetName() : Geom->MjName;
		if (MjName == TEXT("floor") || MjName == TEXT("vision_floor") || MjName == TEXT("vision_marker"))
		{
			Geom->SetGeomVisibility(false);
		}
	}
	Robot->MarkPackageDirty();
	return true;
}

bool EnsureRobotCameras(UWorld* World, const FString& ActorId, FString& OutError)
{
	AMjArticulation* Robot = nullptr;
	for (TActorIterator<AMjArticulation> It(World); It; ++It)
	{
		if (It->ActorId == ActorId)
		{
			Robot = *It;
			break;
		}
	}
	if (!Robot)
	{
		OutError = FString::Printf(TEXT("spawned %s articulation not found"), *ActorId);
		return false;
	}

	TArray<UMjCamera*> Cameras;
	Robot->GetComponents<UMjCamera>(Cameras);
	for (UMjCamera* Camera : Cameras)
	{
		if (!Camera) continue;
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
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FURSTwoRobotFacingCreateMap,
	"URSoccerLab.E2E.CreateTwoRobotFacingMap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FURSTwoRobotFacingCreateMap::RunTest(const FString& Parameters)
{
	URSoccerLab::FURSRobotTypeRegistry::Get().RegisterDefaultTypes();

	// Non-destructive: verify baked Blueprint, do NOT re-import.
	const FString BlueprintObjectPath = TEXT("/Game/URSoccerLab/Robots/pi_plus/pi_plus.pi_plus");
	TestTrue(TEXT("baked pi_plus Blueprint loadable"),
		LoadObject<UBlueprint>(nullptr, *BlueprintObjectPath) != nullptr);

	FString LevelPath;
	FString LevelError;
	TestTrue(TEXT("load soccer field level"),
		URLabLevelOps::LoadLevelSync(TEXT("URS_SoccerField"), LevelPath, LevelError));

	// Destroy any pre-existing robots inherited from URS_SoccerField.
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	TestNotNull(TEXT("editor world"), World);

	TArray<AMjArticulation*> ExistingRobots;
	for (TActorIterator<AMjArticulation> It(World); It; ++It)
	{
		ExistingRobots.Add(*It);
	}
	for (AMjArticulation* Actor : ExistingRobots)
	{
		const FName StaleName = MakeUniqueObjectName(
			Actor->GetOuter(), Actor->GetClass(),
			FName(*FString::Printf(TEXT("stale_%s"), *Actor->GetName())));
		Actor->Rename(*StaleName.ToString(), Actor->GetOuter(),
			REN_DontCreateRedirectors | REN_NonTransactional);
		World->DestroyActor(Actor);
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
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Manager = World->SpawnActor<AAMjManager>(
			AAMjManager::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params);
	}
	TestNotNull(TEXT("AAMjManager present"), Manager);
	if (!Manager) return false;
	Manager->bAutoCreateSimulateWidget = false;
	Manager->SetPaused(false);
	if (Manager->NetworkManager)
	{
		Manager->NetworkManager->bEnableAllCameras = true;
	}

	UURSSceneConfigComponent* SceneComp = FindObject<UURSSceneConfigComponent>(Manager, TEXT("URSSceneConfig"));
	if (!SceneComp)
	{
		SceneComp = NewObject<UURSSceneConfigComponent>(
			Manager, UURSSceneConfigComponent::StaticClass(), TEXT("URSSceneConfig"));
	}
	TestNotNull(TEXT("scene config component"), SceneComp);
	if (!SceneComp) return false;
	SceneComp->CreationMethod = EComponentCreationMethod::Instance;
	SceneComp->ConfigPath = TwoRobotSceneConfigPath;
	SceneComp->bAutoApplyOnBeginPlay = false;
	if (!SceneComp->IsRegistered())
	{
		Manager->AddInstanceComponent(SceneComp);
		SceneComp->RegisterComponent();
	}

	FString ApplyError;
	TestTrue(TEXT("scene config applies"), SceneComp->ApplyConfig(ApplyError));

	for (const FString& ActorId : {FString(TEXT("robot_rp0")), FString(TEXT("robot_rp1"))})
	{
		FString GeomError;
		if (!HideImportedFieldGeoms(World, ActorId, GeomError))
		{
			AddError(GeomError);
			return false;
		}
		FString CamError;
		if (!EnsureRobotCameras(World, ActorId, CamError))
		{
			AddError(CamError);
			return false;
		}
	}

	int32 RobotCount = 0;
	for (TActorIterator<AMjArticulation> It(World); It; ++It)
	{
		if (It->ActorId == TEXT("robot_rp0") || It->ActorId == TEXT("robot_rp1"))
		{
			++RobotCount;
		}
	}
	TestEqual(TEXT("exactly two configured robots in level"), RobotCount, 2);

	FString NewLevelPath = FString::Printf(TEXT("/Game/Levels/%s"), TwoRobotLevelName);
	FString SavedLevelPath;
	FString SaveError;
	TestTrue(TEXT("save two-robot level"),
		URLabLevelOps::SaveCurrentLevelSync(NewLevelPath, SaveError));

	UE_LOG(LogTemp, Display, TEXT("URSoccerLab two-robot facing map ready at /Game/Levels/%s"), TwoRobotLevelName);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
