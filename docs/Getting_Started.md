# Start a match

Place the AppImage, scene JSON and external assets in one working directory.
You do not need Unreal Engine or Docker on the machine running the AppImage.
Keep the directory structure inside each robot package intact.

## Create scene.json

This example starts one Booster K1 robot with stereo cameras, a ball and two
goals in the fixed hall. It assumes the installation layout shown in the
[README](../README.md). The robot package must have id `booster_k1`.

```json
{
  "version": "urs_scene_v1",
  "encoder": {"codec": "av1", "backend": "auto", "fallback_codec": "h264"},
  "robot_types": {
    "booster_k1": "assets/robots/booster_k1/robot.json"
  },
  "robots": [
    {
      "actor_id": "robot_rp0",
      "type": "booster_k1",
      "translation_m": [-1, 0, 0.552],
      "rotation_quat_xyzw": [0, 0, 0, 1]
    }
  ],
  "objects": [
    {
      "actor_id": "ball",
      "type": "soccer_ball",
      "physics": {
        "radius_m": 0.11,
        "mass_kg": 0.43,
        "friction": [0.6, 0.005, 0.001]
      }
    }
  ],
  "field": {
    "length_m": 9,
    "width_m": 6,
    "visual": {"base_color_map": "assets/field/albedo.png"}
  },
  "goals": {
    "width_m": 1.8,
    "height_m": 1.2,
    "post_radius_m": 0.05,
    "poses": [
      {"translation_m": [-4.5, 0, 0], "yaw_deg": 0},
      {"translation_m": [4.5, 0, 0], "yaw_deg": 180}
    ]
  },
  "render": {
    "lumen": true,
    "hardware_ray_tracing": true,
    "auto_exposure": false,
    "exposure_compensation": 2.5,
    "motion_blur": false
  },
  "lighting": {
    "emissive_intensity": 10,
    "lamp_intensity_lumens": 0
  },
  "camera_freq": 30,
  "state_freq": 60,
  "vision": {
    "mode": "stereo_rgb",
    "rgb": {
      "bitrate_kbps": 2000,
      "keyframe_interval_s": 2
    }
  },
  "guest_inspector": {
    "enabled": true,
    "port": 12000,
    "max_guests": 1,
    "width": 640,
    "height": 480,
    "rate_hz": 30,
    "bitrate_kbps": 2000,
    "keyframe_interval_s": 2
  }
}
```

The example configures a scene, not a standing or walking controller. Connect a
controller to keep an unlocked robot upright. Booster servos and camera placement
in the normalized package are provisional.

Distances use metres: X forward, Y left, Z up. The robot position is its root
body position, not its eye position. Robot root height depends on the package.
The ball's omitted position puts its centre at its radius above the ground.
If you set a ball position explicitly, set its Z accordingly.

Both goal poses and field dimensions/image are required. Asset paths may be
absolute or relative to `scene.json`. Missing or invalid assets fail startup.

## Run and receive images

```bash
chmod +x URSoccerLab.AppImage
./URSoccerLab.AppImage scene.json
```

The simulator renders offscreen. In another terminal, use the Python client:

```python
import time
from ursoccerlab import RobotClient

robot = RobotClient("127.0.0.1", 10000)
try:
    while True:
        for kind, data in robot.recv():
            if kind == "state":
                print(data)
        time.sleep(0.002)
finally:
    robot.close()
```

Install the shipped client wheel before running this snippet. To save camera
images and guest recordings, see [client examples](https://github.com/LuoShaoyinn/URSoccerLab/blob/main-cli/python/README.md).
With AV1, a new connection may wait until the next periodic keyframe; the example
uses a two-second interval. Robot state arrives independently of camera frames.

## Change appearance and physics

Field and ball `visual` blocks can reference external base color, normal,
roughness, metallic and AO maps. Base color is required when a visual block is
present; other maps are optional. Ball textures wrap using the ball mesh's UVs.
For example, add this beside the ball's `physics`:

```json
"visual": {
  "base_color_map": "assets/ball/albedo.png",
  "normal_map": "assets/ball/normal.png",
  "roughness_map": "assets/ball/roughness.png",
  "ao_map": "assets/ball/ao.png",
  "normal_format": "opengl"
}
```

`lighting.emissive_intensity` sets absolute Unreal linear emission, not lumens.
The default preset uses 10. `lamp_intensity_lumens` controls the separate auxiliary
point lights; zero leaves them off. Camera motion blur and Unreal film grain are
configured under `render`. See the [scene reference](URSoccerLab_Scene_Building_Api.md)
for all fields, defaults and valid ranges.

Restart after changing configuration or textures. The launcher accepts only the
JSON path and does not watch files for changes.

## Troubleshooting

- **Usage error:** supply exactly one existing JSON file; do not append engine flags.
- **No window:** expected; connect a robot or guest client to receive images.
- **Missing robot/map:** inspect paths relative to the JSON file; external assets
  are supplied separately from the application and are not in Git.
- **Encoding fails:** check the `[URS Encoder]` messages for the selected/rejected backend. Set shared `encoder.codec` to `"jpeg"` or `"raw"` to diagnose separately.
- **FUSE unavailable:** run with `APPIMAGE_EXTRACT_AND_RUN=1` in the environment.
- **Connection refused:** inspect startup logs under `~/.local/share/URSoccerLab/`
  and verify the robot order and inspector settings in JSON.
