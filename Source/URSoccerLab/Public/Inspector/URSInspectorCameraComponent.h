#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Inspector/URSInspectorProtocol.h"
#include "Vision/URSImageEncoder.h"
#include "Async/Future.h"
#include "Containers/Queue.h"
#include "URSInspectorCameraComponent.generated.h"
class USceneCaptureComponent2D;
class UTextureRenderTarget2D;
class IImageWrapperModule;
struct FURSInspectorReadback;
DECLARE_MULTICAST_DELEGATE_TwoParams(FURSInspectorFrame, uint64, const URSoccerLab::FEncodedCameraFrame&);
UCLASS()
class URSOCCERLAB_API UURSInspectorCameraComponent : public UActorComponent
{
 GENERATED_BODY()
public:
 UURSInspectorCameraComponent();
 virtual void BeginPlay() override;
 virtual void EndPlay(const EEndPlayReason::Type Reason) override;
 virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* Function) override;
 bool SetPose(uint64 Session, const URSoccerLab::FInspectorPose& Pose);
 void Remove(uint64 Session);
 void Reset();
 FURSInspectorFrame OnFrame;
private:
 UPROPERTY() TMap<uint64, TObjectPtr<USceneCaptureComponent2D>> Captures;
 UPROPERTY() TMap<uint64, TObjectPtr<UTextureRenderTarget2D>> Targets;
 struct FSession
 {
  TSharedPtr<FURSInspectorReadback, ESPMode::ThreadSafe> Readback;
  bool Busy = false; double NextCapture = 0; uint32 Sequence = 0;
 };
 TMap<uint64, FSession> Sessions;
 uint32 Generation = 0;
 struct FCompleted { uint32 Generation; uint64 Session; URSoccerLab::FEncodedCameraFrame Frame; };
 struct FMailbox { TQueue<FCompleted, EQueueMode::Mpsc> Frames; };
 TSharedPtr<FMailbox, ESPMode::ThreadSafe> Mailbox;
 TArray<TFuture<void>> Jobs;
 IImageWrapperModule* EncoderModule = nullptr;
 bool Jpeg = true;
};
