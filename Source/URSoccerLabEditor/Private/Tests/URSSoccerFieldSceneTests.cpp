#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Scene/URSSoccerFieldSceneBuilder.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FURSSoccerFieldCreateScene,
	"URSoccerLab.Scene.CreateSoccerField",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FURSSoccerFieldCreateScene::RunTest(const FString& Parameters)
{
	FURSSoccerFieldSceneBuildOptions Options;
	FString LevelPath;
	FString Error;
	if (!FURSSoccerFieldSceneBuilder::BuildScene(Options, LevelPath, Error))
	{
		AddError(Error);
		return false;
	}

	TestEqual(TEXT("saved level path"), LevelPath, FString(TEXT("/Game/Levels/URS_SoccerField")));
	UE_LOG(LogTemp, Display, TEXT("URSoccerLab soccer field scene ready at %s"), *LevelPath);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
