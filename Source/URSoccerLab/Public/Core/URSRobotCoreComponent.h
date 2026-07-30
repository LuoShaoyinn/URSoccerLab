#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "URSRobotCoreComponent.generated.h"

class AAMjManager;
class AMjArticulation;
class UMjActuator;
class UMjJoint;
class UMjCamera;

USTRUCT(BlueprintType)
struct FURSCameraInfo
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab")
	FString Name;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab")
	FString Format;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab")
	int32 Width = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab")
	int32 Height = 0;
};

USTRUCT(BlueprintType)
struct FURSRobotState
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab")
	FString ActorId;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab")
	double SimTime = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab")
	bool bCommandTimedOut = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab")
	FVector BasePos = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab")
	FQuat BaseQuat = FQuat::Identity;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab")
	TArray<double> BaseVel;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab")
	TArray<FString> JointNames;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab")
	TArray<double> JointQpos;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab")
	TArray<double> JointQvel;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab")
	TArray<FString> ActuatorNames;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab")
	TArray<double> MotorCommand;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab")
	TArray<FURSCameraInfo> Cameras;
};

USTRUCT(BlueprintType)
struct FURSPoseResult
{
	GENERATED_BODY()

	bool bOk = false;
	FString Error;
	FString Message;
	FVector AppliedTranslation = FVector::ZeroVector;
	FQuat AppliedRotation = FQuat::Identity;
	TArray<float> AppliedJointQpos;
	double SimTime = 0.0;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnCoreRobotsChanged);

UCLASS(ClassGroup = (URSoccerLab), meta = (BlueprintSpawnableComponent))
class URSOCCERLAB_API UURSRobotCoreComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UURSRobotCoreComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab")
	bool bAutoStart = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab", meta = (ClampMin = "0.0"))
	double CommandTimeoutSec = 0.1;

	UPROPERTY(BlueprintAssignable, Category = "URSoccerLab")
	FOnCoreRobotsChanged OnRobotsChanged;

	UFUNCTION(BlueprintCallable, Category = "URSoccerLab")
	bool Initialize();

	UFUNCTION(BlueprintCallable, Category = "URSoccerLab")
	TArray<FString> GetRobotIds() const;

	UFUNCTION(BlueprintCallable, Category = "URSoccerLab")
	bool GetRobotState(const FString& ActorId, FURSRobotState& OutState);

	UFUNCTION(BlueprintCallable, Category = "URSoccerLab")
	void SubmitCommand(const FString& ActorId, const TMap<FString, float>& NamedValues);

	UFUNCTION(BlueprintCallable, Category = "URSoccerLab")
	bool RequestCameraReadback(const FString& ActorId);

	UFUNCTION(BlueprintCallable, Category = "URSoccerLab")
	bool RequestNamedCameraReadback(const FString& ActorId, const FString& CameraName);

	UFUNCTION(BlueprintPure, Category = "URSoccerLab")
	bool IsCameraFrameReady(const FString& ActorId, const FString& CameraName) const;

	UFUNCTION(BlueprintCallable, Category = "URSoccerLab")
	bool ConsumeCameraFrame(const FString& ActorId, const FString& CameraName, TArray<FColor>& OutPixels);

	bool ConsumeDepthCameraFrame(const FString& ActorId, const FString& CameraName, TArray<float>& OutDepthMeters);

	FURSPoseResult SetPose(const FString& ActorId, const FVector* Translation, const FQuat* Rotation, const TArray<float>* JointQpos);

	UFUNCTION(BlueprintCallable, Category = "URSoccerLab")
	FURSPoseResult GetPose(const FString& ActorId) const;

	UFUNCTION(BlueprintCallable, Category = "URSoccerLab")
	FURSPoseResult ResetRobot(const FString& ActorId);

	// Pose-lock: when enabled, the last SetPose is re-applied every physics
	// step so the robot is frozen at the target pose regardless of physics
	// forces.  Used by the head demo for base-orientation sweeps.
	struct FPoseLock
	{
		bool bActive = false;
		FVector Translation = FVector::ZeroVector;
		FQuat Rotation = FQuat::Identity;
		TArray<float> JointQpos;
	};

	FURSPoseResult SetPoseLock(const FString& ActorId, bool bLock,
		const FVector* Trans = nullptr, const FQuat* Rot = nullptr,
		const TArray<float>* JointQpos = nullptr);

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	struct FActuatorInfo
	{
		TWeakObjectPtr<UMjActuator> Actuator;
		FString Name;
		int32 MjId = -1;
	};

	struct FJointInfo
	{
		TWeakObjectPtr<UMjJoint> Joint;
		FString Name;
		int32 MjId = -1;
	};

	struct FCameraEntry
	{
		TWeakObjectPtr<UMjCamera> Camera;
		FString Name;
		bool bReadbackRequested = false;
	};

	struct FRobotEndpoint
	{
		FString ActorId;
		TWeakObjectPtr<AMjArticulation> Articulation;

		TArray<FActuatorInfo> Actuators;
		TMap<FString, int32> ActuatorNameToIndex;

		TArray<FJointInfo> Joints;

		TArray<FCameraEntry> Cameras;

		TArray<float> LatestCommand;
		TMap<FString, float> LastNamedValues;
		double LastCommandTimeSec = 0.0;
		bool bHasCommand = false;

		FPoseLock PoseLock;
	};

	TArray<FRobotEndpoint> Endpoints;
	TWeakObjectPtr<AAMjManager> Manager;
	TWeakObjectPtr<UObject> SceneConfigComp;
	bool bCallbacksRegistered = false;
	bool bInitialized = false;
	FTimerHandle CompiledSceneRetryTimer;

	UFUNCTION()
	void OnSceneConfigApplied();

	void RebuildEndpointCache();
	void TryInitializeCompiledScene();
	void InitializeConfiguredRobotPoses();
	void RegisterPhysicsCallbacks();
	void PreStepPhysics(struct mjModel_* Model, struct mjData_* Data);

	void ApplyCommands(double NowSec);
	void ApplyPoseLocks(struct mjModel_* Model, struct mjData_* Data);

	FRobotEndpoint* FindEndpoint(const FString& ActorId);
	const FRobotEndpoint* FindEndpoint(const FString& ActorId) const;

	struct FQposLayout;
	static FQposLayout DiscoverQposLayout(AMjArticulation* Articulation, const struct mjModel_* Model);
	static int32 DiscoverRootBodyId(AMjArticulation* Articulation, const struct mjModel_* Model);
};
