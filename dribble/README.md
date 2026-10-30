# dribble

Look at the ball with the left eye, then walk forward to dribble it. The
walking policy runs at 50 Hz; a latest-frame-only vision worker runs the ball
detector independently so camera traffic never blocks control.

The detector is the Ultralytics COCO `yolo26s.pt` checkpoint (class 32 =
sports ball, normalized to `ball`). Resize/NMS are handled by Ultralytics, so
no fixed-shape ONNX is required. Defaults to the ROCm GPU.

## Prerequisites

- Vision + a PyTorch backend: `install the required ML dependencies separately`.
- Pi Plus policy checkpoint: `models/policies/pi_plus_model_40000.pt`
  (used by the local `policy.py`).
- Ultralytics `.pt` checkpoint: `models/yolo26s.pt` (tracked via
  git LFS; the `--ultralytics-pt` default points here).

## Scene

`scene.json` — `pi_plus` walker (`robot_rp0`, port `10000`) starts at
`(-1, 0)` facing `+X`; `pi_plus` observer (`robot_rp1`, port `10001`) at
`(0, 3)`; soccer ball at the origin. The example resets `robot_rp0` and the
ball through the admin endpoint before control.

## Run

Supply the external robot packages and field maps referenced by `scene.json`.
Start the packaged simulator from the project root:

```bash
./dist/URSoccerLab.AppImage dribble/scene.json
```

Then run the client (needs vision + a PyTorch backend):

```bash
# Install the required ML dependencies separately (see README.md).
python dribble/dribble.py --ultralytics-device 0 --duration 10
```

## MOS9 on the earlier grass pitch

The MOS9 scene contains only the dribbling robot; use `--observer-port 0`
to skip the observer connection. Use `scene_mos9.json` and `--robot-type mos9`
to select the MOS9 ONNX gait
(`models/policies/mos9_walk_v11_5500.onnx`; requires ONNX Runtime).
The original Pi Plus scene and controller default remain available.

This scene expects `external/field/grass_pitch.png`, a copy of the earlier
`refs/field_fixed.png`, plus the normal, roughness and AO maps in
`external/field/grass1-ue/`. All textures remain external and Git-ignored.
Restart the simulator when changing scenes or textures.

```bash
./dist/URSoccerLab.AppImage dribble/scene_mos9.json
python dribble/dribble.py --robot-type mos9 --observer-port 0 --ultralytics-device cpu --duration 12
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
| `--ultralytics-pt` | `models/yolo26s.pt` | Ultralytics checkpoint |
| `--ultralytics-device` | `0` | ROCm GPU id, or `cpu` |
| `--ultralytics-conf` | `0.15` | detection confidence |
| `--duration` | `1` | dribble length in seconds |
| `--vx` | `0.3` | forward velocity (m/s) |
| `--yaw-target` | `0` | desired world yaw (rad) |
| `--seek-timeout` | `8` | seconds to center the ball before failing |

## Output

`../artifacts/outputs/dribble/{left_eye,detections,observer}.mp4` and `trace.json`.

## Policy helpers

This folder copies the Pi Plus policy helpers into `policy.py` so it has no
dependency on the `pi_walk` example.
The optional MOS9 controller reuses gait constants and ONNX inference from
the `walk/mos9_walk` example.
