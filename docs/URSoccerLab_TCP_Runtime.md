# TCP Runtime

## Implemented

- **Modular TCP transport**: `UURSTcpTransportComponent` (auto-created by `AURSSoccerGameMode`) owns per-robot listeners and one global admin listener.
- **Per-robot command sockets**: one TCP listener per active robot. Default ports: `robot_rp0` = 10000, `robot_rp1` = 10001, etc.
- **Admin RPC socket**: one TCP listener on port 11000 (global, shared by all robots).
- **Motor commands**: inbound JSON on the robot port. Keys are actuator names, values are floats. Only recognised actuator names update motor targets; unrecognised keys are silently ignored. The watchdog is refreshed only if at least one actuator was actually changed — an empty `{}` does **not** keep stale commands alive.
- **State publishing**: outbound JSON on the robot port at `StateRateHz` (default 60 Hz). Includes `sim_time`, base pose/velocity, joint qpos/qvel, actuator values, and camera metadata.
- **Vision publishing**: RGB (`0x01`) and depth (`0x02`) are separate,
  versioned binary messages. Scene config selects stereo RGB or aligned RGBD
  and gives RGB/depth independent rates and codecs. Packets are emitted only
  when a new GPU readback is available.
- **Default camera stream**: stereo 640x480 RGB at 30 Hz, JPEG quality 85.
- **Bounded asynchronous encoding**: JPEG encoding and depth
  quantization/compression run on Unreal's worker pool. Each robot permits at
  most one in-flight RGB job and one in-flight depth job, so slow encoding
  drops stale opportunities instead of blocking physics or growing a queue.
- **Single network owner**: URSoccerLab leaves URLab camera rendering and
  readback enabled but disables URLab's legacy ZMQ, shared-memory, and RPC
  transports. Project messages are published only through the consolidated
  TCP transport described here.
- **Physics/render isolation**: MuJoCo steps on URLab's dedicated physics
  thread. The game/TCP thread reads the latest coherent render snapshot rather
  than live `mjData`; endpoint and command state is synchronized separately.
  Rendering and compression can therefore miss camera-rate opportunities
  without stalling or racing the integrator.
- **Camera motion blur**: real camera captures use velocity-based blur with persistent render history. The default amount is `0.5` (a 180-degree shutter), the maximum streak is 5% of screen width, and velocity scaling follows `CameraRateHz`.
- **Outbound write queues**: each client has a `WriteBuffer`. Frames are enqueued non-blocking and flushed every transport tick. Clients whose queue exceeds `MaxSendQueueBytes` (default 4 MB) are disconnected (back-pressure).
- **Command watchdog**: `CommandTimeoutSec` (default 2 s). If no valid command arrives within the timeout, motors are zeroed.

Motion blur can be tuned per run:

```text
-URSMotionBlur=0|1
-URSMotionBlurAmount=0.0..1.0
-URSMotionBlurMax=0.0..100.0
-URSMotionBlurTargetFPS=0..120
```

Target FPS `0` follows the actual render-frame rate. A fixed value is generally
more reproducible for robotics datasets.

Do not disable main-viewport world rendering for the ordinary
`SceneCaptureComponent2D` backend. UE dispatches its deferred captures from the
viewport draw, so disabling the world view freezes the camera render targets.
Use a small offscreen viewport (`-ForceRes -ResX=64 -ResY=64`) to make its cost
negligible. The following switch remains available for render-path diagnostics:

```text
-URSDisableMainViewport=0|1
```

## Frame Format

All TCP communication uses length-prefixed frames:

```text
[4-byte big-endian length][1-byte type][payload]
```

| Type | Value | Payload |
|------|-------|---------|
| JSON | 0x00  | UTF-8 JSON text |
| RGB | 0x01 | Versioned RGB image set |
| Depth | 0x02 | Versioned depth image set |

### RGB and depth message layout

```text
[version u8 = 2] [image_count u8] [flags LE16]
[sequence LE32] [sim_time LE float64]
  per image:
    [camera_name_length u8] [camera_name UTF-8]
    [codec u8] [pixel_format u8] [reserved u8]
    [width LE16] [height LE16]
    [uncompressed_length LE32] [data_length LE32] [data]
```

Codecs are `0x00` raw, `0x01` JPEG, and `0x02` zlib. Pixel formats are
`0x00` BGRA8, `0x01` little-endian float32 depth in metres, and `0x02`
little-endian uint16 depth in millimetres.

A stereo-RGB message contains the synchronized left and right images. In RGBD
mode, the RGB message contains the left image and the independently scheduled
depth message contains depth aligned to that left camera. Sequences are
per-robot and per-message-type; use `sim_time` to correlate vision with state.

The Python client continues to recognize the legacy pre-v2 `0x01` packed
camera payload so old recordings can still be inspected.

## Admin RPC

The admin listener accepts JSON requests of the form:

```json
{"command": "set_pose", "args": {"actor_id": "robot_rp0", ...}}
```

Supported commands:

- **set_pose** — force-write root translation (`translation_m`), root rotation (`rotation_quat_xyzw` as `[x,y,z,w]`), and/or non-root `joint_qpos` under `CallbackMutex` + `mj_forward`. All numeric inputs are validated: NaN/infinity rejected, quaternions normalised. Returns `fixed_base` if translation/rotation is sent to a robot whose root is welded to the world.
- **get_pose** — read `mjData::xpos`/`xquat` of the root body plus non-root qpos.
- **reset** — return the robot to its initial spawn pose.
- **lock_pose / unlock_pose** — hold a robot at a fixed pose (overrides physics).

Minimal example using `py_example/src/ursoccerlab/tcp.py`:

```python
from ursoccerlab import AdminClient

admin = AdminClient("127.0.0.1", 11000)
reply = admin.set_pose("robot_rp0",
    translation_m=[0.5, 0.0, 0.3762],
    rotation_quat_xyzw=[0.0, 0.0, 0.0, 1.0])
print(reply)
admin.reset("robot_rp0")
admin.close()
```

## Quick Client Check

```python
from ursoccerlab import RobotClient

client = RobotClient("127.0.0.1", 10000)
client.send_command({"head_pitch_joint": 0.1})
for kind, data in client.recv():
    print(kind, data)
```

## Dynamic Scene Config

`AURSSoccerGameMode::InitGame` reads `Config/URS_scene.json` before
`AAMjManager::BeginPlay` and spawns the listed robots and dynamic objects via
their type registries. The registered robot type is `pi_plus`; the registered
object type is `soccer_ball`.

The runtime accepts `-URSSceneConfig=<path>` to override the scene JSON. A
relative path is resolved from the project directory; an absolute path may be
used for per-example or externally managed configurations.

```json
{
  "version": "urs_scene_v1",
  "vision": {
    "mode": "rgbd",
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
      "translation_m": [0.0, 0.0, 0.3762],
      "rotation_quat_xyzw": [0.0, 0.0, 0.0, 1.0]
    }
  ]
}
```

Translation and rotation are optional; missing translation falls back to
the type's `DefaultBaseHeightM`, missing rotation to identity. The
component stashes the initial pose per `actor_id` so the admin `reset`
command can return to spawn.

## Validation Run

```bash
make UnrealEditor-Linux-Development ARGS="-project=$PROJ"
"$UE" "$PROJ" -NullRHI -unattended -nop4 -nosplash \
  -ExecCmds="Automation RunTests URSoccerLab; Quit"
```

Run the complete runtime automation suite after any C++ change, together with
the TCP smoke clients in `Tools/runtime/`.

## Full-match camera benchmark

`benchmark_match_vision.py` launches the field, connects one client to every
configured robot, drains every state and vision stream, checks complete
messages, and compares MuJoCo simulation-time advance with wall time:

For a normal production run without the benchmark clients:

```bash
py_example/.venv/bin/python Tools/runtime/run_scene.py \
  --scene-config Config/examples/six_robots_stereo_rgb.json
```

```bash
py_example/.venv/bin/python Tools/runtime/benchmark_match_vision.py \
  --scene-config Config/examples/six_robots_rgbd.json \
  --duration-sec 12 --output Saved/Benchmarks/six_rgbd.json

py_example/.venv/bin/python Tools/runtime/benchmark_match_vision.py \
  --scene-config Config/examples/six_robots_stereo_rgb.json \
  --duration-sec 12 --output Saved/Benchmarks/six_stereo_rgb.json
```

The benchmark uses the production nDisplay atlas by default. Pass
`--scene-capture` only to compare the legacy independent-capture backend.
On the RX 7900 XTX with the indoor-only production level, Lumen HWRT,
hardware-ray-traced MegaLights, JPEG quality 85, and 640x480 sensors:

| 3v3 mode | Delivered rate per robot | Minimum physics/wall ratio |
| --- | --- | ---: |
| Twelve RGB cameras | ~26.7 stereo messages/s (53.3 images/s) | 0.9998 |

Every measured message was complete and there were no sequence gaps. Removing
the Directional Light, Sky Light, Sky Atmosphere, fog, and cloud actors from
the level improved 12-view delivery from 15.5 to 26.7 Hz. Rendering remains
the limiting stage. Atlas readback, asynchronous JPEG, and TCP do not create a
second throughput limit, and the physics clock remains real-time; bounded
vision jobs skip unavailable render/encode opportunities instead of
accumulating latency or blocking MuJoCo.
