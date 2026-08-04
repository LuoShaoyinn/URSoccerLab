#pragma once

#include "CoreMinimal.h"
#include "Scene/URSObjectTypeRegistry.h"
#include "Scene/URSRobotTypeRegistry.h"

namespace URSoccerLab
{
enum class EURSVisionMode : uint8
{
	StereoRgb,
	Rgbd,
};

enum class EURSRgbCompression : uint8
{
	Raw,
	Jpeg,
};

enum class EURSDepthCompression : uint8
{
	RawFloat32,
	RawUint16Millimeters,
	ZlibUint16Millimeters,
};

struct URSOCCERLAB_API FURSRgbStreamConfig
{
	double RateHz = 30.0;
	EURSRgbCompression Compression = EURSRgbCompression::Jpeg;
	int32 JpegQuality = 85;
};

struct URSOCCERLAB_API FURSDepthStreamConfig
{
	double RateHz = 15.0;
	EURSDepthCompression Compression = EURSDepthCompression::ZlibUint16Millimeters;
	// uint16 millimeters represents at most 65.535 metres.
	double MaxDepthMeters = 65.535;
};

struct URSOCCERLAB_API FURSVisionConfig
{
	EURSVisionMode Mode = EURSVisionMode::StereoRgb;
	FString LeftCamera = TEXT("left_eye");
	FString RightCamera = TEXT("right_eye");
	FURSRgbStreamConfig Rgb;
	FURSDepthStreamConfig Depth;
};

struct URSOCCERLAB_API FURSRenderConfig
{
	// True when a "render" block was present in the JSON. When false the
	// engine's render settings are left untouched.
	bool bIsSet = false;
	// Master switch. When false a minimal render preset is applied (lowest
	// resolution, no GI/reflections/shadows/motion-blur). When true the
	// feature flags below are applied.
	bool bEnable = true;
	bool bLumen = true;               // Lumen GI + reflections
	bool bHardwareRayTracing = false;  // r.RayTracing + Lumen hardware RT
	FString AntiAliasing = TEXT("tsr"); // none|fxaa|taa|tsr
	double ScreenPercentage = 100.0;  // 10..200
	int32 ShadowQuality = 3;          // 0..5
	bool bMotionBlur = false;
	bool bAutoExposure = false;
	double ExposureCompensation = 0.0;
	TOptional<int32> ResolutionX;   // e.g. 640
	TOptional<int32> ResolutionY;   // e.g. 480
};

struct URSOCCERLAB_API FURSPrivilegeConfig
{
	// Emit the robot's own world base position in the state JSON.
	bool bSelfPos = false;
	// Emit the ball position expressed in the robot's yaw-only frame.
	bool bBallPosRelated = false;
	// Emit the ball linear velocity expressed in the robot's yaw-only frame.
	bool bBallVelRelated = false;
	// Emit every actor's world position (robots + objects).
	bool bAllPos = false;
};

struct URSOCCERLAB_API FURSNoiseConfig
{
	// Per-channel Gaussian noise standard deviation (0 disables that channel).
	double Qpos = 0.0;          // joint positions (rad)
	double Qvel = 0.0;          // joint velocities (rad/s)
	double Qtor = 0.0;          // actuator command / torque feedback
	double ImuQuat = 0.0;       // base orientation quaternion components
	double ImuAngVel = 0.0;     // base angular velocity (rad/s)
	double CameraImuQuat = 0.0; // head/camera link orientation quaternion
	double CameraImuAngVel = 0.0; // head/camera link angular velocity (rad/s)
	double SelfPos = 0.0;       // privileged self world pos (m)
	double BallPosRelated = 0.0;// privileged ball-in-yaw-frame pos (m)
	double BallVelRelated = 0.0;// privileged ball-in-yaw-frame vel (m/s)
	double AllPos = 0.0;        // privileged all-actor world pos (m)
};

struct URSOCCERLAB_API FURSRobotSpawn
{
	FString ActorId;
	FString Type;
	TOptional<FVector> TranslationMeters;
	TOptional<FQuat> RotationQuatXyzw;
	// Optional named qpos values for every non-root joint of this robot type.
	TOptional<TMap<FString, float>> JointPositionsRad;
	// Optional privileged state exposed through the robot's state JSON.
	FURSPrivilegeConfig Privilege;
	// Optional Gaussian observation noise injected into the state JSON.
	FURSNoiseConfig Noise;
};

struct URSOCCERLAB_API FURSObjectSpawn
{
	FString ActorId;
	FString Type;
	TOptional<FVector> TranslationMeters;
	TOptional<FQuat> RotationQuatXyzw;
};

struct URSOCCERLAB_API FURSSceneConfig
{
	FString Version = TEXT("urs_scene_v1");
	FURSVisionConfig Vision;
	FURSRenderConfig Render;
	TArray<FURSRobotSpawn> Robots;
	TArray<FURSObjectSpawn> Objects;
};

struct URSOCCERLAB_API FURSSceneConfigValidationResult
{
	bool bOk = true;
	TArray<FString> Errors;
};

class URSOCCERLAB_API FURSSceneConfigIo
{
public:
	static bool LoadFromFile(const FString& AbsPath, FURSSceneConfig& Out, FString& OutError);
	static bool WriteToFile(const FString& AbsPath, const FURSSceneConfig& In, FString& OutError);

	static FURSSceneConfig MakeDefault();
	static FURSSceneConfigValidationResult Validate(const FURSSceneConfig& Config);
};
} // namespace URSoccerLab
