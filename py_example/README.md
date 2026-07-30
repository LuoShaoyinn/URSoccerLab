# URSoccerLab Python Examples

TCP-based clients for the URSoccerLab robot control API.

## Setup

```bash
cd py_example
uv sync
```

The default environment uses Python 3.12 and does not install PyTorch. Choose
exactly one PyTorch backend when running the walking-policy example:

```bash
uv sync --extra torch_cpu
uv sync --extra torch_rocm
uv sync --extra torch_cuda
```

The extras are mutually exclusive. The ROCm and CUDA builds are supported on
Linux; `torch_rocm` currently selects ROCm 7.2.4 and `torch_cuda` selects
CUDA 13.0.

## RobotClient — motor commands, state, and camera

```bash
uv run python -c "
from ursoccerlab import RobotClient
client = RobotClient('127.0.0.1', 10000)
client.send_command({'head_pitch_joint': 0.1})
for kind, data in client.recv():
    print(kind, type(data))
"
```

- Motor commands: JSON dict of `{actuator_name: float}`
- State: JSON with `sim_time`, `base`, `joints`, `actuators`, `cameras`
- Camera: packed binary with `sim_time`, width, height, JPEG/raw pixels

## AdminClient — set_pose, reset, lock_pose

```python
from ursoccerlab import AdminClient
admin = AdminClient('127.0.0.1', 11000)
admin.set_pose('robot_rp0', translation_m=[0.5, 0.0, 0.3762])
admin.reset('robot_rp0')
admin.close()
```

## Run A Scene

Start an offscreen runtime from the project root with the scene config named
by the example:

```bash
"$HOME/Unreal_Engine_5.7.4/Engine/Binaries/Linux/UnrealEditor" \
  "$PWD/URSoccerLab.uproject" /Game/Levels/URS_SoccerField -game -RenderOffscreen \
  -DDC-ForceMemoryCache -unattended -nop4 -nosplash -NoSound \
  -URSSceneConfig="$PWD/Config/examples/two_robots_face_to_face.json"
```

Each client uses TCP only. `robot_rp0` is port `10000`; `robot_rp1` is port
`10001`. Output files go under `py_example/out/` and are ignored by Git.

## 1. Head Motion

Two standing robots are at `(-1, 0)` and `(1, 0)`, facing one another. Both
heads sweep while the legs remain uncommanded. Both left-eye videos are
recorded.

```bash
uv run python examples/move_head.py \
  --video0 out/head_motion_rp0.mp4 --video1 out/head_motion_rp1.mp4
```

Use `Config/examples/two_robots_face_to_face.json` in the runtime command.

## 2. Walker And Observer

`robot_rp0` starts at `(0, 0)` and walks along `+X`. `robot_rp1` stands at
`(0, 3)` facing the walker. The policy sends motor commands only to `robot_rp0`
and records both left-eye cameras:

```bash
uv sync --extra torch_rocm
uv run --extra torch_rocm python examples/walk_policy.py \
  --vx 0.35 --duration 8 \
  --video out/walker.mp4 \
  --observer-video out/observer.mp4
```

Use `Config/examples/walker_and_observer.json` in the runtime command.
It requires `refs/mos-brain/simulation/mujoco/assets/policies/pi_plus_model_40000.pt`.
The policy was trained against the older mos-brain Pi model. It is useful for
exercising the TCP motor and camera path, but is not a validated gait for the
current Pi MJCF until its dynamics and actuator calibration are matched or the
policy is retrained.

## 3. Static Face-To-Face

The same `(-1, 0)` / `(1, 0)` scene, but neither robot receives a motor
command. This is a two-camera standing capture, not a control test:

```bash
uv run python examples/standing.py \
  --video0 out/standing_rp0.mp4 --video1 out/standing_rp1.mp4
```

Use `Config/examples/two_robots_face_to_face.json` in the runtime command.

## Vision Smoke Test

From the project root:

```bash
uv run --project py_example python Tools/runtime/run_vision_smoke_test.py
```

Starts the simulator, connects via TCP, sends zero commands, captures camera
frames, and validates non-blank RGB content.

The runner can compare JPEG and uncompressed BGRA transport:

```bash
uv run --project py_example python Tools/runtime/run_vision_smoke_test.py \
  --camera-compress jpeg --jpeg-quality 85 \
  --out py_example/out/vision_jpeg_q85

uv run --project py_example python Tools/runtime/run_vision_smoke_test.py \
  --camera-compress raw \
  --out py_example/out/vision_raw
```

Each output directory includes `transport.json` with the received codec and
encoded payload sizes. The saved `camera.png` is only a decoded inspection
image; it does not indicate the wire encoding.

The launcher uses Unreal's persistent Zen derived-data cache by default so
compiled shaders are reused. `--force-memory-ddc` is available only as a
recovery option; it discards compiled shader data when Unreal exits.

## Layout

```text
src/ursoccerlab/     reusable TCP, camera, and video APIs
examples/            standing, head-motion, walking, and vision clients
tests/               protocol and camera parser tests
out/                 ignored local captures
```

Run the unit tests without installing another test framework:

```bash
uv run python -m unittest discover -s tests
```

## Protocol

Frame format: `[4-byte BE length][1-byte type][payload]`

- Type `0x00`: JSON (state, command, admin)
- Type `0x01`: Camera (binary packed)

Camera layout:
```
[1-byte codec][1-byte num_cams][8-byte LE double sim_time]
per-cam: [width LE16][height LE16][data_len LE32][pixel data]
```

Codec: `0x00` = raw BGRA, `0x01` = JPEG.
