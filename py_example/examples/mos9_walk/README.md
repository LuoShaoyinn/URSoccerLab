# mos9_walk

Run the MOS9 AMP walking policy (ONNX, `walk_v11_terrain`) while recording both
robots' left-eye cameras. The walker follows the policy; a second robot stands
as a stationary observer.

## Prerequisites

- ONNX policy: `py_example/models/policies/mos9_walk_v11_5500.onnx`
- Install ONNX Runtime with `install the required ML dependencies separately`; no PyTorch backend is required.

## Scene

`scene.json` — MOS9 walker (`robot_rp0`, port `10000`) + MOS9 observer
(`robot_rp1`, port `10001`).

## Run

Supply the external robot packages and field maps referenced by `scene.json`.
Start the packaged simulator from the project root:

```bash
./dist/URSoccerLab.AppImage py_example/examples/mos9_walk/scene.json
```

Then run the client:

```bash
cd py_example
python examples/mos9_walk/mos9_walk.py \
  --robot-port 10000 --observer-port 10001 \
  --vx 0.4 --duration 15 \
  --video ../artifacts/outputs/mos9_walker.mp4 --observer-video ../artifacts/outputs/mos9_observer.mp4
```

For solo walking (no observer): `--observer-port 0` and launch with
`Config/examples/mos9_solo.json`.

## Options

| flag | default | notes |
|------|---------|-------|
| `--vx` | `0.4` | forward velocity command (m/s) |
| `--vy` / `--wz` | `0` | lateral / yaw-rate command |
| `--duration` | `15` | walk length in seconds |
| `--policy-hz` | `50` | policy inference rate |
| `--base-height` | `0.56` | spawn z used by the admin `set_pose` reset |

## Output

Walker and observer H.264 MP4s under `../artifacts/outputs/`.
