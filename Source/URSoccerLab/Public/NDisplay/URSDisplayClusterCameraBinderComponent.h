#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "URSDisplayClusterCameraBinderComponent.generated.h"

class UCameraComponent;
class FRDGBuilder;
class FRDGTexture;
class SWindow;

/**
 * Production adapter that binds URLab MuJoCo cameras to nDisplay camera-policy
 * viewports and asynchronously reads the composited atlas back once per sensor
 * sample. The TCP transport then slices named camera images from that atlas.
 */
UCLASS(ClassGroup = (URS), meta = (BlueprintSpawnableComponent))
class URSOCCERLAB_API UURSDisplayClusterCameraBinderComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UURSDisplayClusterCameraBinderComponent();
	virtual ~UURSDisplayClusterCameraBinderComponent() override;

	bool IsReady() const { return bBound; }
	bool RequestRgbFrame();
	uint64 GetLatestRgbFrameSequence() const { return LatestAtlasSequence; }
	bool CopyRgbFrame(
		const FString& ActorId,
		const FString& CameraName,
		uint64 MinimumSequence,
		TArray<FColor>& OutPixels,
		int32& OutWidth,
		int32& OutHeight,
		uint64& OutSequence) const;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(
		float DeltaTime,
		ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

private:
	struct FReadbackSlot;
	struct FCompletedAtlas
	{
		FIntPoint Size = FIntPoint::ZeroValue;
		TArray<FColor> Pixels;
	};

	bool TryBindCameras();
	void OnBackBufferReady_RenderThread(
		FRDGBuilder& GraphBuilder,
		SWindow& Window,
		FRDGTexture* BackBuffer);
	void DrainCompletedAtlases();
	static FString CameraKey(const FString& ActorId, const FString& CameraName);

	UPROPERTY(Transient)
	TArray<TObjectPtr<UCameraComponent>> CameraProxies;

	TArray<TSharedPtr<FReadbackSlot, ESPMode::ThreadSafe>> ReadbackSlots;
	TQueue<FCompletedAtlas, EQueueMode::Mpsc> CompletedAtlases;
	TMap<FString, FIntRect> CameraRects;
	FIntPoint LatestAtlasSize = FIntPoint::ZeroValue;
	TArray<FColor> LatestAtlasPixels;
	FDelegateHandle BackBufferDelegateHandle;
	TAtomic<bool> bReadbackRequested{false};
	TAtomic<int32> OutstandingReadbacks{0};
	uint64 LatestAtlasSequence = 0;
	bool bBound = false;
	int32 RequestedCameraCount = 4;
	FString RequestedCameraName;
};
