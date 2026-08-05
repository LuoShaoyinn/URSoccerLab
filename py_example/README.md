# URSoccerLab Python Examples

TCP-based clients for the URSoccerLab robot control API.

## Setup

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
- RGB: versioned binary image sets with JPEG/raw BGRA pixels
- Depth: independent versioned messages with float32 metres or
  raw/zlib-compressed uint16 millimetres

Commands, state, RGB, and depth share this one bidirectional TCP connection.
Their rates are independent: the default publishes state at 60 Hz and two
JPEG-compressed RGB cameras at 30 Hz. Worker threads encode images, then queue
completed frames back to the game thread; only the game thread writes the
socket.

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
scene it expects as `scene.json`. Always launch the runtime through the
production nDisplay atlas backend (`Tools/runtime/run_scene.py`); it generates
the nDisplay viewport config from the scene's robot/camera count and starts a
single-node cluster session, so the `URSDisplayClusterCameraBinderComponent`
owns camera capture. Do **not** launch a plain `-game` session — that falls
back to the per-actor MjCamera readback path, which is not supported for the
examples.

From the project root:

```bash
uv run --project py_example python Tools/runtime/run_scene.py \
  --scene-config py_example/examples/move_head/scene.json
```

This is a foreground process (the editor window). In a second terminal, run the
client from `py_example/`. Each client uses TCP only. `robot_rp0` is port
`10000`; `robot_rp1` is port `10001`. Port `11000` is one optional global
administration connection, not a
second per-robot stream. Output files go under `py_example/out/` and are ignored
by Git. Each example folder is self-contained (no cross-imports between
examples).

## 1. Head Motion

Two standing robots face one another. Both heads sweep while the legs remain
uncommanded. Both left-eye videos are recorded.

```bash
uv run python examples/move_head/move_head.py \
  --port 10000 10001 --duration 10 \
  --video out/head_motion
```

Scene: `examples/move_head/scene.json` (`two_robots_face_to_face`, pi_plus).
For a mos9 variant, point the runtime at `Config/examples/mos9_face_to_face.json`.

For a single robot:

```bash
uv run python examples/move_head/move_head.py \
  --port 10000 --duration 10 \
  --video out/head_solo
```

## 2. Standing

The same scene, but neither robot receives a head command. All actuators are
held at 0 (static capture):

```bash
uv run python examples/standing/standing.py \
  --port 10000 10001 --duration 5 \
  --video out/standing
```

Scene: `examples/standing/scene.json`.

## 3. MOS9 Walking

Run the MOS9 AMP walking policy (ONNX, walk_v11_terrain). The walker follows
the policy while an observer robot stands still and records the walk from its
left-eye camera.

```bash
uv run python examples/mos9_walk/mos9_walk.py \
  --robot-port 10000 --observer-port 10001 --vx 0.4 --duration 15 \
  --video out/mos9_walker.mp4 --observer-video out/mos9_observer.mp4
```

Scene: `examples/mos9_walk/scene.json`. Requires
`refs/MOS9-AMP/logs/rsl_rl/mos9_loco/walk_v11_terrain/exported/policy_5500.onnx`.

For solo walking (no observer):

```bash
uv run python examples/mos9_walk/mos9_walk.py \
  --robot-port 10000 --observer-port 0 --vx 0.4 --duration 15 \
  --video out/mos9_walker.mp4
```

Use `Config/examples/mos9_solo.json` in the runtime command for solo mode.

## 4. Pi Plus Walking

`robot_rp0` starts at `(-1, 0)` and walks along `+X`. `robot_rp1` stands at
`(0, 3)` facing the walker. The policy sends motor commands only to `robot_rp0`
and records both left-eye cameras:

```bash
uv sync --extra vision --extra torch_rocm
uv run --extra vision --extra torch_rocm python examples/pi_walk/pi_walk.py \
  --vx 0.35 --duration 15 \
  --video out/walker.mp4 \
  --observer-video out/observer.mp4
```

Scene: `examples/pi_walk/scene.json`. It requires
`refs/mos-brain/simulation/mujoco/assets/policies/pi_plus_model_40000.pt`.
The policy was trained against the older mos-brain Pi model. It is useful for
exercising the TCP motor and camera path, but is not a validated gait for the
current Pi MJCF until its dynamics and actuator calibration are matched or the
policy is retrained.

## YOLO Left-Eye Inference

Install the optional vision extra, start a scene, and run the trained YOLO26
checkpoint on camera index 0 (the robot's left eye):

```bash
uv sync --extra vision
uv run --extra vision python examples/yolo_left_eye/yolo_left_eye.py \
  --port 10000 --out out/yolo_left_eye
```

For repeatable inference on an existing capture:

```bash
uv run --extra vision python examples/yolo_left_eye/yolo_left_eye.py \
  --image out/yolo/camera.png --out out/yolo_saved_frame
```

Scene: `examples/yolo_left_eye/scene.json`. The example defaults to
`refs/vision/models/yolo26/yolo26s_best.onnx` and writes `left_eye.png`,
`annotated.png`, and `detections.json`. Use
`--model ../refs/vision/models/yolo26/yolo26n_best.onnx` for the nano model.
This inspection example uses ONNX Runtime on CPU; the deployment code under
`refs/vision` converts the same checkpoint to TensorRT.

### Look at the ball and dribble

The vision-control example centers the ball with the head, then runs the
walking policy while continuously tracking the ball from the left eye. It uses
lateral velocity to remove horizontal ball displacement and reserves a
closed-loop PID for world-yaw control. The PID consumes the simulated IMU
orientation and angular velocity and defaults to a zero-radian heading. Camera
inference runs on a latest-frame-only worker, independent of the 50 Hz policy
loop. The detector is the Ultralytics COCO `yolo26s.pt` checkpoint (class 32 =
sports ball); resize/NMS are handled by Ultralytics, defaulting to the ROCm GPU:

```bash
uv sync --extra vision --extra torch_rocm
uv run --extra vision --extra torch_rocm \
  python examples/dribble/dribble.py \
  --ultralytics-device 0 --duration 10
```

Scene: `examples/dribble/scene.json`. At startup the example resets `robot_rp0`
and the ball through the admin endpoint so warm-up cannot leave stale scene
state; pass `--no-reset-at-start` to preserve the live poses. The example
writes raw and annotated videos plus an external observer video and JSON
detection/control trace under `out/dribble/`.

## Layout

```text
src/ursoccerlab/                       reusable TCP, camera, and video APIs
examples/move_head/                    head-sweep capture (scene.json)
examples/standing/                     static standing capture (scene.json)
examples/mos9_walk/                    MOS9 AMP walk policy (scene.json)
examples/pi_walk/                      Pi Plus walk policy (scene.json)
examples/yolo_left_eye/                YOLO26 inference inspection (scene.json)
examples/dribble/                      look-at-ball + dribble (policy.py + scene.json)
Config/examples/                       alternative/general scene configs
tests/                                 protocol and camera parser tests
out/                                   ignored local captures
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
  [u8 codec][u8 pixel_format][u8 reserved]
  [u16 width][u16 height]
  [u32 uncompressed_len][u32 data_len][data]
```

Codec: `0x00` raw, `0x01` JPEG, `0x02` zlib. Pixel format: `0x00`
BGRA8, `0x01` float32 metres, `0x02` uint16 millimetres. Use
`camera_to_rgb()` and `depth_to_meters()` for decoded NumPy arrays.
