# mos9_walk

Run the MOS9 AMP walking policy (ONNX, `walk_v11_terrain`) while recording both
robots' left-eye cameras. The walker follows the policy; a second robot stands
as a stationary observer.

## Prerequisites

- ONNX policy: `refs/MOS9-AMP/logs/rsl_rl/mos9_loco/walk_v11_terrain/exported/policy_5500.onnx`
- No PyTorch backend required (pure ONNX Runtime, included in the default env).

## Scene

`scene.json` — MOS9 walker (`robot_rp0`, port `10000`) + MOS9 observer
(`robot_rp1`, port `10001`).

## Run

Start the simulator offscreen in a separate terminal (project root):

```bash
uv run --project py_example python Tools/runtime/run_scene.py \
  --scene-config py_example/examples/mos9_walk/scene.json
```

Then run the client:

```bash
cd py_example
uv run python examples/mos9_walk/mos9_walk.py \
  --robot-port 10000 --observer-port 10001 \
  --vx 0.4 --duration 15 \
  --video out/mos9_walker.mp4 --observer-video out/mos9_observer.mp4
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

Walker and observer H.264 MP4s under `out/`.
