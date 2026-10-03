#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Transport/URSTcpProtocol.h"
#include "Vision/URSImageEncoder.h"
#include "URSTcpTransportComponent.generated.h"
class UURSRobotCoreComponent;
class UURSCameraStreamComponent;
class IURSNetworkService;
UCLASS(ClassGroup = (URSoccerLab), meta = (BlueprintSpawnableComponent))
class URSOCCERLAB_API UURSTcpTransportComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UURSTcpTransportComponent();
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab") bool bAutoStart = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab") int32 RobotBasePort = 10000;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab") int32 AdminPort = 11000;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab") double StateRateHz = 60;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* TickFunction) override;

private:
	TWeakObjectPtr<UURSRobotCoreComponent> Core;
	TWeakObjectPtr<UURSCameraStreamComponent> CameraStream;
	FDelegateHandle FrameHandle;
	IURSNetworkService* NetThread = nullptr;
	bool bStarted = false;
	bool StartTransport();
	void StopTransport();
	void RebuildNetworkThread();
	UFUNCTION() void OnRobotsChanged();
	void SendCameraFrame(const URSoccerLab::FEncodedCameraFrame& Frame);
};
