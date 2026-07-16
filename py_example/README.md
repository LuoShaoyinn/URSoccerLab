## URSoccerLab Zero-Command Vision Smoke Test

This client waits for URSoccerLab ZMQ metadata, sends an all-zero motor command,
saves one proprioception/state packet, and saves one camera frame when a camera is
advertised.

Run from the project root:

```bash
cd py_example
uv sync
uv run python main.py --host 127.0.0.1 --robot robot_rp0 --out out
```

Outputs:

- `out/meta.json`
- `out/state.json`
- `out/camera.png` if a BGRA8 URLab camera is active

If no camera is advertised, the script still tests command/state and prints a
clear `camera_note`.

Manual camera override:

```bash
uv run python main.py \
  --camera-endpoint tcp://127.0.0.1:5558 \
  --camera-topic robot_rp0/camera/Camera \
  --camera-width 640 \
  --camera-height 480
```
