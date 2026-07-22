#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Scene/URSSceneConfig.h"
#include "URSSceneConfigComponent.generated.h"

class AAMjManager;
class AMjArticulation;
USTRUCT(BlueprintType)
struct FURSSpawnedRobotInfo
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab|Scene")
	FString ActorId;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab|Scene")
	FString TypeName;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab|Scene")
	FVector InitialTranslationMeters = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab|Scene")
	FQuat InitialRotationXyzw = FQuat::Identity;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnSceneConfigApplied);

UCLASS(ClassGroup = (URSoccerLab), meta = (BlueprintSpawnableComponent))
class URSOCCERLAB_API UURSSceneConfigComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UURSSceneConfigComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab|Scene")
	FString ConfigPath = TEXT("Config/URS_scene.json");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab|Scene")
	bool bAutoApplyOnBeginPlay = true;

	UPROPERTY(BlueprintAssignable, Category = "URSoccerLab|Scene")
	FOnSceneConfigApplied OnSceneConfigApplied;

	UFUNCTION(BlueprintCallable, Category = "URSoccerLab|Scene")
	bool ReloadConfig(FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "URSoccerLab|Scene")
	bool ApplyConfig(FString& OutError);

	UFUNCTION(BlueprintPure, Category = "URSoccerLab|Scene")
	bool GetInitialPose(const FString& ActorId, FVector& OutTranslationMeters, FQuat& OutRotationXyzw) const;

	const TMap<FString, FURSSpawnedRobotInfo>& GetSpawnedRobots() const { return SpawnedRobots; }
	const URSoccerLab::FURSSceneConfig& GetActiveConfig() const { return ActiveConfig; }

	/** Returns the actor_ids this component has spawned at some point and
	 *  still knows about (used to detect ids that were removed from the
	 *  config on reload so the caller can destroy the stale actors). */
	const TSet<FString>& GetKnownActorIds() const { return KnownActorIds; }

	virtual void BeginPlay() override;

private:
	URSoccerLab::FURSSceneConfig ActiveConfig;
	TMap<FString, FURSSpawnedRobotInfo> SpawnedRobots;
	TSet<FString> KnownActorIds;

	void DestroyConfiguredRobots();
	void DestroyActorsWithIds(const TSet<FString>& ActorIds);
	bool SpawnOneRobot(AAMjManager* Manager, const URSoccerLab::FURSRobotSpawn& Spawn, FString& OutError);
};
