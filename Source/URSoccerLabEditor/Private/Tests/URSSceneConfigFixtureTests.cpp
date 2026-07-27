#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Misc/AutomationTest.h"
#include "MuJoCo/Core/AMjManager.h"
#include "MuJoCo/Core/MjArticulation.h"
#include "MjLevelOps.h"
#include "Scene/URSSceneConfig.h"
#include "Scene/URSSceneConfigComponent.h"
#include "Scene/URSRobotTypeRegistry.h"

namespace
{
constexpr const TCHAR* SoccerFieldLevelName = TEXT("URS_SoccerField");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FURSSceneConfigApplyTest,
	"URSoccerLab.E2E.ApplyDefaultSceneConfig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FURSSceneConfigApplyTest::RunTest(const FString& Parameters)
{
	URSoccerLab::FURSRobotTypeRegistry::Get().RegisterDefaultTypes();

	const FString XmlPath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectDir(),
			TEXT("Assets/MosBrainCameraTest/pi_plus/pi_plus_stereo_camera.xml")));
	TestTrue(TEXT("pi_plus source XML present"), FPaths::FileExists(XmlPath));

	FString BlueprintClassPath;
	FString BlueprintShortName;
	FString ImportError;
	bool bImportedNow = false;
	TestTrue(TEXT("import pi_plus blueprint"),
		URLabLevelOps::ImportXmlSync(
			XmlPath, true, BlueprintClassPath, BlueprintShortName, bImportedNow, ImportError));

	FString LevelPath;
	FString LevelError;
	TestTrue(TEXT("load soccer field level"),
		URLabLevelOps::LoadLevelSync(SoccerFieldLevelName, LevelPath, LevelError));

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	TestNotNull(TEXT("editor world"), World);

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
	if (!Manager)
	{
		return false;
	}
	Manager->bAutoCreateSimulateWidget = false;
	Manager->SetPaused(true);

	UURSSceneConfigComponent* SceneComp = NewObject<UURSSceneConfigComponent>(
		Manager, UURSSceneConfigComponent::StaticClass(), TEXT("URSSceneConfig"));
	SceneComp->CreationMethod = EComponentCreationMethod::Instance;
	if (!SceneComp->IsRegistered())
	{
		Manager->AddInstanceComponent(SceneComp);
		SceneComp->RegisterComponent();
	}

	const URSoccerLab::FURSSceneConfig DefaultConfig = URSoccerLab::FURSSceneConfigIo::MakeDefault();
	const URSoccerLab::FURSSceneConfigValidationResult Validation = URSoccerLab::FURSSceneConfigIo::Validate(DefaultConfig);
	TestTrue(TEXT("default config validates"), Validation.bOk);

	FString ApplyError;
	const bool bApplied = SceneComp->ApplyConfig(DefaultConfig, ApplyError);
	TestTrue(TEXT("ApplyConfig succeeded"), bApplied);
	if (!bApplied)
	{
		UE_LOG(LogTemp, Error, TEXT("ApplyConfig error: %s"), *ApplyError);
		return false;
	}

	int32 FoundRobots = 0;
	FString FoundActorId;
	for (TActorIterator<AMjArticulation> It(World); It; ++It)
	{
		if (It->ActorId == TEXT("robot_rp0"))
		{
			++FoundRobots;
			FoundActorId = It->ActorId;
		}
	}
	TestEqual(TEXT("exactly one robot_rp0 spawned"), FoundRobots, 1);
	TestEqual(TEXT("spawned actor id"), FoundActorId, FString(TEXT("robot_rp0")));

	FVector InitialTranslation = FVector::ZeroVector;
	FQuat InitialRotation = FQuat::Identity;
	TestTrue(TEXT("initial pose recorded"),
		SceneComp->GetInitialPose(TEXT("robot_rp0"), InitialTranslation, InitialRotation));
	TestEqual(TEXT("initial pose Z"), InitialTranslation.Z, 0.3762);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
