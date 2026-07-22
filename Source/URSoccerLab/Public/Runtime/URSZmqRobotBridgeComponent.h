#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Runtime/URSRobotProtocol.h"
#include "Runtime/URSAdminProtocol.h"
#include "URSZmqRobotBridgeComponent.generated.h"

class AAMjManager;
class AMjArticulation;
class UMjActuator;
class UURSSceneConfigComponent;

USTRUCT(BlueprintType)
struct FURSRobotEndpointInfo
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab|ZMQ")
	FString RobotName;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab|ZMQ")
	FString CommandEndpoint;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab|ZMQ")
	FString StateTopic;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab|ZMQ")
	TArray<FString> ActuatorNames;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab|ZMQ")
	TArray<int32> ActuatorIds;
};

USTRUCT(BlueprintType)
struct FURSAdminEndpointInfo
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab|ZMQ")
	FString RobotName;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab|ZMQ")
	FString AdminEndpoint;
};

UCLASS(ClassGroup = (URSoccerLab), meta = (BlueprintSpawnableComponent))
class URSOCCERLAB_API UURSZmqRobotBridgeComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UURSZmqRobotBridgeComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab|ZMQ")
	bool bAutoStart = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab|ZMQ")
	bool bUsePhysicsCallbacks = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab|ZMQ", meta = (ClampMin = "10000", ClampMax = "65535"))
	int32 CommandBasePort = URSoccerLab::DefaultCommandBasePort;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab|ZMQ", meta = (ClampMin = "10000", ClampMax = "65535"))
	int32 StatePort = URSoccerLab::DefaultStatePort;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab|ZMQ", meta = (ClampMin = "10000", ClampMax = "65535"))
	int32 MetaPort = URSoccerLab::DefaultMetaPort;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab|ZMQ", meta = (ClampMin = "0.0"))
	double CommandTimeoutSec = 0.1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab|ZMQ", meta = (ClampMin = "1.0", ClampMax = "1000.0"))
	double StatePublishRateHz = 60.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab|ZMQ", meta = (ClampMin = "0.1"))
	double MetaPublishIntervalSec = 1.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab|Admin", meta = (ClampMin = "11000", ClampMax = "65535"))
	int32 AdminBasePort = URSoccerLab::DefaultAdminBasePort;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab|ZMQ")
	TArray<FString> RobotNames;

	UFUNCTION(BlueprintCallable, Category = "URSoccerLab|ZMQ")
	bool StartBridge();

	UFUNCTION(BlueprintCallable, Category = "URSoccerLab|ZMQ")
	void StopBridge();

	UFUNCTION(BlueprintCallable, Category = "URSoccerLab|ZMQ")
	bool RebuildEndpointCache();

	UFUNCTION(BlueprintCallable, Category = "URSoccerLab|ZMQ")
	int32 DrainCommandSockets();

	UFUNCTION(BlueprintCallable, Category = "URSoccerLab|ZMQ")
	void PublishMetadata();

	UFUNCTION(BlueprintCallable, Category = "URSoccerLab|ZMQ")
	void PublishState();

	UFUNCTION(BlueprintCallable, Category = "URSoccerLab|Admin")
	int32 DrainAdminSockets();

	UFUNCTION(BlueprintPure, Category = "URSoccerLab|Admin")
	const TArray<FURSAdminEndpointInfo>& GetAdminEndpointInfo() const { return AdminEndpointInfo; }

	UFUNCTION(BlueprintPure, Category = "URSoccerLab|ZMQ")
	const TArray<FURSRobotEndpointInfo>& GetEndpointInfo() const { return EndpointInfo; }

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	UFUNCTION()
	void OnSceneConfigApplied();
	void PullRobotNamesFromSceneConfig();
	struct FRobotRuntimeEndpoint
	{
		FString RobotName;
		FString CommandEndpoint;
		FString AdminEndpoint;
		FString StateTopic;
		TWeakObjectPtr<AMjArticulation> Articulation;
		TArray<TWeakObjectPtr<UMjActuator>> Actuators;
		TArray<TWeakObjectPtr<class UMjJoint>> Joints;
		TArray<FString> ActuatorNames;
		TArray<int32> ActuatorIds;
		TArray<FString> JointNames;
		TArray<int32> JointIds;
		URSoccerLab::FMotorCommandBuffer CommandBuffer;
		void* CommandSocket = nullptr;
		void* AdminSocket = nullptr;
	};

	UPROPERTY(Transient)
	TArray<FURSRobotEndpointInfo> EndpointInfo;

	UPROPERTY(Transient)
	TArray<FURSAdminEndpointInfo> AdminEndpointInfo;

	TArray<FRobotRuntimeEndpoint> RuntimeEndpoints;
	TWeakObjectPtr<AAMjManager> Manager;
	TWeakObjectPtr<UURSSceneConfigComponent> SceneConfig;
	void* ZmqContext = nullptr;
	void* StatePublisher = nullptr;
	void* MetaPublisher = nullptr;
	bool bBridgeStarted = false;
	bool bCallbacksRegistered = false;
	double LastStatePublishSec = 0.0;
	double LastMetaPublishSec = 0.0;

	URSoccerLab::FRobotRuntimeConfig MakeRuntimeConfig() const;
	bool BindCommandSockets();
	bool BindAdminSockets();
	bool BindPublisherSockets();
	void CloseCommandSockets();
	void CloseAdminSockets();
	void ClosePublisherSockets();
	void ApplyLatestCommands(double NowSec);
	void RegisterPhysicsCallbacks();
	void PreStepPhysics(struct mjModel_* Model, struct mjData_* Data);
	void PostStepPhysics(struct mjModel_* Model, struct mjData_* Data);
	void PublishStateFromData(struct mjModel_* Model, struct mjData_* Data, double NowSec);
	bool SendJsonMessage(void* Socket, const FString& Topic, const FString& Json) const;
	FString HandleAdminRequest(FRobotRuntimeEndpoint& Endpoint, const FString& RequestBody);
	FString HandleSetPose(FRobotRuntimeEndpoint& Endpoint, const URSoccerLab::FAdminPoseRequest& Req);
	FString HandleGetPose(FRobotRuntimeEndpoint& Endpoint);
	FString HandleReset(FRobotRuntimeEndpoint& Endpoint);
};
