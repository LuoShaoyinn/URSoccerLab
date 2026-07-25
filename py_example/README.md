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
  --motion-amplitude 1.0 \
  --motion-duration-sec 3.0 \
  --camera-frame-count 20
```

The project-owned `pi_plus` camera fixture unlocks `head_yaw_joint` and
`head_pitch_joint`, so `--motion-regex head` targets those motors directly:

The matching fixed robot asset is
`Assets/MosBrainCameraTest/pi_plus/pi_plus_stereo_camera.xml`.

```bash
uv run python main.py \
  --host 127.0.0.1 \
  --robot robot_rp0 \
  --out out/pi_motion \
  --motion-regex head \
  --motion-amplitude 1.0 \
  --motion-duration-sec 3.0 \
  --camera-frame-count 20
```

Motion runs save `camera_before.png`, `camera_after.png`, and keep
`camera.png` as the after frame. By default `--motion-frequency-hz 0` sends a
constant command; set a positive frequency to send a sine wave.

Manual camera override:

```bash
uv run python main.py \
  --camera-endpoint tcp://127.0.0.1:5558 \
  --camera-topic robot_rp0/camera/left_eye \
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
  --require-nonzero-command \
  --out py_example/out/motor_vision_smoke
```

Reusable UE soccer-field scene:

```bash
uv run python Tools/create_soccer_field_scene.py --nullrhi
```

That command creates `/Game/Levels/URS_SoccerField` as a saved UE scene asset.
`Tools/ue_bake_soccer_field_scene.py` contains the scene recipe: it imports the
GLB field visuals, converts Blender/glTF Y-up node transforms into the UE level
frame, and adds a default UE skylight. Runtime simulator code should load this
map and only add dynamic robots/cameras/control.

## Pure-Python Bipedal Walking Example (`walk_pi_plus.py`)

The simulator ships no gait or locomotion model. This standalone example drives
the mos-brain `pi_plus_model_40000.pt` locomotion policy against the Pi Plus
MuJoCo model using a self-contained Python pipeline (observation assembly, MLP
policy inference, PD position control). Use it as a reference for writing your
own decider or motion client.

It needs a separate environment from `main.py` (mujoco + torch + imageio):

```bash
uv venv /tmp/walk-venv --python 3.12 --clear
source /tmp/walk-venv/bin/activate
uv pip install "mujoco" "torch" "numpy<2.3" "imageio" "imageio-ffmpeg" "pillow"
```

Walk forward for 6 seconds and save an offscreen video plus a trajectory dump:

```bash
python py_example/walk_pi_plus.py \
  --duration 6.0 --vx 0.5 \
  --video py_example/out/walk_forward.mp4 \
  --trajectory py_example/out/walk_forward.npz
```

Other useful invocations:

```bash
# turn in place (forward + yaw rate)
python py_example/walk_pi_plus.py --vx 0.3 --vtheta 0.6 --video out/walk_turn.mp4

# balance in place (zero command)
python py_example/walk_pi_plus.py --vx 0.0 --duration 10.0 --video out/walk_balance.mp4
```

The script prints forward displacement, average speed, minimum upright value,
and whether the robot stayed upright (`min upright > 0.5`). The trajectory npz
contains `t, x, y, z, upright, qpos` for downstream plotting/analysis.

### Validating against the mos-brain origin pipeline

`compare_with_mos_brain.py` drives the **actual** `MultiRobotMujocoSim` from
`refs/mos-brain` for a single Pi Plus and compares its walking trajectory
against `walk_pi_plus.py`:

```bash
python py_example/compare_with_mos_brain.py --duration 5.0 --vx 0.5
```

Both runs use the same velocity command. The reference robot stands on the
mos-brain soccer pitch while the example uses the standalone `pi_plus.xml`
floor, so absolute positions differ slightly, but the macro walking behaviour
(forward speed, stability, gait) should match within a few percent. A typical
result for `--vx 0.5` over 5 s:

```
mos-brain reference : +2.31 m forward, 0.46 m/s, min upright 0.98
pure-python example : +2.42 m forward, 0.48 m/s, min upright 0.98
=> MATCH (within tolerance)
```

### How the pipeline mirrors mos-brain

All constants (joint order, PD gains, action scale, default pose, observation
history length, command clip) are copied from
`refs/mos-brain/.../app/multi_robot_sim.py` and `app/runtime_config.py`. The
per-control-step loop is:

1. **Observation** (69 dims): IMU angular velocity, projected gravity, clipped
   `[vx, vy, vtheta]` command, joint positions (relative to default, reordered
   MuJoCo→policy), joint velocities, and last action.
2. **History**: the 69-dim vector is appended to a rolling buffer of length 5
   (345-dim), clipped to `[-100, 100]`, and fed to the JIT/MLP policy.
3. **Action**: the 20-dim policy output is scaled by `0.25`, reordered
   policy→MuJoCo, and added to the default pose to form PD targets.
4. **Control**: every physics step applies `tau = kp*(target - q) - kd*qd`,
   clipped to `[-20, 20]` N·m, then `mj_step` runs at `dt = 0.002` s. The policy
   runs every 10 steps (50 Hz).
