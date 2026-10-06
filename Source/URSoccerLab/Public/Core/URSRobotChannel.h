#pragma once
#include "CoreMinimal.h"
#include "Core/URSTripleBuffer.h"
#include "Core/URSBuffers.h"
#include "URSSnapshot.h"

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

// Stable, UObject-free handles. Each state buffer has one consumer.
struct FURSRobotChannel
{
	FString ActorId;
	FRobotMetadata Meta;
	TSharedPtr<URSTripleBuffer<FRobotSnapshot>, ESPMode::ThreadSafe> StateBuf;
	TSharedPtr<URSTripleBuffer<FCommandSet>, ESPMode::ThreadSafe> CmdBuf;
	TSharedPtr<URSTripleBuffer<FGainSet>, ESPMode::ThreadSafe> GainBuf;
};

// Coalesces partial controller updates before publishing to latest-value buffers.
// Caller owns AccumulatedGains; the buffers do not call any engine/physics APIs.
inline void PublishRobotMessage(const FURSRobotChannel& Channel, const FCommandSet* Command, const FGainSet* Gains,
                                FGainSet& AccumulatedGains)
{
	if (Command)
	{
		if (Channel.CmdBuf)
			Channel.CmdBuf->PublishValue(*Command);
		return;
	}
	if (!Gains)
		return;
	const auto& Parsed = *Gains;
	auto& Acc = AccumulatedGains;
	for (int32 Index = 0; Index < FMath::Min(Channel.Meta.ActuatorNames.Num(), URS_MAX_ACTUATORS); ++Index)
	{
		if (Parsed.bHasKp[Index])
		{
			Acc.Kp[Index] = Parsed.Kp[Index];
			Acc.bHasKp[Index] = true;
		}
		if (Parsed.bHasKv[Index])
		{
			Acc.Kv[Index] = Parsed.Kv[Index];
			Acc.bHasKv[Index] = true;
		}
		if (Parsed.bHasDamping[Index])
		{
			Acc.Damping[Index] = Parsed.Damping[Index];
			Acc.bHasDamping[Index] = true;
		}
	}
	if (Parsed.bHasMode)
	{
		Acc.Mode = Parsed.Mode;
		Acc.bHasMode = true;
	}
	Acc.bValid = true;
	if (Channel.GainBuf)
		Channel.GainBuf->PublishValue(Acc);
}
