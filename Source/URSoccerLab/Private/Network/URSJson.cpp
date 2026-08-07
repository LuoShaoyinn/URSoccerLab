#include "Network/URSJson.h"
#include <yyjson.h>
#include "Misc/AsciiSet.h"

// ============================================================================
// URSJsonBuilder
// ============================================================================

void URSJsonBuilder::AppendDoubleArray(yyjson_mut_doc* Doc,
	yyjson_mut_val* Parent, const char* Key,
	double A, double B, double C)
{
	yyjson_mut_val* Arr = yyjson_mut_arr(Doc);
	yyjson_mut_arr_add_real(Doc, Arr, A);
	yyjson_mut_arr_add_real(Doc, Arr, B);
	yyjson_mut_arr_add_real(Doc, Arr, C);
	yyjson_mut_obj_add_val(Doc, Parent, Key, Arr);
}

void URSJsonBuilder::AppendDoubleArray4(yyjson_mut_doc* Doc,
	yyjson_mut_val* Parent, const char* Key,
	double A, double B, double C, double D)
{
	yyjson_mut_val* Arr = yyjson_mut_arr(Doc);
	yyjson_mut_arr_add_real(Doc, Arr, A);
	yyjson_mut_arr_add_real(Doc, Arr, B);
	yyjson_mut_arr_add_real(Doc, Arr, C);
	yyjson_mut_arr_add_real(Doc, Arr, D);
	yyjson_mut_obj_add_val(Doc, Parent, Key, Arr);
}

TArray<uint8> URSJsonBuilder::BuildStateJson(
	const FRobotSnapshot& Snap,
	const FRobotMetadata& Meta)
{
	TArray<uint8> Result;

	yyjson_mut_doc* Doc = yyjson_mut_doc_new(nullptr);
	yyjson_mut_val* Root = yyjson_mut_obj(Doc);
	yyjson_mut_doc_set_root(Doc, Root);

	yyjson_mut_obj_add_real(Doc, Root, "sim_time", Snap.SimTime);
	yyjson_mut_obj_add_bool(Doc, Root, "command_timed_out", Snap.bCommandTimedOut);

	// --- base ---
	yyjson_mut_val* Base = yyjson_mut_obj(Doc);
	yyjson_mut_obj_add_val(Doc, Root, "base", Base);

	AppendDoubleArray(Doc, Base, "pos",
		Snap.BasePos[0], Snap.BasePos[1], Snap.BasePos[2]);
	AppendDoubleArray4(Doc, Base, "quat",
		Snap.BaseQuat[0], Snap.BaseQuat[1], Snap.BaseQuat[2], Snap.BaseQuat[3]);

	yyjson_mut_val* Vel = yyjson_mut_arr(Doc);
	for (int32 i = 0; i < 6; ++i)
		yyjson_mut_arr_add_real(Doc, Vel, Snap.BaseVel[i]);
	yyjson_mut_obj_add_val(Doc, Base, "vel", Vel);

	// --- joints ---
	yyjson_mut_val* Joints = yyjson_mut_obj(Doc);
	yyjson_mut_obj_add_val(Doc, Root, "joints", Joints);
	const int32 JC = FMath::Min(Snap.JointCount, Meta.JointNames.Num());
	for (int32 i = 0; i < JC; ++i)
	{
		yyjson_mut_val* J = yyjson_mut_obj(Doc);
		yyjson_mut_obj_add_real(Doc, J, "qpos", Snap.JointQpos[i]);
		yyjson_mut_obj_add_real(Doc, J, "qvel", Snap.JointQvel[i]);
		// FString → UTF-8 for key: use FTCHARToUTF8 then yyjson_mut_strncpy
		FTCHARToUTF8 Conv(*Meta.JointNames[i]);
		yyjson_mut_val* Key = yyjson_mut_strncpy(Doc, (const char*)Conv.Get(), Conv.Length());
		yyjson_mut_obj_add(Joints, Key, J);
	}

	// --- actuators ---
	yyjson_mut_val* Acts = yyjson_mut_obj(Doc);
	yyjson_mut_obj_add_val(Doc, Root, "actuators", Acts);
	const int32 AC = FMath::Min(Snap.ActuatorCount, Meta.ActuatorNames.Num());
	for (int32 i = 0; i < AC; ++i)
	{
		FTCHARToUTF8 Conv(*Meta.ActuatorNames[i]);
		yyjson_mut_val* Key = yyjson_mut_strncpy(Doc, (const char*)Conv.Get(), Conv.Length());
		yyjson_mut_obj_add(Acts, Key, yyjson_mut_real(Doc, Snap.ActuatorCmds[i]));
	}

	// --- cameras ---
	if (Meta.CameraNames.Num() > 0)
	{
		yyjson_mut_val* Cams = yyjson_mut_arr(Doc);
		yyjson_mut_obj_add_val(Doc, Root, "cameras", Cams);
		for (int32 i = 0; i < Meta.CameraNames.Num(); ++i)
		{
			yyjson_mut_val* C = yyjson_mut_obj(Doc);
			FTCHARToUTF8 N(*Meta.CameraNames[i]);
			FTCHARToUTF8 F(*Meta.CameraFormats[i]);
			yyjson_mut_obj_add_strncpy(Doc, C, "name", (const char*)N.Get(), N.Length());
			yyjson_mut_obj_add_strncpy(Doc, C, "format", (const char*)F.Get(), F.Length());
			yyjson_mut_obj_add_int(Doc, C, "width", Meta.CameraWidths.IsValidIndex(i) ? Meta.CameraWidths[i] : 0);
			yyjson_mut_obj_add_int(Doc, C, "height", Meta.CameraHeights.IsValidIndex(i) ? Meta.CameraHeights[i] : 0);
			yyjson_mut_arr_append(Cams, C);
		}
	}

	// --- camera_imu ---
	if (Snap.bHasCameraImu)
	{
		yyjson_mut_val* Imu = yyjson_mut_obj(Doc);
		yyjson_mut_obj_add_val(Doc, Root, "camera_imu", Imu);
		AppendDoubleArray4(Doc, Imu, "quat",
			Snap.HeadQuat[0], Snap.HeadQuat[1], Snap.HeadQuat[2], Snap.HeadQuat[3]);
		AppendDoubleArray(Doc, Imu, "ang_vel",
			Snap.HeadAngVel[0], Snap.HeadAngVel[1], Snap.HeadAngVel[2]);
	}

	// --- privileged positions ---
	if (Snap.bPrivSelfPos)
		AppendDoubleArray(Doc, Root, "self_pos",
			Snap.SelfPos[0], Snap.SelfPos[1], Snap.SelfPos[2]);
	if (Snap.bPrivBallPosRelated)
		AppendDoubleArray(Doc, Root, "ball_pos_related",
			Snap.BallPosRelated[0], Snap.BallPosRelated[1], Snap.BallPosRelated[2]);
	if (Snap.bPrivBallVelRelated)
		AppendDoubleArray(Doc, Root, "ball_vel_related",
			Snap.BallVelRelated[0], Snap.BallVelRelated[1], Snap.BallVelRelated[2]);

	if (Snap.bPrivAllPos)
	{
		yyjson_mut_val* All = yyjson_mut_obj(Doc);
		yyjson_mut_obj_add_val(Doc, Root, "all_pos", All);
		for (int32 i = 0; i < Snap.ActorCount && i < Meta.AllActorNames.Num(); ++i)
		{
			FTCHARToUTF8 Conv(*Meta.AllActorNames[i]);
			yyjson_mut_val* Key = yyjson_mut_strncpy(Doc, (const char*)Conv.Get(), Conv.Length());
			yyjson_mut_val* Arr = yyjson_mut_arr(Doc);
			yyjson_mut_arr_add_real(Doc, Arr, Snap.ActorPos[i][0]);
			yyjson_mut_arr_add_real(Doc, Arr, Snap.ActorPos[i][1]);
			yyjson_mut_arr_add_real(Doc, Arr, Snap.ActorPos[i][2]);
			yyjson_mut_obj_add(All, Key, Arr);
		}
	}

	// --- serialize to compact UTF-8 ---
	size_t Len = 0;
	char* Buf = yyjson_mut_write(Doc, YYJSON_WRITE_NOFLAG, &Len);
	if (Buf && Len > 0)
		Result.Append((const uint8*)Buf, Len);
	free(Buf);
	yyjson_mut_doc_free(Doc);

	return Result;
}

// ============================================================================
// URSJsonParser
// ============================================================================

bool URSJsonParser::IsControllerParams(const uint8* Data, int32 Len)
{
	yyjson_doc* Doc = yyjson_read((const char*)Data, Len, YYJSON_READ_NOFLAG);
	if (!Doc) return false;
	yyjson_val* Root = yyjson_doc_get_root(Doc);
	bool bResult = false;
	if (Root && yyjson_is_obj(Root))
	{
		bResult = yyjson_obj_get(Root, "kp") ||
		          yyjson_obj_get(Root, "kv") ||
		          yyjson_obj_get(Root, "damping") ||
		          yyjson_obj_get(Root, "actuator_mode");
	}
	yyjson_doc_free(Doc);
	return bResult;
}

bool URSJsonParser::ParseCommand(const uint8* Data, int32 Len, FCommandSet& Out)
{
	yyjson_doc* Doc = yyjson_read((const char*)Data, Len, YYJSON_READ_NOFLAG);
	if (!Doc) return false;
	yyjson_val* Root = yyjson_doc_get_root(Doc);
	if (!Root || !yyjson_is_obj(Root))
	{
		yyjson_doc_free(Doc);
		return false;
	}

	// Flat name→float map: find matching actuator names
	bool bAny = false;
	// NOTE: The actuator name→index mapping is NOT available here (the parser
	// doesn't know the endpoint layout). The caller handles the mapping.
	// For now, just flag that this is a command (not gain params).
	// The network thread will do the name→index resolution.

	yyjson_doc_free(Doc);
	return true;  // it's a command (not gain params)
}

bool URSJsonParser::ParseGainParams(const uint8* Data, int32 Len,
	FGainSet& Out, const TArray<FString>& ActuatorNames)
{
	yyjson_doc* Doc = yyjson_read((const char*)Data, Len, YYJSON_READ_NOFLAG);
	if (!Doc) return false;
	yyjson_val* Root = yyjson_doc_get_root(Doc);
	if (!Root || !yyjson_is_obj(Root))
	{
		yyjson_doc_free(Doc);
		return false;
	}

	const int32 N = FMath::Min(ActuatorNames.Num(), URS_MAX_ACTUATORS);

	auto ParseMap = [&](const char* Key, double* Dst)
	{
		yyjson_val* Obj = yyjson_obj_get(Root, Key);
		if (!Obj || !yyjson_is_obj(Obj)) return;
		yyjson_obj_iter Iter;
		yyjson_obj_iter_init(Obj, &Iter);
		yyjson_val* KeyVal;
		while ((KeyVal = yyjson_obj_iter_next(&Iter)))
		{
			yyjson_val* V = yyjson_obj_iter_get_val(KeyVal);
			if (!yyjson_is_num(V)) continue;
			const char* K = yyjson_get_str(KeyVal);
			FString FName(UTF8_TO_TCHAR(K));
			for (int32 i = 0; i < N; ++i)
			{
				if (ActuatorNames[i] == FName)
				{
					Dst[i] = yyjson_get_num(V);
					break;
				}
			}
		}
	};

	ParseMap("kp", Out.Kp);
	ParseMap("kv", Out.Kv);
	ParseMap("damping", Out.Damping);

	yyjson_val* ModeVal = yyjson_obj_get(Root, "actuator_mode");
	if (ModeVal && yyjson_is_str(ModeVal))
	{
		const char* M = yyjson_get_str(ModeVal);
		if (FString(UTF8_TO_TCHAR(M)) == TEXT("torque"))
			Out.Mode = 1;
		else
			Out.Mode = 0;
	}
	else
	{
		Out.Mode = 0;  // default position
	}

	Out.bValid = true;
	yyjson_doc_free(Doc);
	return true;
}
