# TCP Runtime

## Implemented

- **Modular TCP transport**: `UURSTcpTransportComponent` coordinates a replaceable `IURSNetworkService`. The TCP socket worker owns per-robot listeners and one global admin listener. See [runtime boundaries](Runtime_Architecture.md).
- **Per-robot command sockets**: one TCP listener per active robot. Default ports: `robot_rp0` = 10000, `robot_rp1` = 10001, etc.
- **Admin RPC socket**: one TCP listener on port 11000 (global, shared by all robots).
- **Motor commands**: inbound JSON on the robot port. Keys are actuator names, values are floats. Only recognised actuator names update motor targets; unrecognised keys are silently ignored. The watchdog is refreshed only if at least one actuator was actually changed — an empty `{}` does **not** keep stale commands alive.
- **State publishing**: outbound JSON on the robot port at `StateRateHz` (default 60 Hz). Includes `sim_time`, base pose/velocity, joint qpos/qvel, actuator values, and camera metadata.
- **Vision publishing**: the independent camera component emits versioned RGB (`0x01`) image sets after GPU readback. RGBD mode also publishes independently scheduled lossless depth (`0x02`).
- **Default camera stream**: example scenes use stereo 640x480 per eye at 30 Hz, hardware video. JPEG/raw remain selectable. See [AV1 runtime](AV1_Runtime.md).
- **Bounded asynchronous encoding**: `UURSCameraStreamComponent` owns capture scheduling and `FImageEncoder` runs on Unreal's worker pool. Each robot permits at most one in-flight RGB job per scene generation. No socket or packet-framing code runs in the camera component or encoder.
- **Single network owner**: URSoccerLab leaves URLab camera rendering and
  readback enabled but disables URLab's legacy ZMQ, shared-memory, and RPC
  transports. Project messages are published only through the consolidated
  TCP transport described here.
- **Physics/render isolation**: MuJoCo steps on URLab's dedicated physics
  thread. Unreal reads a coherent render snapshot, while the network worker reads
  its own state snapshot buffer. Neither hot path reads live `mjData`; endpoint
  and command state is synchronized separately.
  Rendering and compression can therefore miss camera-rate opportunities
  without stalling or racing the integrator.
- **Camera motion blur**: real camera captures use velocity-based blur with persistent render history. JSON enables motion blur explicitly; its amount defaults to `0.5`, maximum streak to 5% of screen width, and target FPS zero follows the configured camera rate.
- **Outbound buffering**: the network worker flushes nonblocking writes. Each client keeps one latest video frame waiting behind any partially sent frame. The byte stream is never truncated mid-frame; clients whose unsent byte buffer exceeds 4 MiB are disconnected.
- **Admin threading**: socket I/O runs on the network worker. Ordered requests cross a queue to the game-thread admin service, and replies return to the originating connection.
- **Command watchdog**: `CommandTimeoutSec` (default 0.1 s). If no valid command arrives within the timeout, motors are zeroed.

Configure motion blur, lighting and camera rates in scene JSON; see the
[scene reference](URSoccerLab_Scene_Building_Api.md). The AppImage accepts only
`./URSoccerLab.AppImage scene.json`. Internal render diagnostic switches belong
to developer tools, not the packaged user interface.

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
    [codec u8] [pixel_format u8] [image_flags u8]
    [width LE16] [height LE16]
    [uncompressed_length LE32] [data_length LE32] [data]
```

Codecs are `0x00` raw, `0x01` JPEG, `0x02` zlib, `0x03` AV1, `0x04` H.264 and `0x05` H.265.
Video image flags bit 0 marks a keyframe; its inner payload, epochs and stereo
packing are documented in [AV1 runtime](AV1_Runtime.md). Pixel formats are
`0x00` BGRA8, `0x01` little-endian float32 depth in metres, and `0x02`
little-endian uint16 depth in millimetres.

A stereo-RGB message contains both eyes: video codecs pack them side by side in one
encoded image entry, while JPEG/raw carry two separate entries. The Python
client exposes named left/right images for both layouts. In RGBD
mode, the RGB message contains the left image and the independently scheduled
depth message contains depth aligned to that left camera. Sequences are
per-robot and per-message-type; `sim_time` is sampled at consumption and does not guarantee exact exposure/state synchronization.

The Python client continues to recognize the legacy pre-v2 `0x01` packed
camera payload so old recordings can still be inspected.

## Admin RPC

The admin listener accepts JSON requests of the form:

```json
{"command": "set_pose", "args": {"actor_id": "robot_rp0", "translation_m": [0.5, 0, 0.3762]}}
```

Supported commands:

- **set_pose** — force-write root translation (`translation_m`), root rotation (`rotation_quat_xyzw` as `[x,y,z,w]`), and/or non-root `joint_qpos` under `CallbackMutex` + `mj_forward`. All numeric inputs are validated: NaN/infinity rejected, quaternions normalised. Returns `fixed_base` if translation/rotation is sent to a robot whose root is welded to the world.
- **get_pose** — read `mjData::xpos`/`xquat` of the root body plus non-root qpos.
- **reset** — return the robot to its initial spawn pose.
- **lock_pose / unlock_pose** — hold a robot at a fixed pose (overrides physics).

Minimal example using the installed `ursoccerlab` wheel:

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
client.send_command({"head_pitch_joint_servo": 0.1})
for kind, data in client.recv():
    print(kind, data)
```

## Scene configuration

Scene JSON declares external robot packages, robot/object poses, mandatory field
and goals, RGB/depth modes, guest cameras, lighting and effects. See
[Getting started](Getting_Started.md) for a runnable example and the
[scene reference](URSoccerLab_Scene_Building_Api.md) for defaults. Relative asset
paths resolve from the JSON directory. Admin reset returns each actor to its
configured initial pose.

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
python Tools/runtime/run_scene.py \
  --scene-config Config/examples/six_robots_stereo_rgb.json
```

```bash
python Tools/runtime/benchmark_match_vision.py \
  --scene-config Config/examples/six_robots_rgbd.json \
  --duration-sec 12 --output artifacts/benchmarks/six_rgbd.json

python Tools/runtime/benchmark_match_vision.py \
  --scene-config Config/examples/six_robots_stereo_rgb.json \
  --duration-sec 12 --output artifacts/benchmarks/six_stereo_rgb.json
```

The benchmark uses the production nDisplay atlas by default. Pass
`--scene-capture` only to compare the legacy independent-capture backend.
Measure your own scene and GPU; delivered rates depend on view count,
resolution, lighting and encoding settings. Dated measurements are preserved
under [experiments](experiments/README.md) and are not current throughput guarantees.
