# pi_walk

Run one Pi Plus walking policy while recording both robots' left-eye cameras.
`robot_rp0` starts at `(-1, 0)` and walks along `+X`; `robot_rp1` stands at
`(0, 3)` facing the walker.

## Prerequisites

- PyTorch backend extra (pick one): `install the required ML dependencies separately` (ROCm),
  `--extra torch_cpu`, or `--extra torch_cuda`.
- Policy checkpoint: `py_example/models/policies/pi_plus_model_40000.pt`

> The policy was trained against the older mos-brain Pi dynamics. It exercises
> the runtime protocol and capture path, but is **not a validated gait** for
> the current Pi MJCF until the dynamics/actuator calibration is matched or the
> policy is retrained.

## Scene

`scene.json` — `pi_plus` walker (`robot_rp0`, port `10000`) + `pi_plus`
observer (`robot_rp1`, port `10001`).

## Run

Supply the external robot packages and field maps referenced by `scene.json`.
Start the packaged simulator from the project root:

```bash
./dist/URSoccerLab.AppImage py_example/examples/pi_walk/scene.json
```

Then run the client (needs a PyTorch backend extra):

```bash
cd py_example
# Install the required ML dependencies separately (see py_example/README.md).
python examples/pi_walk/pi_walk.py \
  --vx 0.35 --duration 15 \
  --video ../artifacts/outputs/walker.mp4 --observer-video ../artifacts/outputs/observer.mp4
```

## Options

| flag | default | notes |
|------|---------|-------|
| `--vx` | `0.35` | forward velocity command (m/s) |
| `--duration` | `15` | walk length in seconds |
| `--policy-hz` | `50` | policy inference rate |
| `--policy` | `py_example/models/policies/pi_plus_model_40000.pt` | checkpoint path |

## Output

Walker and observer H.264 MP4s under `../artifacts/outputs/`.

## Shared code

This folder is the canonical home of the Pi Plus policy helpers (`Actor`,
`load_policy`, `observation`, joint maps). The `dribble` example ships its own
copy in `examples/dribble/policy.py` so it stays self-contained.
