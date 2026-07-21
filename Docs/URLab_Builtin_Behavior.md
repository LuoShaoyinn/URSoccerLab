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

Project code should therefore:

- Put physical camera placement and optical orientation in MJCF.
- Use `xyaxes`, `quat`, or another MuJoCo-supported orientation attribute in
  the MJCF when changing the optical frame.
- Avoid setting `CaptureComponent` relative location or rotation in URSoccerLab.
- Only configure project-owned transport/render options such as ZMQ endpoint,
  shared-memory enablement, resolution fallback, FOV fallback, and ray-tracing
  use.

For the current Pi Plus smoke robot, the tested head-mounted camera is:

```xml
<camera name="urlab_origin_camera"
        pos="-0.08 0 0.04"
        xyaxes="0 1 0 -1 0 0"
        fovy="90"
        resolution="640 480" />
```

The negative X offset is intentional for this asset: the old positive-X
location was inside the `head_pitch_link` mesh and caused self-occlusion.

## URSoccerLab Ownership

URLab owns:

- MJCF/URDF import into Unreal components.
- MuJoCo-to-Unreal position and rotation conversion.
- Physics stepping and imported articulation/component sync.
- `UMjCamera` render target creation and optical-frame conversion.
- Builtin camera streaming registration through `UMjNetworkManager`.

URSoccerLab owns:

- Project scene assets and saved field level.
- Project robot fixtures under `Assets/MosBrainCameraTest`.
- The external robot command/state protocol.
- Per-robot port assignment and metadata.
- Editor smoke fixtures that assemble a test map from URLab imports.

When behavior looks wrong, first check the MJCF/asset frame and URLab import
result before adding C++ transform corrections in URSoccerLab.
