#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Scene/URSSceneConfig.h"
#include "URSSnapshot.h"
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

	// Camera/head IMU: orientation (world quat) and angular velocity (body
	// frame) of the link that carries the eye cameras. Valid only when
	// bHasCameraImu is true.
	bool bHasCameraImu = false;
	FQuat HeadQuat = FQuat::Identity;
	FVector HeadAngVel = FVector::ZeroVector;

	// Privilege flags mirrored from this robot's scene-config entry. The
	// corresponding fields below are only populated when the flag is set.
	bool bPrivSelfPos = false;
	bool bPrivBallPosRelated = false;
	bool bPrivBallVelRelated = false;
	bool bPrivAllPos = false;

	// Privileged state. self_pos and all_pos are in world metres; ball_pos_related
	// is the ball expressed in the robot's yaw-only frame.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab")
	FVector SelfPos = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab")
	FVector BallPosRelated = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab")
	FVector BallVelRelated = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "URSoccerLab")
	TMap<FString, FVector> AllPos;

	// Per-channel Gaussian noise sigmas mirrored from this robot's scene entry.
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

	void SubmitControllerParams(const FString& ActorId,
		const TMap<FString, double>* Kp,
		const TMap<FString, double>* Kv,
		const TMap<FString, double>* Damping,
		const FString* ActuatorMode);

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
		int32 JointType = -1;
		int32 QposAdr = -1;
		int32 QposSize = 0;
		int32 DofAdr = -1;
		int32 DofSize = 0;
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
			, Articulation(MoveTemp(Other.Articulation))
			, Actuators(MoveTemp(Other.Actuators))
			, ActuatorNameToIndex(MoveTemp(Other.ActuatorNameToIndex))
			, Joints(MoveTemp(Other.Joints))
			, RootBodyId(Other.RootBodyId)
			, RootQposAdr(Other.RootQposAdr)
			, Cameras(MoveTemp(Other.Cameras))
			, HeadCameraBodyId(Other.HeadCameraBodyId)
			, LatestCommand(MoveTemp(Other.LatestCommand))
			, LastNamedValues(MoveTemp(Other.LastNamedValues))
			, LastCommandTimeSec(Other.LastCommandTimeSec.load())
			, bHasCommand(Other.bHasCommand.load())
			, ActuatorMode(Other.ActuatorMode)
			, bGainsDirty(Other.bGainsDirty.load())
			, PoseLock(MoveTemp(Other.PoseLock))
			, Privilege(MoveTemp(Other.Privilege))
			, Noise(MoveTemp(Other.Noise))
		{
			FMemory::Memcpy(KpArr, Other.KpArr, sizeof(KpArr));
			FMemory::Memcpy(KvArr, Other.KvArr, sizeof(KvArr));
			FMemory::Memcpy(DampingArr, Other.DampingArr, sizeof(DampingArr));
		}
		FRobotEndpoint& operator=(FRobotEndpoint&& Other)
		{
			ActorId = MoveTemp(Other.ActorId);
			Articulation = MoveTemp(Other.Articulation);
			Actuators = MoveTemp(Other.Actuators);
			ActuatorNameToIndex = MoveTemp(Other.ActuatorNameToIndex);
			Joints = MoveTemp(Other.Joints);
			RootBodyId = Other.RootBodyId;
			RootQposAdr = Other.RootQposAdr;
			Cameras = MoveTemp(Other.Cameras);
			HeadCameraBodyId = Other.HeadCameraBodyId;
			LatestCommand = MoveTemp(Other.LatestCommand);
			LastNamedValues = MoveTemp(Other.LastNamedValues);
			LastCommandTimeSec.store(Other.LastCommandTimeSec.load());
			bHasCommand.store(Other.bHasCommand.load());
			ActuatorMode = Other.ActuatorMode;
			FMemory::Memcpy(KpArr, Other.KpArr, sizeof(KpArr));
			FMemory::Memcpy(KvArr, Other.KvArr, sizeof(KvArr));
			FMemory::Memcpy(DampingArr, Other.DampingArr, sizeof(DampingArr));
			bGainsDirty.store(Other.bGainsDirty.load());
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

		// Resolved body id of the link carrying the eye cameras (the
		// normalized "head link"), used for the camera_imu state. -1 if none.
		int32 HeadCameraBodyId = -1;

		TArray<float> LatestCommand;
		TMap<FString, float> LastNamedValues;
		std::atomic<double> LastCommandTimeSec{0.0};
		std::atomic<bool> bHasCommand{false};

		// --- Controller parameters (runtime-tunable via JSON) ---
		enum class EURSActuatorMode : uint8 { Torque, Position };
		static constexpr int32 MAX_GAINS = 40;
		double KpArr[MAX_GAINS] = {0};
		double KvArr[MAX_GAINS] = {0};
		double DampingArr[MAX_GAINS] = {0};
		EURSActuatorMode ActuatorMode = EURSActuatorMode::Position;
		std::atomic<bool> bGainsDirty{false};

		FPoseLock PoseLock;
		URSoccerLab::FURSPrivilegeConfig Privilege;
		URSoccerLab::FURSNoiseConfig Noise;
	};

	TArray<FRobotEndpoint> Endpoints;
	// One triple buffer per robot (physics writes, game reads, lock-free)
	TArray<TTripleBuffer<FRobotSnapshot>> Snapshots;
	TMap<FString, int32> ActorRootBodyIds;
	// Sequence lock for endpoint array rebuilds (rare). Physics thread reads
	// seq before/after iteration; if changed, skips that step.
	std::atomic<uint32> EndpointSeq{0};
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
	void InitializeConfiguredObjectPoses();
	void RegisterPhysicsCallbacks();
	void PreStepPhysics(struct mjModel_* Model, struct mjData_* Data);
	void PublishSnapshots(struct mjModel_* Model, struct mjData_* Data);

	void ApplyCommands(double NowSec);
	void ApplyControllerGains(struct mjModel_* Model);
	void ApplyPoseLocks(struct mjModel_* Model, struct mjData_* Data);

	FRobotEndpoint* FindEndpoint(const FString& ActorId);
	const FRobotEndpoint* FindEndpoint(const FString& ActorId) const;

	struct FQposLayout;
	static FQposLayout DiscoverQposLayout(AMjArticulation* Articulation, const struct mjModel_* Model);
	static int32 DiscoverRootBodyId(AMjArticulation* Articulation, const struct mjModel_* Model);
};
