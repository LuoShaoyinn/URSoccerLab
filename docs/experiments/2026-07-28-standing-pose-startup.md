# Pi Standing Pose Startup Fix

## Scope

Investigate free-base Pi robots falling to the ground and producing blank or
ground-facing camera images after startup.

## Root Cause

The initial reset also ran during `InitGame`, before URLab compiled the MuJoCo
model. At that time no joint IDs or qpos layout existed, so the reset found no
robot endpoints and never retried after compilation.

## Changes

- Reset applies only the scene-configured free-base transform after URLab
  model/data exist. Pi keeps its MJCF-native zero joint qpos.
- `AURSSoccerGameMode::StartPlay` triggers the post-compile initialization.
- Pi position actuators use the old simulator's Kp/Kd and 20 Nm effort limit.
- Head-motion commands only the two head actuators. Vision smoke sends no
  motor command.
- Reimported the tracked Pi Blueprint and dependencies from the changed MJCF.

## Validation

- MJCF parsed with MuJoCo: 29 qpos, 22 actuators, expected named joint order.
- UnrealEditor runtime build succeeded.
- 12/12 `URSoccerLab` automation tests passed.
- Two-robot head-motion runtime: both robots held `z=0.38`, upright score
  `1.00` during the eight-second head sweep.
- Standard vision harness passed with a 640x480 frame, mean RGB 38.21 and
  nonblack pixel ratio 0.418.
