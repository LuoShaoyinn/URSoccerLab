#pragma once
#include "Core/URSBuffers.h"

#include "CoreMinimal.h"

// ---------------------------------------------------------------------------
// FRobotSnapshot — flat POD struct written by the physics thread every step.
// Read by the game / network thread to build state JSON.
// ---------------------------------------------------------------------------
static constexpr int32 URS_MAX_JOINTS = 40;
static constexpr int32 URS_MAX_ACTORS = 24;

struct FRobotSnapshot
{
	double SimTime = 0.0;
	bool bCommandTimedOut = true;

	// Base pose (world)
	double BasePos[3] = {0, 0, 0};
	double BaseQuat[4] = {1, 0, 0, 0}; // w, x, y, z
	double BaseVel[6] = {0};

	// Joints (non-root hinge / slide)
	int32 JointCount = 0;
	double JointQpos[URS_MAX_JOINTS] = {0};
	double JointQvel[URS_MAX_JOINTS] = {0};

	// Actuator last-applied commands
	int32 ActuatorCount = 0;
	double ActuatorCmds[URS_MAX_ACTUATORS] = {0};

	// Camera / head IMU
	bool bHasCameraImu = false;
	double HeadQuat[4] = {1, 0, 0, 0};
	double HeadAngVel[3] = {0};

	// Privileged positions (world-space)
	bool bPrivSelfPos = false;
	bool bPrivBallPosRelated = false;
	bool bPrivBallVelRelated = false;
	bool bPrivAllPos = false;
	double SelfPos[3] = {0};
	double BallPosRelated[3] = {0};
	double BallVelRelated[3] = {0};
	int32 ActorCount = 0;
	double ActorPos[URS_MAX_ACTORS][3] = {{0}};
};
