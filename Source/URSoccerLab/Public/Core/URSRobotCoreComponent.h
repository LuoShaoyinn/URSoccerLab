#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Scene/URSSceneConfig.h"
#include "URSSnapshot.h"
#include "Core/URSTripleBuffer.h"
#include "Core/URSBuffers.h"
#include "URSRobotCoreComponent.generated.h"

class AAMjManager;
class AMjArticulation;
class UMjActuator;
class UMjJoint;
class UMjCamera;
class UURSTcpTransportComponent;

// Legacy state struct (kept for Blueprint compatibility + camera publish path)
USTRUCT(BlueprintType)
struct FURSCameraInfo
{
	GENERATED_BODY()
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab") FString Name;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab") FString Format;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab") int32 Width = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab") int32 Height = 0;
};

USTRUCT(BlueprintType)
struct FURSRobotState
{
	GENERATED_BODY()
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab") FString ActorId;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab") double SimTime = 0.0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab") bool bCommandTimedOut = false;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab") FVector BasePos = FVector::ZeroVector;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab") FQuat BaseQuat = FQuat::Identity;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab") TArray<double> BaseVel;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab") TArray<FString> JointNames;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab") TArray<double> JointQpos;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab") TArray<double> JointQvel;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab") TArray<FString> ActuatorNames;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab") TArray<double> MotorCommand;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab") TArray<FURSCameraInfo> Cameras;
	bool bHasCameraImu = false;
	FQuat HeadQuat = FQuat::Identity;
	FVector HeadAngVel = FVector::ZeroVector;
	bool bPrivSelfPos = false;
	bool bPrivBallPosRelated = false;
	bool bPrivBallVelRelated = false;
	bool bPrivAllPos = false;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab") FVector SelfPos = FVector::ZeroVector;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab") FVector BallPosRelated = FVector::ZeroVector;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab") FVector BallVelRelated = FVector::ZeroVector;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab") TMap<FString, FVector> AllPos;
	URSoccerLab::FURSNoiseConfig Noise;
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
	friend class UURSTcpTransportComponent;

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

	// Reads from triple buffer (lock-free). Brief lock only for static metadata.
	UFUNCTION(BlueprintCallable, Category = "URSoccerLab")
	bool GetRobotState(const FString& ActorId, FURSRobotState& OutState);

	// Triple-buffer command wrappers (called by transport or network thread)
	void SubmitCommand(const FString& ActorId, const TMap<FString, float>& NamedValues);
	void SubmitControllerParams(const FString& ActorId,
		const TMap<FString, double>* Kp,
		const TMap<FString, double>* Kv,
		const TMap<FString, double>* Damping,
		const FString* ActuatorMode);

	// Camera readback (game thread only — uses UE render API)
	UFUNCTION(BlueprintCallable, Category = "URSoccerLab")
	bool RequestCameraReadback(const FString& ActorId);
	UFUNCTION(BlueprintCallable, Category = "URSoccerLab")
	bool RequestNamedCameraReadback(const FString& ActorId, const FString& CameraName);
	UFUNCTION(BlueprintPure, Category = "URSoccerLab")
	bool IsCameraFrameReady(const FString& ActorId, const FString& CameraName) const;
	UFUNCTION(BlueprintCallable, Category = "URSoccerLab")
	bool ConsumeCameraFrame(const FString& ActorId, const FString& CameraName, TArray<FColor>& OutPixels);
	bool ConsumeDepthCameraFrame(const FString& ActorId, const FString& CameraName, TArray<float>& OutDepthMeters);

	// Pose admin (under CallbackMutex — rare, not on hot path)
	FURSPoseResult SetPose(const FString& ActorId, const FVector* Translation, const FQuat* Rotation, const TArray<float>* JointQpos);
	UFUNCTION(BlueprintCallable, Category = "URSoccerLab") FURSPoseResult GetPose(const FString& ActorId) const;
	UFUNCTION(BlueprintCallable, Category = "URSoccerLab") FURSPoseResult ResetRobot(const FString& ActorId);

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

	// Access for transport to wire triple buffers
	friend class UURSTcpTransportComponent;

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
		int32 JointType = -1;
		int32 QposAdr = -1, QposSize = 0;
		int32 DofAdr = -1, DofSize = 0;
	};
	struct FCameraEntry
	{
		TWeakObjectPtr<UMjCamera> Camera;
		FString Name;
		bool bReadbackRequested = false;
	};

	struct FRobotEndpoint
	{
		FRobotEndpoint() = default;
		FRobotEndpoint(FRobotEndpoint&& Other)
			: ActorId(MoveTemp(Other.ActorId))
			, Articulation(Other.Articulation)
			, Actuators(MoveTemp(Other.Actuators))
			, ActuatorNameToIndex(MoveTemp(Other.ActuatorNameToIndex))
			, Joints(MoveTemp(Other.Joints))
			, RootBodyId(Other.RootBodyId), RootQposAdr(Other.RootQposAdr)
			, Cameras(MoveTemp(Other.Cameras))
			, HeadCameraBodyId(Other.HeadCameraBodyId)
			, PoseLock(MoveTemp(Other.PoseLock))
			, Privilege(MoveTemp(Other.Privilege))
			, Noise(MoveTemp(Other.Noise))
		{}
		FRobotEndpoint& operator=(FRobotEndpoint&& Other)
		{
			ActorId = MoveTemp(Other.ActorId);
			Articulation = Other.Articulation;
			Actuators = MoveTemp(Other.Actuators);
			ActuatorNameToIndex = MoveTemp(Other.ActuatorNameToIndex);
			Joints = MoveTemp(Other.Joints);
			RootBodyId = Other.RootBodyId; RootQposAdr = Other.RootQposAdr;
			Cameras = MoveTemp(Other.Cameras);
			HeadCameraBodyId = Other.HeadCameraBodyId;
			PoseLock = MoveTemp(Other.PoseLock);
			Privilege = MoveTemp(Other.Privilege);
			Noise = MoveTemp(Other.Noise);
			return *this;
		}

		FString ActorId;
		TWeakObjectPtr<AMjArticulation> Articulation;
		TArray<FActuatorInfo> Actuators;
		TMap<FString, int32> ActuatorNameToIndex;
		TArray<FJointInfo> Joints;
		int32 RootBodyId = -1;
		int32 RootQposAdr = -1;
		TArray<FCameraEntry> Cameras;
		int32 HeadCameraBodyId = -1;

		// Triple buffers (shared between physics + network threads)
		URSTripleBuffer<FRobotSnapshot> StateBuffer;
		URSTripleBuffer<FCommandSet>    CmdBuffer;
		URSTripleBuffer<FGainSet>       GainBuffer;

		FPoseLock PoseLock;
		URSoccerLab::FURSPrivilegeConfig Privilege;
		URSoccerLab::FURSNoiseConfig Noise;
	};

	TArray<FRobotEndpoint> Endpoints;
	TArray<FRobotEndpoint>& GetEndpoints() { return Endpoints; }
	TMap<FString, int32> ActorRootBodyIds;
	std::atomic<uint32> EndpointSeq{0};
	TWeakObjectPtr<AAMjManager> Manager;
	TWeakObjectPtr<UObject> SceneConfigComp;
	bool bCallbacksRegistered = false;
	bool bInitialized = false;
	FTimerHandle CompiledSceneRetryTimer;

	UFUNCTION() void OnSceneConfigApplied();
	void RebuildEndpointCache();
	void TryInitializeCompiledScene();
	void InitializeConfiguredRobotPoses();
	void InitializeConfiguredObjectPoses();
	void RegisterPhysicsCallbacks();
	void PreStepPhysics(struct mjModel_* Model, struct mjData_* Data);
	void PublishSnapshot(struct mjModel_* Model, struct mjData_* Data, int32 Ri);
	void ApplyCommands(double NowSec);
	void ApplyGains(struct mjModel_* Model);
	void ApplyPoseLocks(struct mjModel_* Model, struct mjData_* Data);
	FRobotEndpoint* FindEndpoint(const FString& ActorId);
	const FRobotEndpoint* FindEndpoint(const FString& ActorId) const;
	int32 FindEndpointIndex(const FString& ActorId) const;
	struct FQposLayout;
	static FQposLayout DiscoverQposLayout(AMjArticulation* Articulation, const struct mjModel_* Model);
	static int32 DiscoverRootBodyId(AMjArticulation* Articulation, const struct mjModel_* Model);
};
