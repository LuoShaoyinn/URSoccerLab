#pragma once

// URSJson.h — yyjson-based JSON build/parse for the network thread.
// No FJsonObject, no MakeShared, no FString serialization overhead.
// All input/output is UTF-8 bytes (const char* / TArray<uint8>).

#include "CoreMinimal.h"
#include "URSSnapshot.h"
#include "Core/URSBuffers.h"

struct yyjson_doc;
struct yyjson_mut_doc;
typedef struct yyjson_doc yyjson_doc;
typedef struct yyjson_mut_doc yyjson_mut_doc;

struct FRobotMetadata
{
	// Static per-robot data that doesn't change between physics steps.
	// Set once during initialization, read by the JSON builder.
	TArray<FString> JointNames;
	TArray<FString> ActuatorNames;
	TArray<FString> CameraNames;
	TArray<int32> CameraWidths;
	TArray<int32> CameraHeights;
	TArray<FString> CameraFormats;

	bool bPrivSelfPos = false;
	bool bPrivBallPosRelated = false;
	bool bPrivBallVelRelated = false;
	bool bPrivAllPos = false;
	TArray<FString> AllActorNames;

	struct FNoiseConfig
	{
		double Qpos = 0, Qvel = 0, Qtor = 0;
		double ImuQuat = 0, ImuAngVel = 0;
		double CameraImuQuat = 0, CameraImuAngVel = 0;
		double SelfPos = 0, BallPosRelated = 0, BallVelRelated = 0, AllPos = 0;
	} Noise;
};

class URSJsonBuilder
{
public:
	// Build a compact state JSON from snapshot + metadata.
	// Returns UTF-8 bytes ready for TCP send (no FString conversion).
	static TArray<uint8> BuildStateJson(
		const FRobotSnapshot& Snap,
		const FRobotMetadata& Meta);

private:
	static void AppendDoubleArray(yyjson_mut_doc* Doc,
		struct yyjson_mut_val* Parent, const char* Key,
		double A, double B, double C);
	static void AppendDoubleArray4(yyjson_mut_doc* Doc,
		struct yyjson_mut_val* Parent, const char* Key,
		double A, double B, double C, double D);
};

class URSJsonParser
{
public:
	// Parse a gain/controller-params JSON.
	// Returns true if the JSON contains gain fields.
	static bool ParseGainParams(const uint8* Data, int32 Len,
		FGainSet& Out, const TArray<FString>& ActuatorNames);

	// Check if JSON contains controller param keys (kp/kv/damping/actuator_mode).
	static bool IsControllerParams(const uint8* Data, int32 Len);
};
