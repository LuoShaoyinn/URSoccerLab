#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "MuJoCo/Components/Geometry/MjGeom.h"
#include "MuJoCo/Components/Sensors/MjCamera.h"
#include "MuJoCo/Core/AMjManager.h"
#include "MuJoCo/Core/MjArticulation.h"
#include "MjLevelOps.h"
#include "Scene/URSSceneConfigComponent.h"
#include "Transport/NetworkManager.h"
#include "UObject/SavePackage.h"
#include "URSSoccerGameMode.h"

namespace
{
constexpr const TCHAR* SoccerFieldLevelName = TEXT("URS_SoccerField");

bool SpawnManagerWithSceneConfig(UWorld* World, FString& OutError)
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
	if (SceneComp)
	{
		SceneComp->CreationMethod = EComponentCreationMethod::Instance;
		if (!SceneComp->IsRegistered())
		{
			Manager->AddInstanceComponent(SceneComp);
			SceneComp->RegisterComponent();
		}
	}

	Manager->MarkPackageDirty();
	return true;
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FURSVisionSmokeCreateMap,
	"URSoccerLab.E2E.CreateVisionSmokeMap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FURSVisionSmokeCreateMap::RunTest(const FString& Parameters)
{
	// Non-destructive: validate that the baked pi_plus Blueprint exists at
	// the tracked path. Do NOT re-import from XML.
	const FString BlueprintObjectPath = TEXT("/Game/URSoccerLab/Robots/pi_plus/pi_plus.pi_plus");
	UBlueprint* RobotBP = LoadObject<UBlueprint>(nullptr, *BlueprintObjectPath);
	TestNotNull(TEXT("baked pi_plus Blueprint exists"), RobotBP);
	if (!RobotBP)
	{
		AddError(FString::Printf(TEXT("Baked Blueprint not found at %s. Run URSoccerLab.Maintenance.MigrateRobotAssets first."), *BlueprintObjectPath));
		return false;
	}

	FString LevelPath;
	FString LevelError;
	TestTrue(TEXT("load soccer field level"),
		URLabLevelOps::LoadLevelSync(SoccerFieldLevelName, LevelPath, LevelError));

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	TestNotNull(TEXT("editor world"), World);
	if (!World) return false;

	FString SetupError;
	TestTrue(TEXT("spawn manager with scene config"),
		SpawnManagerWithSceneConfig(World, SetupError));

	if (AWorldSettings* WS = World->GetWorldSettings(true))
	{
		WS->DefaultGameMode = AURSSoccerGameMode::StaticClass();
	}

	FString SavedLevelPath;
	FString SaveError;
	TestTrue(TEXT("save level"),
		URLabLevelOps::SaveCurrentLevelSync(SavedLevelPath, SaveError));

	TestEqual(TEXT("saved level path"), SavedLevelPath, FString(TEXT("/Game/Levels/URS_SoccerField")));
	UE_LOG(LogTemp, Display, TEXT("URSoccerLab field-only map ready at %s"), *SavedLevelPath);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
