#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Inspector/URSInspectorNetwork.h"
#include "URSInspectorTransportComponent.generated.h"
class UURSInspectorCameraComponent;
class UURSRobotCoreComponent;
UCLASS()
class URSOCCERLAB_API UURSInspectorTransportComponent : public UActorComponent
{
 GENERATED_BODY()
public:
 UURSInspectorTransportComponent();
 virtual void BeginPlay() override;
 virtual void EndPlay(const EEndPlayReason::Type Reason) override;
 virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* Function) override;
private:
 TUniquePtr<IURSInspectorNetwork> Network;
 TWeakObjectPtr<UURSInspectorCameraComponent> Camera;
 TWeakObjectPtr<UURSRobotCoreComponent> Core;
 FDelegateHandle FrameHandle;
 bool Enabled = true; int32 Capacity = 4;
 int32 Port = 12000; bool Failed = false;
 UFUNCTION() void OnRobotsChanged();
 void Send(uint64 Id, const URSoccerLab::FEncodedCameraFrame& Frame);
};
