#pragma once

#include "CoreMinimal.h"
#include "Scene/URSRobotTypeRegistry.h"

namespace URSoccerLab
{
struct URSOCCERLAB_API FURSRobotSpawn
{
	FString ActorId;
	FString Type;
	TOptional<FVector> TranslationMeters;
	TOptional<FQuat> RotationQuatXyzw;
};

struct URSOCCERLAB_API FURSSceneConfig
{
	FString Version = TEXT("urs_scene_v1");
	TArray<FURSRobotSpawn> Robots;
};

struct URSOCCERLAB_API FURSSceneConfigValidationResult
{
	bool bOk = true;
	TArray<FString> Errors;
};

class URSOCCERLAB_API FURSSceneConfigIo
{
public:
	static bool LoadFromFile(const FString& AbsPath, FURSSceneConfig& Out, FString& OutError);
	static bool WriteToFile(const FString& AbsPath, const FURSSceneConfig& In, FString& OutError);

	static FURSSceneConfig MakeDefault();
	static FURSSceneConfigValidationResult Validate(const FURSSceneConfig& Config);
};
} // namespace URSoccerLab
