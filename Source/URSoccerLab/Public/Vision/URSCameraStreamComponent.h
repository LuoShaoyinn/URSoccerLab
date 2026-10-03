#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Containers/Queue.h"
#include "Async/Future.h"
#include "Scene/URSSceneConfig.h"
#include "Vision/URSImageEncoder.h"
#include "URSCameraStreamComponent.generated.h"
class UURSRobotCoreComponent;
class UURSDisplayClusterCameraBinderComponent;
class IImageWrapperModule;
DECLARE_MULTICAST_DELEGATE_OneParam(FURSOnEncodedFrame, const URSoccerLab::FEncodedCameraFrame&);
UCLASS(ClassGroup = (URSoccerLab), meta = (BlueprintSpawnableComponent))
class URSOCCERLAB_API UURSCameraStreamComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UURSCameraStreamComponent();
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* TickFunction) override;
	FURSOnEncodedFrame OnEncodedFrame;
	bool IsLayoutReady() const;
	void SetCaptureDemand(bool bEnabled)
	{
		bCaptureDemand = bEnabled;
	}
	void SetCameraRate(double Rate)
	{
		CameraRateHz = FMath::Clamp(Rate, 1.0, 120.0);
	}

private:
	TWeakObjectPtr<UURSRobotCoreComponent> Core;
	TWeakObjectPtr<UURSDisplayClusterCameraBinderComponent> NDisplayBinder;
	URSoccerLab::FURSVisionConfig VisionConfig;
	IImageWrapperModule* ImageWrapperModule = nullptr;
	double CameraRateHz = 30, NextRgbTimeSec = 0;
	FString CameraCompress = TEXT("jpeg");
	int32 JpegQuality = 85;
	uint32 CaptureGeneration = 0, Sequence = 0;
	bool bCaptureDemand = false;
	struct FCameraState
	{
		FString ActorId;
		bool bRgbEncodeInFlight = false;
		uint64 LastNDisplayRgbSequence = 0;
	};
	TArray<FCameraState> CameraStates;
	struct FCompletedFrame
	{
		uint32 Generation = 0;
		URSoccerLab::FEncodedCameraFrame Frame;
	};
	struct FMailbox
	{
		TQueue<FCompletedFrame, EQueueMode::Mpsc> Frames;
	};
	TSharedPtr<FMailbox, ESPMode::ThreadSafe> Mailbox;
	TArray<TFuture<void>> EncodeJobs;
	UFUNCTION() void OnRobotsChanged();
	void TickCameraCapture();
	void DrainCompletedFrames();
};
