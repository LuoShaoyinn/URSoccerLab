# Scene Building and Configuration

URSoccerLab keeps the authored Unreal environment separate from the runtime
MuJoCo scene. The production level contains the background, lighting, manager,
and project components. A JSON file selects the robots, dynamic objects, poses,
and camera transport settings for each run.

## Startup flow

```text
/Game/Levels/URS_SoccerField.umap
        + Config/URS_scene.json
                    |
                    v
AURSSoccerGameMode::InitGame
        |
        +-> register robot and object types
        +-> apply JSON and spawn baked AMjArticulation Blueprints
        +-> disable URLab legacy network transports
                    |
                    v
AAMjManager::BeginPlay -> compile one MuJoCo model -> physics thread
                    |
                    v
UURSTcpTransportComponent::BeginPlay -> robot/admin listeners
```

`InitGame` runs before `BeginPlay`, so every configured articulation exists
before URLab compiles the MuJoCo model. Robots and dynamic objects must not be
baked into the level.

## Scene JSON

```json
{
  "version": "urs_scene_v1",
  "field": {
    "length_m": 9.0, "width_m": 6.0,
    "border_x_m": 0.8, "border_y_m": 0.9,
    "map_image": "field.png"
  },
  "goals": {
    "width_m": 1.8, "height_m": 1.2, "post_radius_m": 0.05,
    "poses": [
      {"translation_m": [-4.5, 0, 0], "yaw_deg": 0},
      {"translation_m": [4.5, 0, 0], "yaw_deg": 180}
    ]
  },
  "vision": {
    "mode": "stereo_rgb",
    "left_camera": "left_eye",
    "right_camera": "right_eye",
    "rgb": {"rate_hz": 30, "compression": "jpeg", "jpeg_quality": 85},
    "depth": {
      "rate_hz": 15,
      "compression": "zlib_u16_mm",
      "max_depth_m": 65.535
    }
  },
  "robots": [
    {
      "actor_id": "robot_rp0",
      "type": "pi_plus",
      "translation_m": [-1.0, 0.0, 0.3762],
      "rotation_quat_xyzw": [0.0, 0.0, 0.0, 1.0]
    }
  ],
  "objects": [
    {
      "actor_id": "ball",
      "type": "soccer_ball",
      "translation_m": [0.0, 0.0, 0.075],
      "rotation_quat_xyzw": [0.0, 0.0, 0.0, 1.0]
    }
  ]
}
```

The runtime accepts `-URSSceneConfig=<path>`. Relative paths are resolved from
the project directory; absolute paths are useful for generated experiments.

### Fields and defaults

| Field | Required | Default |
| --- | :---: | --- |
| `version` | yes | must be `urs_scene_v1` |
| `vision.mode` | no | `stereo_rgb`; alternative: `rgbd` |
| `vision.left_camera` | no | `left_eye` |
| `vision.right_camera` | no | `right_eye` |
| `vision.rgb.rate_hz` | no | `30` |
| `vision.rgb.compression` | no | `jpeg`; alternative: `raw` |
| `vision.rgb.jpeg_quality` | no | `85` |
| `vision.depth.rate_hz` | no | `15` |
| `vision.depth.compression` | no | `zlib_u16_mm`; alternatives: `raw_f32`, `raw_u16_mm` |
| `vision.depth.max_depth_m` | no | `65.535` |
| `robots` | yes | array, possibly empty |
| `objects` | no | empty array |
| `robots[].actor_id`, `robots[].type` | yes | unique ID and registered type |
| `objects[].actor_id`, `objects[].type` | yes | unique ID and registered type |
| `translation_m` | no | type-specific base height at X/Y zero |
| `rotation_quat_xyzw` | no | `[0, 0, 0, 1]` |

`robots[].joint_positions_rad` may provide an explicit initial posture. When
present, it must contain every non-root joint and no unknown names. This keeps
policy-specific poses in configuration instead of runtime C++.

`stereo_rgb` publishes both named RGB cameras in one synchronized message.
`rgbd` publishes left-eye RGB plus independently scheduled depth aligned with
that viewpoint. JPEG is the practical RGB default; depth remains numeric and
uses raw float, raw millimetres, or lossless zlib-compressed millimetres.

## External field

The `field` object is mandatory, including positive `length_m` and `width_m`
(in metres) and a nonempty `map_image` path. `border_x_m` and `border_y_m` are
nonnegative borders on each side, defaulting to 0.8 and 0.9 metres. PNG and JPEG
images load at launch; relative paths resolve from the scene JSON directory,
including when launching from another working directory. Missing or unreadable
images abort startup. There is no built-in pitch-image fallback.

The hall stays fixed. A generic runtime plane covers
`(length_m + 2*border_x_m) × (width_m + 2*border_y_m)`, centered at world zero.
The full image covers this plane: left/right map to MuJoCo -X/+X and top/bottom
to +Y/-Y. Use an image whose markings and borders match your dimensions; the
simulator does not infer pitch geometry from pixels. Goal placement is controlled
independently by the mandatory `goals` block.

The MuJoCo ground remains an infinite flat plane. Field dimensions define the
playing surface, not collision walls or out-of-bounds rules.
Goalposts participate in MuJoCo collisions. Dimensions exceeding the authored
hall are allowed but may visually overlap its geometry.

Change the JSON/image and relaunch; neither a mesh import nor an extra MJCF is
required. These are launch-time settings, not live physics-reload controls.
[`Assets/FieldMaps/example.png`](../Assets/FieldMaps/example.png) is an external
example input and is not bundled into the simulator.

## Goalposts

`goals` is mandatory. Specify positive `width_m`, `height_m`, `post_radius_m`,
and exactly two `poses`, each with `translation_m` and `yaw_deg`:

```json
"goals": {
  "width_m": 1.8,
  "height_m": 1.2,
  "post_radius_m": 0.05,
  "poses": [
    {"translation_m": [-4.5, 0, 0], "yaw_deg": 0},
    {"translation_m": [4.5, 0, 0], "yaw_deg": 180}
  ]
}
```

The pose origin is the ground-level center of the opening. Coordinates use
MuJoCo metres (+X forward, +Y left, +Z up); positive yaw turns +X toward +Y.
Each goal's opening lies in its local Y-Z plane. Width is the clear opening
between posts and height is the clearance under the crossbar. Both goals share
these dimensions. All three parts have the same cylinder radius.

Each goal consists of two vertical cylinders and one horizontal cylinder.
The runtime generates six matching visual and static MuJoCo collision cylinders;
balls and robots can hit them. No net or other goal structure is generated.
There are no baked goal meshes, implicit poses, or field-size placement defaults.
When changing the field length, set the two goal positions explicitly as needed.
Edit JSON and relaunch; no goal rebake or additional MJCF is required.

## Ball overrides

A `soccer_ball` object may specify:

```json
"physics": {
  "radius_m": 0.11,
  "mass_kg": 0.43,
  "friction": [0.6, 0.005, 0.001],
  "solref": [0.02, 0.7]
}
```

Omitted values retain the source defaults: radius 0.075 m, mass 0.2 kg,
friction `[0.8, 0.02, 0.03]`, and `solref [-5000, -20]`. The friction array is
sliding, torsional, and rolling friction. `solref` uses MuJoCo's native contact
parameters; it is not a restitution coefficient. Positive pairs specify
contact time constant/damping ratio; nonpositive pairs use the direct format.

The collision sphere and GLB visual resize together. MuJoCo recomputes solid
sphere inertia from radius and mass. When `translation_m` is omitted, the ball
center height follows its configured radius; an explicit pose takes precedence.
Overrides apply before model compilation. No ball rebake is required.

## Coordinates

Configuration uses the MuJoCo robot frame in metres: +X forward, +Y left, +Z
up. Quaternion wire order is `[x, y, z, w]`. URLab performs the Unreal
centimetre and handedness conversion; project code must not apply another Y
flip.

| Facing direction | `rotation_quat_xyzw` |
| --- | --- |
| +X | `[0, 0, 0, 1]` |
| -X | `[0, 0, 1, 0]` |
| +Y | `[0, 0, 0.7071, 0.7071]` |
| -Y | `[0, 0, -0.7071, 0.7071]` |

## Type registries

Registries map the short JSON `type` to a baked Unreal Blueprint and a default
base height. Defaults are registered in `FURSoccerLabModule::StartupModule`.

| Kind | Type | Blueprint | Base height |
| --- | --- | --- | ---: |
| robot | `pi_plus` | `/Game/URSoccerLab/Robots/pi_plus/pi_plus` | 0.3762 m |
| object | `soccer_ball` | `/Game/URSoccerLab/Objects/soccer_ball/soccer_ball` | 0.075 m |

Adding a type requires an authoritative asset directory under `Assets`, a
baked Blueprint under `/Game/URSoccerLab`, and one registry entry. Robot names,
joint names, actuator names, and camera names form part of the external API.

## Source and baked assets

```text
Assets/                              editable source of truth
  Robots/pi_plus/pi_plus.xml         robot physics, names, cameras, visual frames
  Robots/pi_plus/meshes/*.glb        Unreal-only robot visuals
  Objects/soccer_ball/*.xml|meshes/  dynamic ball physics and visual
  Scenes/SoccerField/physics/*.xml   flat MuJoCo field collision

Content/                             Unreal-generated, tracked with Git LFS
  Levels/URS_SoccerField.umap        authored hall, goals, ground physics and lighting
  URSoccerLab/Robots/...             baked robot Blueprint and meshes
  URSoccerLab/Objects/...            baked object Blueprint and meshes
  URSoccerLab/Scenes/...             background assets referenced by the level
```

The original background GLB is intentionally not retained: the `.umap` and its
referenced Content assets are the authoritative visual scene. The pitch surface is created at runtime from required external configuration.
MuJoCo sees only the flat ground plane plus configured articulations.

Robot and object GLBs are not passed to MuJoCo. Empty MJCF frames named
`visual__<mesh-name>` preserve the body-relative visual transform while the
editor import tools attach the matching GLB to the baked Blueprint. See the
[robot](../Assets/Robots/README.md) and
[object](../Assets/Objects/README.md) conventions.

## Rebuilding and validation

Editor scripts and exact commands are documented in [Tools](../Tools/README.md).
The common checks are:

```bash
python3 Tools/editor/validate_baked_assets.py

uv run --project py_example python Tools/runtime/run_vision_smoke_test.py

uv run --project py_example python Tools/runtime/run_scene.py \
  --scene-config Config/examples/six_robots_stereo_rgb.json
```

Maintained match configurations include six-robot stereo RGB, six-robot RGBD,
and ten-robot/twenty-camera stereo RGB under `Config/examples/`.
