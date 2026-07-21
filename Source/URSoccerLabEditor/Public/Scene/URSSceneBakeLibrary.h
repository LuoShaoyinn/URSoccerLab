#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "URSSceneBakeLibrary.generated.h"

class UStaticMesh;
class UTextureCube;

UCLASS()
class URSOCCERLABEDITOR_API UURSSceneBakeLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "URSoccerLab|SceneBake")
	static bool CreateOrReplaceLevel(const FString& LevelName);

	UFUNCTION(BlueprintCallable, Category = "URSoccerLab|SceneBake")
	static bool SaveCurrentLevel(const FString& LevelPath);

	UFUNCTION(BlueprintCallable, Category = "URSoccerLab|SceneBake")
	static bool SpawnStaticMeshActor(
		UStaticMesh* Mesh,
		const FString& Label,
		const FVector& Location,
		const FRotator& Rotation,
		const FVector& Scale,
		const FString& ActorIdTag);

	UFUNCTION(BlueprintCallable, Category = "URSoccerLab|SceneBake")
	static bool SpawnSpecifiedCubemapSkyLight(
		UTextureCube* Cubemap,
		const FString& Label,
		float Intensity,
		const FString& ActorIdTag);
};
