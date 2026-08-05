# dribble

Look at the ball with the left eye, then walk forward to dribble it. The
walking policy runs at 50 Hz; a latest-frame-only vision worker runs the ball
detector independently so camera traffic never blocks control.

The detector is the Ultralytics COCO `yolo26s.pt` checkpoint (class 32 =
sports ball, normalized to `ball`). Resize/NMS are handled by Ultralytics, so
no fixed-shape ONNX is required. Defaults to the ROCm GPU.

## Prerequisites

- Vision + a PyTorch backend: `uv sync --extra vision --extra torch_rocm`.
- Pi Plus policy checkpoint: `refs/mos-brain/.../pi_plus_model_40000.pt`
  (used by the local `policy.py`).
- Ultralytics `.pt` checkpoint: `py_example/models/yolo26s.pt` (tracked via
  git LFS; the `--ultralytics-pt` default points here).

## Scene

`scene.json` — `pi_plus` walker (`robot_rp0`, port `10000`) starts at
`(-1, 0)` facing `+X`; `pi_plus` observer (`robot_rp1`, port `10001`) at
`(0, 3)`; soccer ball at the origin. The example resets `robot_rp0` and the
ball through the admin endpoint before control.

## Run

Start the simulator offscreen in a separate terminal (project root):

```bash
uv run --project py_example python Tools/runtime/run_scene.py \
  --scene-config py_example/examples/dribble/scene.json
```

Then run the client (needs vision + a PyTorch backend):

```bash
cd py_example
uv sync --extra vision --extra torch_rocm
uv run --extra vision --extra torch_rocm \
  python examples/dribble/dribble.py --ultralytics-device 0 --duration 10
```

## Phases

1. **reset** — admin `reset(robot_rp0)`, `reset(ball)`.
2. **seek** — hold stance, center the ball with head yaw/pitch until locked.
3. **dribble** — run the walking policy; lateral velocity corrects horizontal
   ball error, a PID controls world yaw. A fall guard (base z / up-vector)
   stops early.

## Options (selected)

| flag | default | notes |
|------|---------|-------|
| `--ultralytics-pt` | `py_example/models/yolo26s.pt` | Ultralytics checkpoint |
| `--ultralytics-device` | `0` | ROCm GPU id, or `cpu` |
| `--ultralytics-conf` | `0.15` | detection confidence |
| `--duration` | `1` | dribble length in seconds |
| `--vx` | `0.3` | forward velocity (m/s) |
| `--yaw-target` | `0` | desired world yaw (rad) |
| `--seek-timeout` | `8` | seconds to center the ball before failing |

## Output

`out/dribble/{left_eye,detections,observer}.mp4` and `trace.json`.

## Self-contained

This folder copies the Pi Plus policy helpers into `policy.py` so it has no
dependency on the `pi_walk` example.
