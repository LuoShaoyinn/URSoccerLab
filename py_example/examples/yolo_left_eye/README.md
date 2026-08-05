# yolo_left_eye

Run a trained YOLO26 checkpoint on the robot's left-eye camera (index 0). With
no `--image`, it connects to a running robot and grabs one live frame; with
`--image`, it runs repeatable inference on a saved capture.

## Prerequisites

- Vision extra: `uv sync --extra vision` (ONNX Runtime).
- Model: `refs/vision/models/yolo26/yolo26s_best.onnx` (soccer-trained, 7
  classes: ball, goalpost, robot, L/T/X-intersection, obstacle). Use
  `--model ../refs/vision/models/yolo26/yolo26n_best.onnx` for the nano model.

## Scene

`scene.json` — `pi_plus` walker + observer (same as `pi_walk`); only one robot
port is needed for capture.

## Run

Live capture:

```bash
cd py_example
uv run --extra vision python examples/yolo_left_eye/yolo_left_eye.py \
  --port 10000 --out out/yolo_left_eye
```

Saved frame:

```bash
uv run --extra vision python examples/yolo_left_eye/yolo_left_eye.py \
  --image out/yolo/camera.png --out out/yolo_saved_frame
```

## Options

| flag | default | notes |
|------|---------|-------|
| `--model` | `refs/.../yolo26s_best.onnx` | ONNX checkpoint |
| `--confidence` | `0.4` | detection confidence threshold |
| `--iou` | `0.45` | NMS IoU threshold |
| `--port` | `10000` | robot TCP port (ignored with `--image`) |

## Output

`left_eye.png`, `annotated.png`, and `detections.json` under the `--out` dir.
