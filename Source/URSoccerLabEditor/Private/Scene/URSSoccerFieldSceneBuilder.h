#pragma once

#include "CoreMinimal.h"

struct FURSSoccerFieldSceneBuildOptions
{
	FString LevelName = TEXT("URS_SoccerField");
	FString FieldSourceRelativePath = TEXT("Assets/Scenes/SoccerField/source/field.glb");
	FString FieldImportPath = TEXT("/Game/URSoccerLab/Scenes/SoccerField");
	bool bForceOverwriteLevel = true;
	bool bForceReimportField = true;
	bool bAddDefaultSkyLight = true;
	float SkyLightIntensity = 3.0f;
};

class FURSSoccerFieldSceneBuilder
{
public:
	static bool BuildScene(const FURSSoccerFieldSceneBuildOptions& Options, FString& OutLevelPath, FString& OutError);
	static bool SpawnVisualFixture(UWorld* World, const FURSSoccerFieldSceneBuildOptions& Options, FString& OutError);
	static bool SpawnDefaultSkyLight(UWorld* World, const FURSSoccerFieldSceneBuildOptions& Options, FString& OutError);
};
