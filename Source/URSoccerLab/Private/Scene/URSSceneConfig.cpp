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

bool ReadRenderConfig(const TSharedPtr<FJsonObject>& Root, FURSRenderConfig& Out, FString& OutError)
{
	const TSharedPtr<FJsonObject>* RenderObjPtr = nullptr;
	if (!Root->TryGetObjectField(TEXT("render"), RenderObjPtr))
	{
		// No render block: leave engine render settings untouched.
		return true;
	}
	if (!RenderObjPtr || !RenderObjPtr->IsValid())
	{
		OutError = TEXT("scene config: 'render' must be an object");
		return false;
	}
	Out.bIsSet = true;
	const TSharedPtr<FJsonObject>& R = *RenderObjPtr;

	bool bValue = false;
	if (R->TryGetBoolField(TEXT("enable"), bValue)) Out.bEnable = bValue;
	if (R->TryGetBoolField(TEXT("lumen"), bValue)) Out.bLumen = bValue;
	if (R->TryGetBoolField(TEXT("hardware_ray_tracing"), bValue)) Out.bHardwareRayTracing = bValue;
	if (R->TryGetBoolField(TEXT("motion_blur"), bValue)) Out.bMotionBlur = bValue;
	if (R->TryGetBoolField(TEXT("auto_exposure"), bValue)) Out.bAutoExposure = bValue;

	FString AA;
	if (R->TryGetStringField(TEXT("anti_aliasing"), AA) && !AA.IsEmpty()) Out.AntiAliasing = AA;

	double DValue = 0.0;
	if (R->TryGetNumberField(TEXT("screen_percentage"), DValue) && FMath::IsFinite(DValue) && DValue > 0.0)
	{
		Out.ScreenPercentage = DValue;
	}
	if (R->TryGetNumberField(TEXT("exposure_compensation"), DValue) && FMath::IsFinite(DValue))
	{
		Out.ExposureCompensation = DValue;
	}

	double NValue = 0.0;
	if (R->TryGetNumberField(TEXT("shadow_quality"), NValue) && NValue == FMath::TruncToDouble(NValue)
		&& NValue >= 0.0 && NValue <= 5.0)
	{
		Out.ShadowQuality = static_cast<int32>(NValue);
	}
	if (R->TryGetNumberField(TEXT("resolution_x"), NValue) && NValue == FMath::TruncToDouble(NValue) && NValue >= 1.0)
	{
		Out.ResolutionX = static_cast<int32>(NValue);
	}
	if (R->TryGetNumberField(TEXT("resolution_y"), NValue) && NValue == FMath::TruncToDouble(NValue) && NValue >= 1.0)
	{
		Out.ResolutionY = static_cast<int32>(NValue);
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

	if (!ReadRenderConfig(Root, Out.Render, OutError))
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

		const TSharedPtr<FJsonObject>* PrivilegeObj = nullptr;
		if ((*RobotObj)->TryGetObjectField(TEXT("privilege"), PrivilegeObj) && PrivilegeObj && PrivilegeObj->IsValid())
		{
			const TSharedPtr<FJsonObject>& P = *PrivilegeObj;
			bool bValue = false;
			if (P->TryGetBoolField(TEXT("self_pos"), bValue)) Spawn.Privilege.bSelfPos = bValue;
			if (P->TryGetBoolField(TEXT("ball_pos_related"), bValue)) Spawn.Privilege.bBallPosRelated = bValue;
			if (P->TryGetBoolField(TEXT("ball_vel_related"), bValue)) Spawn.Privilege.bBallVelRelated = bValue;
			if (P->TryGetBoolField(TEXT("all_pos"), bValue)) Spawn.Privilege.bAllPos = bValue;
		}

		const TSharedPtr<FJsonObject>* NoiseObj = nullptr;
		if ((*RobotObj)->TryGetObjectField(TEXT("noise"), NoiseObj) && NoiseObj && NoiseObj->IsValid())
		{
			const TSharedPtr<FJsonObject>& N = *NoiseObj;
			double Sigma = 0.0;
			auto ReadSigma = [&N](const TCHAR* Key, double& Out)
			{
				double V = 0.0;
				if (N->TryGetNumberField(Key, V) && FMath::IsFinite(V) && V >= 0.0) Out = V;
			};
			ReadSigma(TEXT("qpos"), Spawn.Noise.Qpos);
			ReadSigma(TEXT("qvel"), Spawn.Noise.Qvel);
			ReadSigma(TEXT("qtor"), Spawn.Noise.Qtor);
		ReadSigma(TEXT("imu_quat"), Spawn.Noise.ImuQuat);
		ReadSigma(TEXT("imu_ang_vel"), Spawn.Noise.ImuAngVel);
		ReadSigma(TEXT("camera_imu_quat"), Spawn.Noise.CameraImuQuat);
		ReadSigma(TEXT("camera_imu_ang_vel"), Spawn.Noise.CameraImuAngVel);
			ReadSigma(TEXT("self_pos"), Spawn.Noise.SelfPos);
			ReadSigma(TEXT("ball_pos_related"), Spawn.Noise.BallPosRelated);
			ReadSigma(TEXT("ball_vel_related"), Spawn.Noise.BallVelRelated);
			ReadSigma(TEXT("all_pos"), Spawn.Noise.AllPos);
		}

		Out.Robots.Add(MoveTemp(Spawn));
	}

	const TArray<TSharedPtr<FJsonValue>>* ObjectsArr = nullptr;
	if (Root->TryGetArrayField(TEXT("objects"), ObjectsArr))
	{
		for (const TSharedPtr<FJsonValue>& ObjectVal : *ObjectsArr)
		{
			const TSharedPtr<FJsonObject>* ObjectObj;
			if (!ObjectVal.IsValid() || !ObjectVal->TryGetObject(ObjectObj) || !ObjectObj->IsValid())
			{
				OutError = TEXT("scene config: every object entry must be a JSON object");
				return false;
			}

			FURSObjectSpawn Spawn;
			if (!(*ObjectObj)->TryGetStringField(TEXT("actor_id"), Spawn.ActorId) || Spawn.ActorId.IsEmpty())
			{
				OutError = TEXT("scene config: object entry missing non-empty 'actor_id'");
				return false;
			}
			if (!(*ObjectObj)->TryGetStringField(TEXT("type"), Spawn.Type) || Spawn.Type.IsEmpty())
			{
				OutError = FString::Printf(TEXT("scene config: object '%s' missing non-empty 'type'"), *Spawn.ActorId);
				return false;
			}

			const TArray<TSharedPtr<FJsonValue>>* TransArr = nullptr;
			if ((*ObjectObj)->TryGetArrayField(TEXT("translation_m"), TransArr))
			{
				FVector Trans;
				if (!ReadVec3(TransArr, Trans))
				{
					OutError = FString::Printf(TEXT("scene config: object '%s' has invalid 'translation_m' (need 3 finite numbers)"), *Spawn.ActorId);
					return false;
				}
				Spawn.TranslationMeters = Trans;
			}

			const TArray<TSharedPtr<FJsonValue>>* RotArr = nullptr;
			if ((*ObjectObj)->TryGetArrayField(TEXT("rotation_quat_xyzw"), RotArr))
			{
				FQuat Rot;
				if (!ReadQuatXyzw(RotArr, Rot))
				{
					OutError = FString::Printf(TEXT("scene config: object '%s' has invalid 'rotation_quat_xyzw' (need 4 finite numbers)"), *Spawn.ActorId);
					return false;
				}
				Spawn.RotationQuatXyzw = Rot;
			}
			Out.Objects.Add(MoveTemp(Spawn));
		}
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

	if (In.Render.bIsSet)
	{
		TSharedPtr<FJsonObject> RenderObj = MakeShared<FJsonObject>();
		RenderObj->SetBoolField(TEXT("enable"), In.Render.bEnable);
		RenderObj->SetBoolField(TEXT("lumen"), In.Render.bLumen);
		RenderObj->SetBoolField(TEXT("hardware_ray_tracing"), In.Render.bHardwareRayTracing);
		RenderObj->SetStringField(TEXT("anti_aliasing"), In.Render.AntiAliasing);
		RenderObj->SetNumberField(TEXT("screen_percentage"), In.Render.ScreenPercentage);
		RenderObj->SetNumberField(TEXT("shadow_quality"), In.Render.ShadowQuality);
		RenderObj->SetBoolField(TEXT("motion_blur"), In.Render.bMotionBlur);
		RenderObj->SetBoolField(TEXT("auto_exposure"), In.Render.bAutoExposure);
		RenderObj->SetNumberField(TEXT("exposure_compensation"), In.Render.ExposureCompensation);
		if (In.Render.ResolutionX.IsSet()) RenderObj->SetNumberField(TEXT("resolution_x"), In.Render.ResolutionX.GetValue());
		if (In.Render.ResolutionY.IsSet()) RenderObj->SetNumberField(TEXT("resolution_y"), In.Render.ResolutionY.GetValue());
		Root->SetObjectField(TEXT("render"), RenderObj);
	}

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
		if (Spawn.Privilege.bSelfPos || Spawn.Privilege.bBallPosRelated || Spawn.Privilege.bAllPos)
		{
			TSharedPtr<FJsonObject> PrivilegeObj = MakeShared<FJsonObject>();
			PrivilegeObj->SetBoolField(TEXT("self_pos"), Spawn.Privilege.bSelfPos);
			PrivilegeObj->SetBoolField(TEXT("ball_pos_related"), Spawn.Privilege.bBallPosRelated);
			PrivilegeObj->SetBoolField(TEXT("ball_vel_related"), Spawn.Privilege.bBallVelRelated);
			PrivilegeObj->SetBoolField(TEXT("all_pos"), Spawn.Privilege.bAllPos);
			RobotObj->SetObjectField(TEXT("privilege"), PrivilegeObj);
		}
		if (Spawn.Noise.Qpos > 0.0 || Spawn.Noise.Qvel > 0.0 || Spawn.Noise.Qtor > 0.0
			|| Spawn.Noise.ImuQuat > 0.0 || Spawn.Noise.ImuAngVel > 0.0
			|| Spawn.Noise.CameraImuQuat > 0.0 || Spawn.Noise.CameraImuAngVel > 0.0
			|| Spawn.Noise.SelfPos > 0.0 || Spawn.Noise.BallPosRelated > 0.0
			|| Spawn.Noise.BallVelRelated > 0.0 || Spawn.Noise.AllPos > 0.0)
		{
			TSharedPtr<FJsonObject> NoiseObj = MakeShared<FJsonObject>();
			NoiseObj->SetNumberField(TEXT("qpos"), Spawn.Noise.Qpos);
			NoiseObj->SetNumberField(TEXT("qvel"), Spawn.Noise.Qvel);
			NoiseObj->SetNumberField(TEXT("qtor"), Spawn.Noise.Qtor);
			NoiseObj->SetNumberField(TEXT("imu_quat"), Spawn.Noise.ImuQuat);
			NoiseObj->SetNumberField(TEXT("imu_ang_vel"), Spawn.Noise.ImuAngVel);
			NoiseObj->SetNumberField(TEXT("camera_imu_quat"), Spawn.Noise.CameraImuQuat);
			NoiseObj->SetNumberField(TEXT("camera_imu_ang_vel"), Spawn.Noise.CameraImuAngVel);
			NoiseObj->SetNumberField(TEXT("self_pos"), Spawn.Noise.SelfPos);
			NoiseObj->SetNumberField(TEXT("ball_pos_related"), Spawn.Noise.BallPosRelated);
			NoiseObj->SetNumberField(TEXT("ball_vel_related"), Spawn.Noise.BallVelRelated);
			NoiseObj->SetNumberField(TEXT("all_pos"), Spawn.Noise.AllPos);
			RobotObj->SetObjectField(TEXT("noise"), NoiseObj);
		}
		RobotsJson.Add(MakeShared<FJsonValueObject>(RobotObj));
	}
	Root->SetArrayField(TEXT("robots"), RobotsJson);

	TArray<TSharedPtr<FJsonValue>> ObjectsJson;
	for (const FURSObjectSpawn& Spawn : In.Objects)
	{
		TSharedPtr<FJsonObject> ObjectObj = MakeShared<FJsonObject>();
		ObjectObj->SetStringField(TEXT("actor_id"), Spawn.ActorId);
		ObjectObj->SetStringField(TEXT("type"), Spawn.Type);
		if (Spawn.TranslationMeters.IsSet())
		{
			const FVector& Trans = Spawn.TranslationMeters.GetValue();
			ObjectObj->SetArrayField(TEXT("translation_m"), {
				MakeShared<FJsonValueNumber>(Trans.X),
				MakeShared<FJsonValueNumber>(Trans.Y),
				MakeShared<FJsonValueNumber>(Trans.Z)});
		}
		if (Spawn.RotationQuatXyzw.IsSet())
		{
			const FQuat& Quat = Spawn.RotationQuatXyzw.GetValue();
			ObjectObj->SetArrayField(TEXT("rotation_quat_xyzw"), {
				MakeShared<FJsonValueNumber>(Quat.X),
				MakeShared<FJsonValueNumber>(Quat.Y),
				MakeShared<FJsonValueNumber>(Quat.Z),
				MakeShared<FJsonValueNumber>(Quat.W)});
		}
		ObjectsJson.Add(MakeShared<FJsonValueObject>(ObjectObj));
	}
	Root->SetArrayField(TEXT("objects"), ObjectsJson);

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
	Robot.TranslationMeters = FVector(-1.0, 0.0, 0.3762);
	Robot.RotationQuatXyzw = FQuat::Identity;
	FURSObjectSpawn& Ball = Config.Objects.AddDefaulted_GetRef();
	Ball.ActorId = TEXT("ball");
	Ball.Type = TEXT("soccer_ball");
	Ball.TranslationMeters = FVector(0.0, 0.0, 0.075);
	Ball.RotationQuatXyzw = FQuat::Identity;
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

	for (const FURSObjectSpawn& Spawn : Config.Objects)
	{
		if (Spawn.ActorId.IsEmpty())
		{
			Result.bOk = false;
			Result.Errors.Add(TEXT("object entry has empty actor_id"));
			continue;
		}
		if (SeenActorIds.Contains(Spawn.ActorId))
		{
			Result.bOk = false;
			Result.Errors.Add(FString::Printf(TEXT("duplicate actor_id '%s'"), *Spawn.ActorId));
		}
		SeenActorIds.Add(Spawn.ActorId);
		if (Spawn.Type.IsEmpty() || !FURSObjectTypeRegistry::Get().Find(Spawn.Type))
		{
			Result.bOk = false;
			Result.Errors.Add(FString::Printf(TEXT("object '%s' references unknown type '%s'"), *Spawn.ActorId, *Spawn.Type));
		}
		if (Spawn.TranslationMeters.IsSet() && !IsFiniteVec(Spawn.TranslationMeters.GetValue()))
		{
			Result.bOk = false;
			Result.Errors.Add(FString::Printf(TEXT("object '%s' has non-finite translation"), *Spawn.ActorId));
		}
		if (Spawn.RotationQuatXyzw.IsSet() && !IsValidQuat(Spawn.RotationQuatXyzw.GetValue()))
		{
			Result.bOk = false;
			Result.Errors.Add(FString::Printf(TEXT("object '%s' has invalid rotation quaternion"), *Spawn.ActorId));
		}
	}
	return Result;
}
} // namespace URSoccerLab
