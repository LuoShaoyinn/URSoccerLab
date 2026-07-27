# Shadow Jitter Investigation and Z-Offset Fix

Date: 2026-07-27 (updated 2026-07-28)

## Summary

Investigated frame-to-frame shadow/render jitter on MuJoCo-driven robots in UE 5.7.4.
The primary user-visible issue — the robot floating ~39 cm above the ground — was
traced to a double-counted Z offset in the attachment frame and has been fixed.

A residual ~0.16% pixel-level shadow jitter remains from MuJoCo position-controller
micro-oscillations. This is inherent to external physics + VSM and was deemed
acceptable after exhaustive testing of mitigation approaches.

## Z-Offset Bug (FIXED)

### Root cause

`AMjArticulation::Setup` attaches each robot to the MuJoCo world via
`mjs_addFrame`. The frame's position was set to the actor's spawn transform
(`translation_m` from `URS_scene.json`). However, MuJoCo's free-joint `qpos`
is **relative to the parent body** (the frame), not world coordinates. So the
frame's Z offset was added to `qpos` Z, double-counting the spawn height:

- Spawn: `translation_m Z = 0.39`
- SetPose: `qpos[2] = 0.39` (relative to frame)
- Frame Z = 0.39
- Result: `xpos[2] = 0.39 + 0.39 = 0.78` (floating)

### Fix

For robots with a free joint, the attachment frame is set to identity (0,0,0).
The spawn position is encoded entirely in `qpos` via `SetPose`. For fixed-base
robots (no free joint), the frame retains the spawn position (their only source
of world placement).

File: `Plugins/UnrealRoboticsLab/Source/URLab/Private/MuJoCo/Core/MjArticulation.cpp`

### Verification

Standalone MuJoCo settles `base_link` to Z=0.376 m (feet on ground).
After fix, the UE sim reports the same Z=0.376 m. Walker demo: +4.828 m,
upright 0.969.

## Shadow Jitter Investigation (inherent, accepted)

### Approaches tested

| Approach | Result |
| --- | --- |
| Transform-change threshold (skip small updates) | Eliminates jitter on static, but masks real motion on dynamic scenes |
| `NoCollision` on robot meshes | Removes Chaos broadphase coupling, no visible improvement |
| `r.Shadow.Virtual.Cache.ForceInvalidateDirectional=1` | Eliminates cache staleness, no improvement on texel quantization |
| `r.Lumen.HardwareRayTracing=1` | HW-RT GI more stable, but VSM shadow edges unaffected |
| Kinematic hack (`bAlwaysCreatePhysicsState` + velocity injection) | VSM doesn't read BodyInstance velocity; no improvement |
| Skeletal mesh (single scene proxy) | Same jitter as 22+ static meshes; lost self-shadowing |
| Position/rotation quantization (grid snap) | Made jitter worse (grid-boundary aliasing) |

### Diagnosis

The ~400-500 pixel/frame jitter (0.16% of 640×480 image) on static robots
comes from MuJoCo position-controller micro-oscillations (~0.27° head_pitch
at kp=50). These are real physics — the controllers oscillate fighting gravity.
VSM quantizes the resulting sub-texel shadow-edge movement, producing shimmer.

This is inherent to external physics + VSM. Native UE games avoid it through
Nanite LOD stability and skeletal-mesh-optimised VSM paths, but the underlying
movement is still real. The jitter is invisible during actual robot motion
(walking, head sweep) — only visible when robots are perfectly still.

### Remaining configuration

- `r.Lumen.HardwareRayTracing=1` — enabled for faster GI tracing (not a jitter fix)
- Velocity sync in `MjBody::ApplyRenderState` — reads `Snap.CVel`, converts
  body-frame to world-frame, caches for `GetSpatialVelocity()` queries

## Files changed

### Plugin (`Plugins/UnrealRoboticsLab`)

- `Source/URLab/Private/MuJoCo/Core/MjArticulation.cpp` — conditional attachment
  frame: identity for free-joint robots, spawn position for fixed-base
- `Source/URLab/Private/MuJoCo/Components/Bodies/MjBody.cpp` — velocity sync
  from `Snap.CVel`, `GetSpatialVelocity()` returns cached values

### Project

- `Config/DefaultEngine.ini` — added `r.Lumen.HardwareRayTracing=1`
