# URLab Builtin Behavior

This note records the URLab behavior URSoccerLab should rely on, so project
code does not grow extra coordinate or camera patches around the dependency.

## Frames

MuJoCo and the robot assets use a right-handed, Z-up frame in meters:

- X points forward, toward the opponent goal.
- Y points left.
- Z points up.

Unreal Engine uses a left-handed, Z-up frame in centimeters:

- X points forward, toward the opponent goal.
- Y points right.
- Z points up.

URLab converts imported MuJoCo positions with:

```text
UE.X = 100 * MJ.X
UE.Y = -100 * MJ.Y
UE.Z = 100 * MJ.Z
```

URLab converts MuJoCo quaternions in `w x y z` order to Unreal `x y z w`
with the same handedness flip:

```text
UE = (-MJ.x, MJ.y, -MJ.z, MJ.w)
```

Project code should therefore write robot pose data in MuJoCo/robot
coordinates and let URLab import/sync it. Do not add another Y flip in
URSoccerLab runtime code.

## Cameras

MuJoCo cameras use optical forward `-Z` and image up `+Y`. Unreal
`USceneCaptureComponent2D` uses forward `+X` and up `+Z`.

URLab handles this inside `UMjCamera` by creating a child
`SceneCaptureComponent2D` and applying the camera-frame correction there. The
MJCF camera `pos` and orientation attributes belong on the `UMjCamera` parent;
URLab imports those through the same MuJoCo-to-Unreal position and quaternion
conversion used for other components.

For imported fixed cameras, the native runtime path is Unreal attachment
inheritance: `UMjCamera` is attached under the imported moving body component
such as `head_pitch_link`. URLab updates the body components from MuJoCo render
state each frame, and Unreal propagates the body transform to child camera
components. URSoccerLab should not manually copy `xpos`/`xquat` or render
snapshot body poses into camera transforms for this case.

In this path, the cameras may not appear as MuJoCo `mjModel->ncam` entries
available through `cam_xpos`/`cam_xmat`; they are still valid URLab/Unreal
rendering cameras because they exist as `UMjCamera` components attached to the
robot hierarchy.

Project code should therefore:

- Put physical camera placement and optical orientation in MJCF.
- Use `xyaxes`, `quat`, or another MuJoCo-supported orientation attribute in
  the MJCF when changing the optical frame.
- Avoid setting `CaptureComponent` relative location or rotation in URSoccerLab.
- Only configure project-owned transport/render options such as ZMQ endpoint,
  shared-memory enablement, resolution fallback, FOV fallback, and ray-tracing
  use.

For a robot eye link with `+X` forward, `+Y` left, and `+Z` up, use pure
MuJoCo camera axes:

```xml
xyaxes="0 -1 0 0 0 1"
```

That maps camera image-right to eye `-Y`, camera image-up to eye `+Z`, and
camera optical forward `-Z` to eye `+X`.

## Fixed Pi Plus Robot Contract

The shipped Pi Plus fixture is a fixed robot type, not a runtime-dynamic URDF
loader. URSoccerLab code may therefore treat these names as stable ABI:

- Root link: `base_link`
- Head yaw link: `head_yaw_link`
- Head pitch link: `head_pitch_link`
- Head yaw actuator/joint: `head_yaw_joint`
- Head pitch actuator/joint: `head_pitch_joint`

Stereo cameras should be represented directly in the per-robot MJCF under the
moving head body. Do not encode the head-to-eye offsets in C++.

Use these names for the stereo camera contract:

- Left URLab/MJCF camera: `left_eye`
- Right URLab/MJCF camera: `right_eye`

Both cameras are children of `head_pitch_link`, so yaw and pitch motion
naturally moves both eyes. The MJCF `<camera>` `pos` and orientation are the
source URLab renders from.

Current stereo camera mount convention in MJCF:

```xml
<camera name="left_eye"
        pos="0.1 0.03 0.05"
        xyaxes="0 -1 0 0 0 1"
        fovy="60"
        resolution="640 480" />
```

This means:

- Rendered optical forward is eye `+X`.
- Rendered image-left is eye `+Y`.
- Rendered image-up is eye `+Z`.
- The MJCF remains valid for native MuJoCo tooling.
- URLab imports this camera frame, converts it to Unreal coordinates, and lets
  `UMjCamera` apply its built-in capture-component correction.

Use the same `xyaxes` convention for `right_eye` unless the physical
camera model intentionally has a different optical frame.

## URSoccerLab Ownership

URLab owns:

- MJCF/URDF import into Unreal components.
- MuJoCo-to-Unreal position and rotation conversion.
- Physics stepping and imported articulation/component sync.
- `UMjCamera` render target creation and optical-frame conversion.
- Optional builtin ZMQ, shared-memory, and RPC transports. URSoccerLab disables
  these before `AAMjManager::BeginPlay` and keeps URLab's camera rendering and
  readback path enabled.

URSoccerLab owns:

- Project scene assets and saved field level.
- Project robot fixtures under `Assets/Robots`.
- The external robot command/state protocol.
- Consolidated per-robot TCP transport, port assignment, and metadata.
- Editor smoke fixtures that assemble a test map from URLab imports.

When behavior looks wrong, first check the MJCF/asset frame and URLab import
result before adding C++ transform corrections in URSoccerLab.
