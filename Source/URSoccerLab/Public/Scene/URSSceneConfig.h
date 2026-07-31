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

struct URSOCCERLAB_API FURSRobotSpawn
{
	FString ActorId;
	FString Type;
	TOptional<FVector> TranslationMeters;
	TOptional<FQuat> RotationQuatXyzw;
	// Optional named qpos values for every non-root joint of this robot type.
	TOptional<TMap<FString, float>> JointPositionsRad;
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
