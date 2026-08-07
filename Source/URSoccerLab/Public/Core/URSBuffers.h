#pragma once

// URSBuffers.h — POD structs for the triple-buffer command/gain channels.
// No UE containers. Fixed-size arrays. Trivially copyable.

static constexpr int32 URS_MAX_ACTUATORS = 40;

// Command set: network thread writes, physics thread reads.
// Written atomically via triple buffer — always a consistent snapshot.
struct FCommandSet
{
	float Targets[URS_MAX_ACTUATORS] = {0};
	double TimestampSec = 0.0;
	bool bValid = false;

	FCommandSet() = default;
};

// Gain set: network thread writes (rare), physics thread reads.
// Written atomically via triple buffer.
struct FGainSet
{
	double Kp[URS_MAX_ACTUATORS] = {0};
	double Kv[URS_MAX_ACTUATORS] = {0};
	double Damping[URS_MAX_ACTUATORS] = {0};
	int32 Mode = 0;  // 0 = position, 1 = torque
	bool bValid = false;

	FGainSet() = default;
};
