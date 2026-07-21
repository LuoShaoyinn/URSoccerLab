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

For the older single-camera Pi Plus smoke robot, the tested head-mounted camera
was:

```xml
<camera name="urlab_origin_camera"
        pos="-0.08 0 0.04"
        xyaxes="0 1 0 -1 0 0"
        fovy="90"
        resolution="640 480" />
```

The negative X offset was intentional for that asset: the old positive-X
location was inside the previous `head_pitch_link` mesh and caused
self-occlusion. The current Pi Plus fixture is stereo and uses fixed eye bodies
under `head_pitch_link`.

## Fixed Pi Plus Robot Contract

The shipped Pi Plus fixture is a fixed robot type, not a runtime-dynamic URDF
loader. URSoccerLab code may therefore treat these names as stable ABI:

- Root link: `base_link`
- Head yaw link: `head_yaw_link`
- Head pitch link: `head_pitch_link`
- Head yaw actuator/joint: `head_yaw_joint`
- Head pitch actuator/joint: `head_pitch_joint`

Stereo cameras should be represented as fixed child links/joints in the robot
asset and as URLab MJCF cameras under the same moving parent body. Do not encode
the head-to-eye offsets in C++.

Use these names for the stereo camera contract:

- Left camera link: `left_eye_camera_link`
- Left camera fixed joint: `left_eye_camera_joint`
- Left URLab/MJCF camera: `left_eye_camera`
- Right camera link: `right_eye_camera_link`
- Right camera fixed joint: `right_eye_camera_joint`
- Right URLab/MJCF camera: `right_eye_camera`

Both fixed camera joints should have parent `head_pitch_link`, so yaw and pitch
motion naturally moves both eyes. The URDF fixed joint origin records the
physical head-to-eye placement for documentation and non-URLab tooling. The
MJCF `<camera>` `pos` and orientation are the source URLab actually renders
from.

Current stereo camera mount convention in MJCF:

```xml
<body name="left_eye_camera_link" pos="0.16 0.03 0.05">
  <camera name="left_eye_camera"
          pos="0 0 0"
          fovy="90"
          resolution="640 480" />
</body>
```

The current stereo fixture intentionally leaves `xyaxes` unset until the Pi
eye optical frame is calibrated against the new mesh. If an explicit orientation
is added, use the same convention for `right_eye_camera` unless the physical
camera model intentionally has a different optical frame.

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
