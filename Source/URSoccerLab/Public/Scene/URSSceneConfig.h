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
	Av1,
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
	int32 BitrateKbps = 2000;
	double KeyframeIntervalSeconds = 2.0;
	FString VulkanDevice;
};

struct URSOCCERLAB_API FURSGuestInspectorConfig
{
 FURSGuestInspectorConfig() { Rgb.Compression = EURSRgbCompression::Av1; }
 bool bEnabled = true;
 int32 Port = 12000;
 int32 MaxGuests = 4;
 int32 Width = 640, Height = 480;
 double FovDegrees = 90;
 FURSRgbStreamConfig Rgb;
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
	double MotionBlurAmount = 0.5;
	double MotionBlurMaxPercent = 5.0;
	// 0 follows the camera's configured frame rate.
	int32 MotionBlurTargetFps = 0;
	double FilmGrainIntensity = 0.0;
	double FilmGrainShadows = 1.0;
	double FilmGrainMidtones = 1.0;
	double FilmGrainHighlights = 1.0;
	double FilmGrainTexelSize = 1.0;
	bool bAutoExposure = false;
	double ExposureCompensation = 0.0;
	TOptional<int32> ResolutionX;   // e.g. 640
	TOptional<int32> ResolutionY;   // e.g. 480
};

struct URSOCCERLAB_API FURSLightingConfig
{
	bool bIsSet = false;
	double LampIntensityLumens = 0.0;
	TOptional<double> EmissiveIntensity; // Absolute linear emission value; omitted leaves material emission unchanged.
	double SourceRadiusCm = 60.0;
	double SpecularScale = 0.0;
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

// External texture paths are relative to the scene JSON. No authored maps are cooked.
struct URSOCCERLAB_API FURSPBRVisualConfig
{
	FString BaseColorMap;
	FString NormalMap;
	FString RoughnessMap;
	FString MetallicMap;
	FString AoMap;
	double NormalStrength = 1.0;
	double Roughness = 0.8;
	double Metallic = 0.0;
	// Tangent-space DirectX normals by default; OpenGL flips the green channel.
	bool bNormalOpenGL = false;
};

struct URSOCCERLAB_API FURSFieldVisualConfig : FURSPBRVisualConfig
{
	double DetailTileSizeM = 0.5;
};

struct URSOCCERLAB_API FURSFieldPhysicsConfig
{
	TArray<float> Friction = {1.0f, 0.005f, 0.0001f};
	int32 Condim = 3;
	TArray<float> Solref = {0.02f, 1.0f};
	TArray<float> Solimp = {0.9f, 0.95f, 0.001f, 0.5f, 2.0f};
};

struct URSOCCERLAB_API FURSFieldConfig
{
	bool bIsSet = false;
	double LengthM = 9.0;
	double WidthM = 6.0;
	double BorderXM = 0.8;
	double BorderYM = 0.9;
	FURSFieldVisualConfig Visual;
	FURSFieldPhysicsConfig Physics;
};

struct URSOCCERLAB_API FURSGoalPose
{
	FVector TranslationMeters = FVector::ZeroVector;
	double YawDeg = 0.0;
};

struct URSOCCERLAB_API FURSGoalsConfig
{
	bool bIsSet = false;
	double WidthM = 1.8;
	double HeightM = 1.2;
	double PostRadiusM = 0.05;
	TArray<FURSGoalPose> Poses;
};

struct URSOCCERLAB_API FURSBallPhysicsConfig
{
	bool bIsSet = false;
	double RadiusM = 0.075;
	double MassKg = 0.2;
	TArray<float> Friction = {0.8f, 0.02f, 0.03f};
	TArray<float> Solref = {-5000.0f, -20.0f};
};

struct URSOCCERLAB_API FURSObjectSpawn
{
	FURSBallPhysicsConfig Physics;
	TOptional<FURSPBRVisualConfig> Visual;
	FString ActorId;
	FString Type;
	TOptional<FVector> TranslationMeters;
	TOptional<FQuat> RotationQuatXyzw;
};

struct URSOCCERLAB_API FURSSceneConfig
{
	FString Version = TEXT("urs_scene_v1");
	FURSFieldConfig Field;
	FURSGoalsConfig Goals;
	FString SourceDirectory;
	FURSVisionConfig Vision;
	FURSGuestInspectorConfig GuestInspector;
	FURSRenderConfig Render;
	FURSLightingConfig Lighting;

	// Physics timestep override. 0 = use MJCF default.
	double MujocoDt = 0.0;

	// State publish rate (Hz). 0 = use transport default (60).
	double StateFreq = 0.0;

	// Camera publish rate (Hz). 0 = use vision config default.
	double CameraFreq = 0.0;

	// External manifests, resolved relative to the scene JSON.
	TMap<FString, FString> RobotTypes;
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
