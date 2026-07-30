#include "Scene/URSSceneConfig.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace URSoccerLab
{
namespace
{
bool IsFiniteVec(const FVector& V)
{
	return FMath::IsFinite(V.X) && FMath::IsFinite(V.Y) && FMath::IsFinite(V.Z);
}

bool IsValidQuat(const FQuat& Q)
{
	return FMath::IsFinite(Q.X) && FMath::IsFinite(Q.Y)
		&& FMath::IsFinite(Q.Z) && FMath::IsFinite(Q.W)
		&& !(Q.X == 0.0 && Q.Y == 0.0 && Q.Z == 0.0 && Q.W == 0.0);
}

bool ReadVec3(const TArray<TSharedPtr<FJsonValue>>* Arr, FVector& Out)
{
	if (!Arr || Arr->Num() != 3)
	{
		return false;
	}
	Out.X = static_cast<double>((*Arr)[0]->AsNumber());
	Out.Y = static_cast<double>((*Arr)[1]->AsNumber());
	Out.Z = static_cast<double>((*Arr)[2]->AsNumber());
	return IsFiniteVec(Out);
}

bool ReadQuatXyzw(const TArray<TSharedPtr<FJsonValue>>* Arr, FQuat& Out)
{
	if (!Arr || Arr->Num() != 4)
	{
		return false;
	}
	Out.X = static_cast<double>((*Arr)[0]->AsNumber());
	Out.Y = static_cast<double>((*Arr)[1]->AsNumber());
	Out.Z = static_cast<double>((*Arr)[2]->AsNumber());
	Out.W = static_cast<double>((*Arr)[3]->AsNumber());
	return IsValidQuat(Out);
}

bool ParseVisionMode(const FString& Value, EURSVisionMode& Out)
{
	if (Value == TEXT("stereo_rgb"))
	{
		Out = EURSVisionMode::StereoRgb;
		return true;
	}
	if (Value == TEXT("rgbd"))
	{
		Out = EURSVisionMode::Rgbd;
		return true;
	}
	return false;
}

bool ParseRgbCompression(const FString& Value, EURSRgbCompression& Out)
{
	if (Value == TEXT("raw"))
	{
		Out = EURSRgbCompression::Raw;
		return true;
	}
	if (Value == TEXT("jpeg"))
	{
		Out = EURSRgbCompression::Jpeg;
		return true;
	}
	return false;
}

bool ParseDepthCompression(const FString& Value, EURSDepthCompression& Out)
{
	if (Value == TEXT("raw_f32"))
	{
		Out = EURSDepthCompression::RawFloat32;
		return true;
	}
	if (Value == TEXT("raw_u16_mm"))
	{
		Out = EURSDepthCompression::RawUint16Millimeters;
		return true;
	}
	if (Value == TEXT("zlib_u16_mm"))
	{
		Out = EURSDepthCompression::ZlibUint16Millimeters;
		return true;
	}
	return false;
}

const TCHAR* VisionModeString(const EURSVisionMode Value)
{
	return Value == EURSVisionMode::Rgbd ? TEXT("rgbd") : TEXT("stereo_rgb");
}

const TCHAR* RgbCompressionString(const EURSRgbCompression Value)
{
	return Value == EURSRgbCompression::Raw ? TEXT("raw") : TEXT("jpeg");
}

const TCHAR* DepthCompressionString(const EURSDepthCompression Value)
{
	switch (Value)
	{
	case EURSDepthCompression::RawFloat32:
		return TEXT("raw_f32");
	case EURSDepthCompression::RawUint16Millimeters:
		return TEXT("raw_u16_mm");
	case EURSDepthCompression::ZlibUint16Millimeters:
	default:
		return TEXT("zlib_u16_mm");
	}
}

bool ReadVisionConfig(const TSharedPtr<FJsonObject>& Root, FURSVisionConfig& Out, FString& OutError)
{
	const TSharedPtr<FJsonObject>* VisionObjPtr = nullptr;
	if (!Root->TryGetObjectField(TEXT("vision"), VisionObjPtr))
	{
		// Backward-compatible defaults for existing urs_scene_v1 files.
		return true;
	}
	if (!VisionObjPtr || !VisionObjPtr->IsValid())
	{
		OutError = TEXT("scene config: 'vision' must be an object");
		return false;
	}
	const TSharedPtr<FJsonObject>& VisionObj = *VisionObjPtr;

	FString StringValue;
	if (VisionObj->TryGetStringField(TEXT("mode"), StringValue)
		&& !ParseVisionMode(StringValue, Out.Mode))
	{
		OutError = TEXT("scene config: vision.mode must be 'stereo_rgb' or 'rgbd'");
		return false;
	}
	if (VisionObj->TryGetStringField(TEXT("left_camera"), Out.LeftCamera) && Out.LeftCamera.IsEmpty())
	{
		OutError = TEXT("scene config: vision.left_camera must be non-empty");
		return false;
	}
	if (VisionObj->TryGetStringField(TEXT("right_camera"), Out.RightCamera) && Out.RightCamera.IsEmpty())
	{
		OutError = TEXT("scene config: vision.right_camera must be non-empty");
		return false;
	}

	const TSharedPtr<FJsonObject>* RgbObjPtr = nullptr;
	if (VisionObj->TryGetObjectField(TEXT("rgb"), RgbObjPtr))
	{
		if (!RgbObjPtr || !RgbObjPtr->IsValid())
		{
			OutError = TEXT("scene config: vision.rgb must be an object");
			return false;
		}
		const TSharedPtr<FJsonObject>& RgbObj = *RgbObjPtr;
		RgbObj->TryGetNumberField(TEXT("rate_hz"), Out.Rgb.RateHz);
		if (RgbObj->TryGetStringField(TEXT("compression"), StringValue)
			&& !ParseRgbCompression(StringValue, Out.Rgb.Compression))
		{
			OutError = TEXT("scene config: vision.rgb.compression must be 'raw' or 'jpeg'");
			return false;
		}
		double Quality = static_cast<double>(Out.Rgb.JpegQuality);
		if (RgbObj->TryGetNumberField(TEXT("jpeg_quality"), Quality))
		{
			if (!FMath::IsFinite(Quality) || Quality != FMath::TruncToDouble(Quality)
				|| Quality < 1.0 || Quality > 100.0)
			{
				OutError = TEXT("scene config: vision.rgb.jpeg_quality must be an integer in [1, 100]");
				return false;
			}
			Out.Rgb.JpegQuality = static_cast<int32>(Quality);
		}
	}

	const TSharedPtr<FJsonObject>* DepthObjPtr = nullptr;
	if (VisionObj->TryGetObjectField(TEXT("depth"), DepthObjPtr))
	{
		if (!DepthObjPtr || !DepthObjPtr->IsValid())
		{
			OutError = TEXT("scene config: vision.depth must be an object");
			return false;
		}
		const TSharedPtr<FJsonObject>& DepthObj = *DepthObjPtr;
		DepthObj->TryGetNumberField(TEXT("rate_hz"), Out.Depth.RateHz);
		if (DepthObj->TryGetStringField(TEXT("compression"), StringValue)
			&& !ParseDepthCompression(StringValue, Out.Depth.Compression))
		{
			OutError = TEXT("scene config: vision.depth.compression must be 'raw_f32', 'raw_u16_mm', or 'zlib_u16_mm'");
			return false;
		}
		DepthObj->TryGetNumberField(TEXT("max_depth_m"), Out.Depth.MaxDepthMeters);
	}
	return true;
}
} // namespace

bool FURSSceneConfigIo::LoadFromFile(const FString& AbsPath, FURSSceneConfig& Out, FString& OutError)
{
	OutError.Reset();
	Out = FURSSceneConfig();

	FString JsonStr;
	if (!FFileHelper::LoadFileToString(JsonStr, *AbsPath))
	{
		OutError = FString::Printf(TEXT("failed to read scene config file: %s"), *AbsPath);
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = TEXT("scene config is not valid JSON");
		return false;
	}

	FString Version;
	if (!Root->TryGetStringField(TEXT("version"), Version))
	{
		OutError = TEXT("scene config missing 'version' field");
		return false;
	}
	if (Version != Out.Version)
	{
		OutError = FString::Printf(TEXT("scene config version '%s' is unsupported; expected '%s'"), *Version, *Out.Version);
		return false;
	}

	if (!ReadVisionConfig(Root, Out.Vision, OutError))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* RobotsArr = nullptr;
	if (!Root->TryGetArrayField(TEXT("robots"), RobotsArr))
	{
		OutError = TEXT("scene config missing 'robots' array");
		return false;
	}

	for (const TSharedPtr<FJsonValue>& RobotVal : *RobotsArr)
	{
		const TSharedPtr<FJsonObject>* RobotObj;
		if (!RobotVal.IsValid() || !RobotVal->TryGetObject(RobotObj) || !RobotObj->IsValid())
		{
			OutError = TEXT("scene config: every robot entry must be a JSON object");
			return false;
		}

		FURSRobotSpawn Spawn;
		if (!(*RobotObj)->TryGetStringField(TEXT("actor_id"), Spawn.ActorId) || Spawn.ActorId.IsEmpty())
		{
			OutError = TEXT("scene config: robot entry missing non-empty 'actor_id'");
			return false;
		}
		if (!(*RobotObj)->TryGetStringField(TEXT("type"), Spawn.Type) || Spawn.Type.IsEmpty())
		{
			OutError = FString::Printf(TEXT("scene config: robot '%s' missing non-empty 'type'"), *Spawn.ActorId);
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* TransArr = nullptr;
		if ((*RobotObj)->TryGetArrayField(TEXT("translation_m"), TransArr))
		{
			FVector Trans;
			if (!ReadVec3(TransArr, Trans))
			{
				OutError = FString::Printf(TEXT("scene config: robot '%s' has invalid 'translation_m' (need 3 finite numbers)"), *Spawn.ActorId);
				return false;
			}
			Spawn.TranslationMeters = Trans;
		}

		const TArray<TSharedPtr<FJsonValue>>* RotArr = nullptr;
		if ((*RobotObj)->TryGetArrayField(TEXT("rotation_quat_xyzw"), RotArr))
		{
			FQuat Rot;
			if (!ReadQuatXyzw(RotArr, Rot))
			{
				OutError = FString::Printf(TEXT("scene config: robot '%s' has invalid 'rotation_quat_xyzw' (need 4 finite numbers)"), *Spawn.ActorId);
				return false;
			}
			Spawn.RotationQuatXyzw = Rot;
		}

		const TSharedPtr<FJsonObject>* JointPositionsObj = nullptr;
		if ((*RobotObj)->TryGetObjectField(TEXT("joint_positions_rad"), JointPositionsObj))
		{
			if (!JointPositionsObj || !JointPositionsObj->IsValid())
			{
				OutError = FString::Printf(TEXT("scene config: robot '%s' has invalid 'joint_positions_rad' (need an object)"), *Spawn.ActorId);
				return false;
			}

			TMap<FString, float> JointPositions;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*JointPositionsObj)->Values)
			{
				double Value = 0.0;
				if (Pair.Key.IsEmpty() || !Pair.Value.IsValid() || !Pair.Value->TryGetNumber(Value) || !FMath::IsFinite(Value))
				{
					OutError = FString::Printf(TEXT("scene config: robot '%s' has invalid joint position '%s'"), *Spawn.ActorId, *Pair.Key);
					return false;
				}
				JointPositions.Add(Pair.Key, static_cast<float>(Value));
			}
			if (JointPositions.IsEmpty())
			{
				OutError = FString::Printf(TEXT("scene config: robot '%s' has empty 'joint_positions_rad'"), *Spawn.ActorId);
				return false;
			}
			Spawn.JointPositionsRad = MoveTemp(JointPositions);
		}

		Out.Robots.Add(MoveTemp(Spawn));
	}

	return true;
}

bool FURSSceneConfigIo::WriteToFile(const FString& AbsPath, const FURSSceneConfig& In, FString& OutError)
{
	OutError.Reset();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("version"), In.Version);

	TSharedPtr<FJsonObject> VisionObj = MakeShared<FJsonObject>();
	VisionObj->SetStringField(TEXT("mode"), VisionModeString(In.Vision.Mode));
	VisionObj->SetStringField(TEXT("left_camera"), In.Vision.LeftCamera);
	VisionObj->SetStringField(TEXT("right_camera"), In.Vision.RightCamera);

	TSharedPtr<FJsonObject> RgbObj = MakeShared<FJsonObject>();
	RgbObj->SetNumberField(TEXT("rate_hz"), In.Vision.Rgb.RateHz);
	RgbObj->SetStringField(TEXT("compression"), RgbCompressionString(In.Vision.Rgb.Compression));
	RgbObj->SetNumberField(TEXT("jpeg_quality"), In.Vision.Rgb.JpegQuality);
	VisionObj->SetObjectField(TEXT("rgb"), RgbObj);

	TSharedPtr<FJsonObject> DepthObj = MakeShared<FJsonObject>();
	DepthObj->SetNumberField(TEXT("rate_hz"), In.Vision.Depth.RateHz);
	DepthObj->SetStringField(TEXT("compression"), DepthCompressionString(In.Vision.Depth.Compression));
	DepthObj->SetNumberField(TEXT("max_depth_m"), In.Vision.Depth.MaxDepthMeters);
	VisionObj->SetObjectField(TEXT("depth"), DepthObj);
	Root->SetObjectField(TEXT("vision"), VisionObj);

	TArray<TSharedPtr<FJsonValue>> RobotsJson;
	for (const FURSRobotSpawn& Spawn : In.Robots)
	{
		TSharedPtr<FJsonObject> RobotObj = MakeShared<FJsonObject>();
		RobotObj->SetStringField(TEXT("actor_id"), Spawn.ActorId);
		RobotObj->SetStringField(TEXT("type"), Spawn.Type);

		if (Spawn.TranslationMeters.IsSet())
		{
			const FVector& Trans = Spawn.TranslationMeters.GetValue();
			TArray<TSharedPtr<FJsonValue>> TransJson;
			TransJson.Add(MakeShared<FJsonValueNumber>(Trans.X));
			TransJson.Add(MakeShared<FJsonValueNumber>(Trans.Y));
			TransJson.Add(MakeShared<FJsonValueNumber>(Trans.Z));
			RobotObj->SetArrayField(TEXT("translation_m"), TransJson);
		}
		if (Spawn.RotationQuatXyzw.IsSet())
		{
			const FQuat& Quat = Spawn.RotationQuatXyzw.GetValue();
			TArray<TSharedPtr<FJsonValue>> RotJson;
			RotJson.Add(MakeShared<FJsonValueNumber>(Quat.X));
			RotJson.Add(MakeShared<FJsonValueNumber>(Quat.Y));
			RotJson.Add(MakeShared<FJsonValueNumber>(Quat.Z));
			RotJson.Add(MakeShared<FJsonValueNumber>(Quat.W));
			RobotObj->SetArrayField(TEXT("rotation_quat_xyzw"), RotJson);
		}
		if (Spawn.JointPositionsRad.IsSet())
		{
			TSharedPtr<FJsonObject> JointPositionsObj = MakeShared<FJsonObject>();
			TArray<FString> JointNames;
			Spawn.JointPositionsRad.GetValue().GetKeys(JointNames);
			JointNames.Sort();
			for (const FString& JointName : JointNames)
			{
				JointPositionsObj->SetNumberField(JointName, Spawn.JointPositionsRad.GetValue()[JointName]);
			}
			RobotObj->SetObjectField(TEXT("joint_positions_rad"), JointPositionsObj);
		}
		RobotsJson.Add(MakeShared<FJsonValueObject>(RobotObj));
	}
	Root->SetArrayField(TEXT("robots"), RobotsJson);

	FString JsonStr;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JsonStr);
	if (!FJsonSerializer::Serialize(Root.ToSharedRef(), Writer))
	{
		OutError = TEXT("failed to serialize scene config");
		return false;
	}

	if (!FFileHelper::SaveStringToFile(JsonStr, *AbsPath))
	{
		OutError = FString::Printf(TEXT("failed to write scene config file: %s"), *AbsPath);
		return false;
	}
	return true;
}

FURSSceneConfig FURSSceneConfigIo::MakeDefault()
{
	FURSSceneConfig Config;
	FURSRobotSpawn& Robot = Config.Robots.AddDefaulted_GetRef();
	Robot.ActorId = TEXT("robot_rp0");
	Robot.Type = TEXT("pi_plus");
	Robot.TranslationMeters = FVector(0.0, 0.0, 0.3762);
	Robot.RotationQuatXyzw = FQuat::Identity;
	return Config;
}

FURSSceneConfigValidationResult FURSSceneConfigIo::Validate(const FURSSceneConfig& Config)
{
	FURSSceneConfigValidationResult Result;
	TSet<FString> SeenActorIds;

	if (Config.Vision.LeftCamera.IsEmpty())
	{
		Result.bOk = false;
		Result.Errors.Add(TEXT("vision.left_camera is empty"));
	}
	if (Config.Vision.RightCamera.IsEmpty())
	{
		Result.bOk = false;
		Result.Errors.Add(TEXT("vision.right_camera is empty"));
	}
	if (Config.Vision.LeftCamera == Config.Vision.RightCamera)
	{
		Result.bOk = false;
		Result.Errors.Add(TEXT("vision left and right cameras must be different"));
	}
	if (!FMath::IsFinite(Config.Vision.Rgb.RateHz)
		|| Config.Vision.Rgb.RateHz <= 0.0 || Config.Vision.Rgb.RateHz > 240.0)
	{
		Result.bOk = false;
		Result.Errors.Add(TEXT("vision.rgb.rate_hz must be finite and in (0, 240]"));
	}
	if (Config.Vision.Rgb.JpegQuality < 1 || Config.Vision.Rgb.JpegQuality > 100)
	{
		Result.bOk = false;
		Result.Errors.Add(TEXT("vision.rgb.jpeg_quality must be in [1, 100]"));
	}
	if (!FMath::IsFinite(Config.Vision.Depth.RateHz)
		|| Config.Vision.Depth.RateHz <= 0.0 || Config.Vision.Depth.RateHz > 240.0)
	{
		Result.bOk = false;
		Result.Errors.Add(TEXT("vision.depth.rate_hz must be finite and in (0, 240]"));
	}
	if (!FMath::IsFinite(Config.Vision.Depth.MaxDepthMeters)
		|| Config.Vision.Depth.MaxDepthMeters <= 0.0 || Config.Vision.Depth.MaxDepthMeters > 65.535)
	{
		Result.bOk = false;
		Result.Errors.Add(TEXT("vision.depth.max_depth_m must be finite and in (0, 65.535]"));
	}

	for (const FURSRobotSpawn& Spawn : Config.Robots)
	{
		if (Spawn.ActorId.IsEmpty())
		{
			Result.bOk = false;
			Result.Errors.Add(TEXT("robot entry has empty actor_id"));
			continue;
		}
		if (SeenActorIds.Contains(Spawn.ActorId))
		{
			Result.bOk = false;
			Result.Errors.Add(FString::Printf(TEXT("duplicate actor_id '%s'"), *Spawn.ActorId));
		}
		SeenActorIds.Add(Spawn.ActorId);

		if (Spawn.Type.IsEmpty())
		{
			Result.bOk = false;
			Result.Errors.Add(FString::Printf(TEXT("robot '%s' has empty type"), *Spawn.ActorId));
			continue;
		}
		if (!FURSRobotTypeRegistry::Get().Find(Spawn.Type))
		{
			Result.bOk = false;
			Result.Errors.Add(FString::Printf(TEXT("robot '%s' references unknown type '%s'"), *Spawn.ActorId, *Spawn.Type));
		}

		if (Spawn.TranslationMeters.IsSet() && !IsFiniteVec(Spawn.TranslationMeters.GetValue()))
		{
			Result.bOk = false;
			Result.Errors.Add(FString::Printf(TEXT("robot '%s' has non-finite translation"), *Spawn.ActorId));
		}
		if (Spawn.RotationQuatXyzw.IsSet() && !IsValidQuat(Spawn.RotationQuatXyzw.GetValue()))
		{
			Result.bOk = false;
			Result.Errors.Add(FString::Printf(TEXT("robot '%s' has invalid rotation quaternion"), *Spawn.ActorId));
		}
		if (Spawn.JointPositionsRad.IsSet())
		{
			const TMap<FString, float>& JointPositions = Spawn.JointPositionsRad.GetValue();
			if (JointPositions.IsEmpty())
			{
				Result.bOk = false;
				Result.Errors.Add(FString::Printf(TEXT("robot '%s' has empty joint positions"), *Spawn.ActorId));
			}
			for (const TPair<FString, float>& Pair : JointPositions)
			{
				if (Pair.Key.IsEmpty() || !FMath::IsFinite(Pair.Value))
				{
					Result.bOk = false;
					Result.Errors.Add(FString::Printf(TEXT("robot '%s' has invalid joint position"), *Spawn.ActorId));
					break;
				}
			}
		}
	}
	return Result;
}
} // namespace URSoccerLab
