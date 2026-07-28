# Pi Standing Pose Startup Fix

## Scope

Investigate free-base Pi robots falling to the ground and producing blank or
ground-facing camera images after startup.

## Root Cause

The prior MuJoCo simulator explicitly initialized Pi with a nonzero standing
pose. URSoccerLab reset every non-root qpos to zero. For the current Pi MJCF,
zero joint qpos is not a standing pose.

The initial reset also ran during `InitGame`, before URLab compiled the MuJoCo
model. At that time no joint IDs or qpos layout existed, so the reset found no
robot endpoints and never retried after compilation.

## Changes

- `pi_plus` defines all 22 non-root standing joint targets in the fixed robot
  type registry.
- Reset applies that named pose after the URLab model/data exist.
- `AURSSoccerGameMode::StartPlay` triggers the post-compile initialization.
- Pi position actuators use the old simulator's Kp/Kd and 20 Nm effort limit.
- Head-motion and vision-smoke clients continuously command the standing pose
  instead of all-zero actuator targets.
- Reimported the tracked Pi Blueprint and dependencies from the changed MJCF.

## Validation

- MJCF parsed with MuJoCo: 29 qpos, 22 actuators, expected named joint order.
- UnrealEditor runtime build succeeded.
- 12/12 `URSoccerLab` automation tests passed.
- Two-robot head-motion runtime: both robots held `z=0.36`, upright score
  `1.00` during the eight-second head sweep.
- Standard vision harness passed with a 640x480 frame, mean RGB 46.92 and
  nonblack pixel ratio 0.526.
