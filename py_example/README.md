# Python clients for URSoccerLab

Start the simulator with `./URSoccerLab.AppImage scene.json` in a separate terminal.
The application accepts only the JSON path. See [Getting started](../docs/Getting_Started.md)
for assets, a complete scene and startup troubleshooting. The commands below
configure client programs, not the simulator's command-line interface.


TCP-based clients for the URSoccerLab robot control API.
Install the shipped wheel and use it from your own application. This directory
provides API documentation and example source to browse and adapt; a repository
checkout is only needed for source development or running the checkout's examples.

## Install the client wheel

Use Python 3.12 in your own virtual environment. Install the separately supplied
wheel; the repository and Unreal Engine are not required by the client:

```bash
python -m pip install ursoccerlab_client-0.1.0-py3-none-any.whl
```

```python
from ursoccerlab import RobotClient, AdminClient, InspectorClient
from ursoccerlab.media import camera_to_rgb, depth_to_meters
```

The distribution is named `ursoccerlab-client`; its import name is `ursoccerlab`.
The wheel contains the reusable client modules and declares PyAV, NumPy, Pillow
and video-writing dependencies. Pip downloads those dependencies separately;
this is not an offline dependency bundle. Example scripts, controllers, robot
assets and policy weights remain separate. The wheel contains Python code and
is platform-independent; dependency wheels remain platform-specific.

To build a wheel from the repository root:

```bash
uv build --wheel --out-dir dist py_example
```

## Source checkout setup (developers)

The following setup is for developing the client and running repository examples.
Users of the shipped AppImage should install the wheel as described above.

```bash
cd py_example
uv sync
```

The default environment uses Python 3.12 and does not install PyTorch. Choose
exactly one PyTorch backend when running a walking-policy or vision example:

```bash
uv sync --extra torch_cpu
uv sync --extra torch_rocm
uv sync --extra torch_cuda
```

The extras are mutually exclusive. The ROCm and CUDA builds are supported on
Linux; `torch_rocm` currently selects ROCm 7.2.4 (torch + torchvision from the
ROCm index) and `torch_cuda` selects CUDA 13.0. Vision examples also need
`--extra vision` (onnxruntime + ultralytics).

## RobotClient — motor commands, state, and camera

```bash
uv run python -c "
from ursoccerlab import RobotClient
client = RobotClient('127.0.0.1', 10000)
client.send_command({'head_pitch_joint_servo': 0.1})
for kind, data in client.recv():
    print(kind, type(data))
"
```

- Motor commands: JSON dict of `{actuator_name: float}`
- State: JSON with `sim_time`, `base`, `joints`, `actuators`, `cameras`
- RGB: versioned binary image sets with AV1/H.264/H.265/JPEG/raw RGB
- Depth: independent versioned messages with float32 metres or
  raw/zlib-compressed uint16 millimetres

Commands, state, RGB, and depth share this one bidirectional TCP connection.
Their rates are independent: the example scenes publish state at 60 Hz and
stereo AV1 RGB at 30 Hz. The package decodes AV1/H.264/H.265 with PyAV and returns separate
left/right RGB images. RGBD keeps independent lossless depth messages.
New connections receive state immediately and wait for a periodic video keyframe;
connections never request or force keyframes. See [AV1 runtime](../docs/AV1_Runtime.md). Worker threads encode images, then hand completed frames through bounded
mailboxes to the dedicated network worker, which owns socket I/O.

## Controller Parameters — actuator mode and PD gains

Actuator behavior is package-specific. The Pi Plus and MOS9 control examples
use the torque-motor interface. By default the
simulator starts in **torque** mode: each command value is the applied torque
in N·m. To use **position** mode (the value is a target angle in radians and
the simulator runs a PD law internally), call `set_controller_params`:

```python
from ursoccerlab import RobotClient, PI_PLUS

client = RobotClient('127.0.0.1', 10000)
client.set_controller_params(**PI_PLUS, actuator_mode="position")
client.send_command({'l_hip_pitch_joint_servo': 0.15})
```

The PD law per actuator is:

```
torque = kp * (target − qpos) − (kv + damping) * qvel
```

| Parameter | Type | Keys | Effect |
|-----------|------|------|--------|
| `kp` | `dict[str, float]` | actuator name → gain | Proportional (position) gain. Only used in position mode. |
| `kv` | `dict[str, float]` | actuator name → gain | Derivative (velocity) gain. Only used in position mode. |
| `damping` | `dict[str, float]` | actuator name → gain | Extra velocity damping. Applied in **both** modes. |
| `actuator_mode` | `str` | `"position"` or `"torque"` | Selects whether commands are position targets or raw torques. |

All arguments are optional — only the ones provided are updated; existing
values for omitted arguments persist. Settings are **sticky**: once set, they
remain in effect until changed again.

Gain presets that reproduce the original MJCF `<position>` behaviour:

```python
from ursoccerlab import PI_PLUS   # 22-DOF Unitree Pi
from ursoccerlab import MOS9      # 20-DOF MOS9
```

For multi-robot scenes with mixed types, `detect_gains` inspects the actuator
names from a received state and returns the matching preset:

```python
from ursoccerlab.gains import detect_gains
gains = detect_gains(list(state["actuators"].keys()))
client.set_controller_params(**gains, actuator_mode="position")
```

## AdminClient — set_pose, reset, lock_pose

```python
from ursoccerlab import AdminClient
admin = AdminClient('127.0.0.1', 11000)
admin.set_pose('robot_rp0', translation_m=[0.5, 0.0, 0.3762])
admin.reset('robot_rp0')
admin.close()
```

## Run A Scene

Each example lives in its own folder under `examples/<name>/` together with the
scene it expects as `scene.json`. The simulator is started in one terminal; the
Python client runs in a second terminal against the TCP ports it opens.

### Option A — AppImage (recommended, no Unreal Engine needed)

```bash
# Terminal 1 — start the simulator (headless, offscreen)
./dist/URSoccerLab.AppImage \
  py_example/examples/standing/scene.json
```

The scene JSON path is **required** — the AppImage exits with an error if it is
not provided. All other runtime flags (`-RenderOffscreen`, `-NoSound`, camera
readback config) are baked into the AppRun script. Writable data (logs, crash
reports) goes to `~/.local/share/URSoccerLab/`.

The AppImage needs a Vulkan-capable GPU and FUSE support on the host. If FUSE
is unavailable, use `APPIMAGE_EXTRACT_AND_RUN=1` before the launch command.

### Option B — From source (development, needs Unreal Editor)

```bash
export URS_UE="$HOME/software/Unreal_Engine_5.7.4/Engine/Binaries/Linux/UnrealEditor"

uv run --project py_example python Tools/runtime/run_scene.py \
  --scene-config py_example/examples/move_head/scene.json
```

This launches Unreal Editor in standalone `-game` mode with nDisplay atlas backend.
Use this path when iterating on C++ or asset changes.

### Port mapping

| Robot | TCP port |
|-------|----------|
| `robot_rp0` | 10000 |
| `robot_rp1` | 10001 |
| admin (set_pose, reset) | 11000 |

Each per-robot port is a single bidirectional TCP connection: commands go in,
state + camera frames come out (state at 60 Hz, AV1 RGB at 30 Hz in the supplied scenes).

### Running a client

In a second terminal, from `py_example/`:

```bash
uv run python examples/standing/standing.py --port 10000 10001 --duration 5
```

All examples auto-detect the robot type from the first state message and call
`set_controller_params` with the appropriate PD gains before sending any motor
commands (see [Controller Parameters](#controller-parameters--actuator-mode-and-pd-gains)
above). Output videos go under `artifacts/outputs/` (gitignored).


## 1. Head Motion

Two standing robots face one another. Both heads sweep while the legs remain
uncommanded. Both left-eye videos are recorded.

**Start the simulator** (terminal 1):

```bash
# AppImage
./dist/URSoccerLab.AppImage \
  py_example/examples/move_head/scene.json

# or from source
uv run --project py_example python Tools/runtime/run_scene.py \
  --scene-config py_example/examples/move_head/scene.json
```

**Run the client** (terminal 2):

```bash
cd py_example
uv run python examples/move_head/move_head.py \
  --port 10000 10001 --duration 10 \
  --video ../artifacts/outputs/head_motion
```

Scene: `examples/move_head/scene.json` (`two_robots_face_to_face`, pi_plus).
For a mos9 variant, use `Config/examples/mos9_face_to_face.json` as the scene
config when starting the simulator.

## 2. Standing

The same scene, but neither robot receives a head command. All actuators are
held at 0 (static capture):

**Start**:

```bash
./dist/URSoccerLab.AppImage \
  py_example/examples/standing/scene.json
```

**Client**:

```bash
cd py_example
uv run python examples/standing/standing.py \
  --port 10000 10001 --duration 5 \
  --video ../artifacts/outputs/standing
```

## 3. MOS9 Walking

Run the MOS9 AMP walking policy (ONNX, walk_v11_terrain). The walker follows
the policy while an observer robot stands still and records the walk from its
left-eye camera.

**Start**:

```bash
./dist/URSoccerLab.AppImage \
  py_example/examples/mos9_walk/scene.json
```

**Client**:

```bash
cd py_example
uv run --extra vision python examples/mos9_walk/mos9_walk.py \
  --robot-port 10000 --observer-port 10001 --vx 0.4 --duration 15 \
  --video ../artifacts/outputs/mos9_walker.mp4 --observer-video ../artifacts/outputs/mos9_observer.mp4
```

Install ONNX Runtime with `uv sync --extra vision`.
Requires `py_example/models/policies/mos9_walk_v11_5500.onnx` (vendored via
Git LFS). For solo walking (no observer), pass `--observer-port 0` and use
`Config/examples/mos9_solo.json` as the scene config.

## 4. Pi Plus Walking

`robot_rp0` starts at `(-1, 0)` and walks along `+X`. `robot_rp1` stands at
`(0, 3)` facing the walker. The policy sends motor commands only to `robot_rp0`
and records both left-eye cameras:

**Start**:

```bash
./dist/URSoccerLab.AppImage \
  py_example/examples/pi_walk/scene.json
```

**Client** (requires `torch_rocm` or `torch_cuda` extra):

```bash
cd py_example
uv sync --extra vision --extra torch_rocm
uv run --extra vision --extra torch_rocm python examples/pi_walk/pi_walk.py \
  --vx 0.35 --duration 15 \
  --video ../artifacts/outputs/walker.mp4 \
  --observer-video ../artifacts/outputs/observer.mp4
```

Requires `py_example/models/policies/pi_plus_model_40000.pt` (vendored via
Git LFS). The policy was trained against the older mos-brain Pi model — useful
for exercising the TCP motor and camera path, but not a validated gait for the
current Pi MJCF.

### Look at the ball and dribble

The vision-control example centers the ball with the head, then runs the
walking policy while continuously tracking the ball from the left eye. It uses
lateral velocity to remove horizontal ball displacement and reserves a
closed-loop PID for world-yaw control. The PID consumes the simulated IMU
orientation and angular velocity and defaults to a zero-radian heading. Camera
inference runs on a latest-frame-only worker, independent of the 50 Hz policy
loop. The detector is the Ultralytics COCO `yolo26s.pt` checkpoint (class 32 =
sports ball); resize/NMS are handled by Ultralytics, defaulting to the ROCm GPU:

**Start**:

```bash
./dist/URSoccerLab.AppImage \
  py_example/examples/dribble/scene.json
```

**Client** (requires `vision` + `torch_rocm` extras):

```bash
cd py_example
uv sync --extra vision --extra torch_rocm
uv run --extra vision --extra torch_rocm \
  python examples/dribble/dribble.py \
  --ultralytics-device 0 --duration 10
```

At startup the example resets `robot_rp0` and the ball through the admin
endpoint; pass `--no-reset-at-start` to preserve the live poses. Output (raw
and annotated videos, observer video, JSON detection trace) goes under
`../artifacts/outputs/dribble/`.

## Layout

```text
src/ursoccerlab/                       reusable TCP, camera, and video APIs
examples/move_head/                    head-sweep capture (scene.json)
examples/standing/                     static standing capture (scene.json)
examples/mos9_walk/                    MOS9 AMP walk policy (scene.json)
examples/pi_walk/                      Pi Plus walk policy (scene.json)
examples/dribble/                      look-at-ball + dribble (policy.py + scene.json)
Config/examples/                       alternative/general scene configs
tests/                                 protocol and camera parser tests
../artifacts/outputs/                   ignored local captures
```

Run the unit tests without installing another test framework:

```bash
uv run python -m unittest discover -s tests
```

## Protocol

Frame format: `[4-byte BE length][1-byte type][payload]`

- Type `0x00`: JSON (state, command, admin)
- Type `0x01`: RGB image set
- Type `0x02`: Depth image set

RGB/depth v2 layout:

```
[u8 version=2][u8 image_count][u16 flags]
[u32 sequence][f64 sim_time]
then, per image:
  [u8 name_len][UTF-8 camera_name]
  [u8 codec][u8 pixel_format][u8 image_flags]
  [u16 width][u16 height]
  [u32 uncompressed_len][u32 data_len][data]
```

Codec: `0x00` raw, `0x01` JPEG, `0x02` zlib, `0x03` AV1, `0x04` H.264, `0x05` H.265.
`image_flags` bit 0 marks video keyframes.
See [AV1 runtime](../docs/AV1_Runtime.md) for packed stereo and codec epochs. Pixel format: `0x00`
BGRA8, `0x01` float32 metres, `0x02` uint16 millimetres. Use
`camera_to_rgb()` and `depth_to_meters()` for decoded NumPy arrays.

## Inspector receiver

The packaged and source simulators open the shared inspector TCP port `12000`
when `guest_inspector.enabled` is true. See the
[inspector architecture](../docs/Guest_Cameras.md) for the v1 contract.
Each guest controls only its own floating camera and receives compressed RGB.

```sh
uv run python examples/inspector/receive.py --position -4 0 2 \
  --quaternion 0 0 0 1 --duration 10 --video ../artifacts/outputs/inspector.mp4
```

The reusable `InspectorClient` provides `set_camera(translation_m,
rotation_quat_xyzw)`, `recv()`, and `close()`. Poses use MuJoCo world metres and
xyzw orientation, local +X forward and +Z up. The script writes video incrementally
and saves a first-frame PNG; `--fps` sets playback timing only.
