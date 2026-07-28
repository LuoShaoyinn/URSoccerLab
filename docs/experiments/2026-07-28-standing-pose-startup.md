# Pi Standing Pose Startup Fix

## Scope

Investigate free-base Pi robots falling to the ground and producing blank or
ground-facing camera images after startup.

## Root Cause

The initial reset also ran during `InitGame`, before URLab compiled the MuJoCo
model. At that time no joint IDs or qpos layout existed, so the reset found no
robot endpoints and never retried after compilation.

## Changes

- Reset applies the scene-configured free-base transform after URLab model/data
  exist. It uses MJCF-native zero qpos unless a robot explicitly supplies a
  complete named `joint_positions_rad` map in the scene config.
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
- Zero qpos is statically upright, but the legacy walking policy is trained
  around its nonzero `DEFAULT_DOF` reference. The active walking scene records
  that posture in configuration rather than runtime code.
- With that configured posture, the external TCP walking client completed its
  eight-second `vx=0.35` run, reported `+2.413 m` forward displacement, and
  wrote a 74-frame left-eye MP4 with the field and goal visible mid-run. This
  validates the runtime protocol and capture path, not policy-model parity.
