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
//
// Stickiness: only fields whose bHas* flag is set are applied by ApplyGains.
// A partial update (e.g. kp only) therefore leaves kv, damping, and the
// actuator mode at their previously-effective values instead of zeroing them.
struct FGainSet
{
	double Kp[URS_MAX_ACTUATORS] = {0};
	double Kv[URS_MAX_ACTUATORS] = {0};
	double Damping[URS_MAX_ACTUATORS] = {0};
	bool bHasKp[URS_MAX_ACTUATORS] = {false};
	bool bHasKv[URS_MAX_ACTUATORS] = {false};
	bool bHasDamping[URS_MAX_ACTUATORS] = {false};
	int32 Mode = 0;  // 0 = position, 1 = torque
	bool bHasMode = false;
	bool bValid = false;

	FGainSet() = default;
};
