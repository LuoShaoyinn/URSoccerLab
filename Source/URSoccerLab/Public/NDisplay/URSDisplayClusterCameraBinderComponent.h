#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "URSDisplayClusterCameraBinderComponent.generated.h"

class UCameraComponent;

/**
 * Profiling adapter that binds URLab MuJoCo cameras to nDisplay camera-policy
 * viewports. It deliberately lives in URSoccerLab: nDisplay is an optional
 * presentation backend and must not become a required dependency of URLab.
 */
UCLASS(ClassGroup = (URS), meta = (BlueprintSpawnableComponent))
class URSOCCERLAB_API UURSDisplayClusterCameraBinderComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UURSDisplayClusterCameraBinderComponent();

protected:
	virtual void BeginPlay() override;
	virtual void TickComponent(
		float DeltaTime,
		ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

private:
	bool TryBindCameras();

	UPROPERTY(Transient)
	TArray<TObjectPtr<UCameraComponent>> CameraProxies;

	bool bBound = false;
	int32 RequestedCameraCount = 4;
	FString RequestedCameraName;
};
