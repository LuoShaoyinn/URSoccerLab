## URSoccerLab Zero-Command Vision Smoke Test

This client waits for URSoccerLab ZMQ metadata, sends a motor command stream,
saves one proprioception/state packet, and saves one camera frame when a camera is
advertised. With no motion arguments it sends an all-zero motor command.

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

Move named head motors while capturing camera output:

```bash
uv run python main.py \
  --host 127.0.0.1 \
  --robot robot_rp0 \
  --out out/head_motion \
  --motion-regex head \
  --motion-duration-sec 0.75 \
  --camera-frame-count 20
```

The project-owned `pi_plus` camera fixture unlocks `head_yaw_joint` and
`head_pitch_joint`, so `--motion-regex head` targets those motors directly:
The matching tuned URDF is
`Assets/MosBrainCameraTest/pi_plus/pi_plus_head_camera.urdf`.

```bash
uv run python main.py \
  --host 127.0.0.1 \
  --robot robot_rp0 \
  --out out/pi_motion \
  --motion-regex head \
  --motion-duration-sec 0.75 \
  --camera-frame-count 20
```

Manual camera override:

```bash
uv run python main.py \
  --camera-endpoint tcp://127.0.0.1:5558 \
  --camera-topic robot_rp0/camera/Camera \
  --camera-width 640 \
  --camera-height 480
```

End-to-end simulator smoke test from the project root:

```bash
uv run python Tools/run_vision_smoke_test.py
```

That command prepares `/Game/Levels/URS_SoccerField`, starts it offscreen, runs
this client, drains multiple camera frames, and fails unless
`py_example/out/vision_smoke/camera.png` has nonblank RGB content.

End-to-end motor plus vision smoke test from the project root:

```bash
uv run python Tools/run_vision_smoke_test.py \
  --motion-regex head \
  --motion-duration-sec 0.75 \
  --require-nonzero-command \
  --out py_example/out/motor_vision_smoke
```

Reusable UE soccer-field scene:

```bash
uv run python Tools/create_soccer_field_scene.py
```

That command creates `/Game/Levels/URS_SoccerField` as a saved UE scene asset.
It contains the imported GLB field visuals and a default UE skylight. Runtime
simulator code should load this map and only add dynamic robots/cameras/control.
