# mos9_walk

Run the MOS9 AMP walking policy (ONNX, `walk_v11_terrain`) while recording the robot cameras. The walker follows the policy; a second robot can stand as an observer.

## Prerequisites

- ONNX policy: `models/policies/mos9_walk_v11_5500.onnx`
- Install this example’s dependencies and the distributed Python client wheel; no PyTorch backend is required.

## Scene

`scene.json` — MOS9 walker (`robot_rp0`, port `10000`) + MOS9 observer
(`robot_rp1`, port `10001`).

## Run

Supply the external robot packages and field maps referenced by `scene.json`.
Start the packaged simulator from the project root:

```bash
./dist/URSoccerLab.AppImage walk/mos9_walk/scene.json
```

Then run the client:

```bash
python walk/mos9_walk/mos9_walk.py \
  --robot-port 10000 --observer-port 10001 \
  --vx 0.4 --duration 15 \
  --video artifacts/outputs/mos9_walker.mp4 --observer-video artifacts/outputs/mos9_observer.mp4
```

For solo walking (no observer): `--observer-port 0` and launch with
a scene containing a single MOS9.

## Options

| flag | default | notes |
|------|---------|-------|
| `--vx` | `0.4` | forward velocity command (m/s) |
| `--vy` / `--wz` | `0` | lateral / yaw-rate command |
| `--duration` | `15` | walk length in seconds |
| `--policy-hz` | `50` | policy inference rate |
| `--base-height` | `0.56` | spawn z used by the admin pose lock |

## Output

Walker and observer H.264 MP4s under `artifacts/outputs/`.

The controller resets and locks the robot upright through the admin port before releasing it. `--actor` must match the scene actor; the default is `robot_rp0`. It requires the admin port (11000 by default).

On the A843 simulator with 60 Hz state updates, a 12-second trial at `--vx 0.3` stayed upright but drifted laterally. At 50 Hz, a 12-second trial with the other robot moved away stayed upright and moved about 3.1 m forward, with substantial lateral drift. An earlier trial fell with the other robot nearby; collision was not directly measured. This policy is not yet reliable straight-line navigation.
