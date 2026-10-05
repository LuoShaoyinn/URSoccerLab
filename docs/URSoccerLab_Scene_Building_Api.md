# Scene Building and Configuration

URSoccerLab keeps the authored Unreal environment separate from the runtime
MuJoCo scene. The production level contains the background, lighting, manager,
and project components. A JSON file selects the robots, dynamic objects, poses,
and camera transport settings for each run.

## Illumination and camera effects

Scene JSON can adjust the existing hall lamps without changing the level or
external robot packages:

```json
"lighting": {
  "lamp_intensity_lumens": 224,
  "source_radius_cm": 60,
  "specular_scale": 0.1
}
```

Source radius controls soft shadow/highlight size (0..500 cm); specular scale
controls reflected light intensity (0..1). The hall preset uses broad sources
and restrained highlights to keep black metal readable.

The intensity value sets each of the 21 generated point lights (`URS_AutoEmissiveLamp`
actor tag). Zero turns their illumination off. Omitting `lighting` preserves
the authored intensity. Lighting channels stay on channel 0. This changes
illumination, while the visible Unlit lamp materials keep their authored glow.
Keep `render.auto_exposure` false when comparing illumination settings. This
now explicitly overrides the hall volume's automatic exposure. Use
`render.exposure_compensation` to tune fixed camera brightness (each +1 is
one exposure stop); the hall and camera examples use +2.5.

Add these fields to the existing `render` object:

```json
"motion_blur": true,
"motion_blur_amount": 0.5,
"motion_blur_max_percent": 5,
"motion_blur_target_fps": 0,
"film_grain": {
  "intensity": 0.1,
  "shadows": 1,
  "midtones": 1,
  "highlights": 0.25,
  "texel_size": 1
}
```

Film Grain is Unreal's built-in post-process effect; there is no custom noise
model. Intensity and tone-region multipliers accept 0..1, and texel size accepts
0..4. Intensity zero disables grain. Motion blur amount accepts 0..1, maximum
blur accepts 0..100 percent of screen width, and target FPS accepts integers
0..120. Target FPS zero follows each camera's configured output rate, including
its guest-inspector rate. Existing `URSMotionBlur*` command-line overrides are
retained. `render.enable=false` disables both effects.

Robot eye views and guests share these post-process controls through the
existing nDisplay camera pipeline. Raw/JPEG/AV1 receive the processed RGB;
metric depth remains separate. Settings are applied when the scene config is
loaded; editing the JSON alone does not automatically reload the application.
The next cooked application must include this code once, after which these
JSON values need no rebake.

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
    "visual": {"base_color_map": "textures/field.png"},
    "physics": {
      "friction": [1.0, 0.005, 0.0001], "condim": 3,
      "solref": [0.02, 1.0], "solimp": [0.9, 0.95, 0.001, 0.5, 2]
    }
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
(in metres) and a `visual` object with nonempty `base_color_map` path. `border_x_m` and `border_y_m` are
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

The visual surface retains the original static Nanite mesh and glTF material
parent. External normal, roughness, metallic and AO maps are optional; these
repeat at `visual.detail_tile_size_m` while base color covers the full field.
See [PBR maps and ground contact settings](Field_PBR.md) for their formats and
`field.physics` parameters. No authored field maps are cooked into the material.
The old `field.map_image` key is rejected with a migration message.

The MuJoCo ground remains an infinite flat plane; `field.physics` configures
its friction, contact dimensions and contact solver parameters before compilation. Field dimensions define the
playing surface, not collision walls or out-of-bounds rules.
Goalposts participate in MuJoCo collisions. Dimensions exceeding the authored
hall are allowed but may visually overlap its geometry.

Change the JSON/image and relaunch; neither a mesh import nor an extra MJCF is
required. These are launch-time settings, not live physics-reload controls.
`external/field/example.png` is a local external example input. The entire
`external/` directory is Git-ignored and is not bundled into the simulator;
supply its files separately when using the example scenes.

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

## Robot types and object registry

Robot types are explicitly declared through `robot_types`, mapping names to
external package manifests. There are no built-in robot types. See
[Robot packages](Robot_Packages.md) for the `urs_robot_v1` contract and validation.

Objects still use the baked object registry; `soccer_ball` defaults to a base
height of 0.075 m. Joint/actuator identifiers remain the supplied MJCF names.

## Source and baked assets

```text
Assets/                              editable source of truth
  Objects/soccer_ball/*.xml|meshes/  dynamic ball physics and visual
  Scenes/SoccerField/physics/*.xml   flat MuJoCo field collision

external/robots/<type>/               ignored, distributed separately
  robot.json                         identity and bindings
  model.xml                          physics plus GLB visual references
  meshes/*.glb                       Unreal render geometry and PBR

Content/                             Unreal-generated, tracked with Git LFS
  Levels/URS_SoccerField.umap        authored hall, goals, ground physics and lighting
  URSoccerLab/Objects/...            baked object Blueprint and meshes
  URSoccerLab/Scenes/...             background assets referenced by the level
```

The original background GLB is intentionally not retained: the `.umap` and its
referenced Content assets are the authoritative visual scene. The pitch surface is created at runtime from required external configuration.
MuJoCo sees only the flat ground plane plus configured articulations.

Robot GLB mesh/geoms are extracted by the runtime loader and removed from the
MuJoCo input; see [Robot packages](Robot_Packages.md). Objects retain their baked
editor workflow and `visual__` frame convention.

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
