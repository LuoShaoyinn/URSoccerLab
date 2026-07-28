# URSoccerLab Python Examples

TCP-based clients for the URSoccerLab robot control API.

## Setup

```bash
cd py_example
uv sync
```

## RobotClient — motor commands, state, and camera

```bash
uv run python -c "
from common.tcp import RobotClient
client = RobotClient('127.0.0.1', 10000)
client.send_command({'head_pitch_joint': 0.1})
for kind, data in client.recv():
    print(kind, type(data))
"
```

- Motor commands: JSON dict of `{actuator_name: float}`
- State: JSON with `sim_time`, `base`, `joints`, `actuators`, `cameras`
- Camera: packed binary with `sim_time`, width, height, JPEG/raw pixels

## AdminClient — set_pose, reset, lock_pose

```python
from common.tcp import AdminClient
admin = AdminClient('127.0.0.1', 11000)
admin.set_pose('robot_rp0', translation_m=[0.5, 0.0, 0.3762])
admin.reset('robot_rp0')
admin.close()
```

## Head Demo (two robots facing each other)

```bash
uv run python demos/head_motion.py
```

Start the simulator with `Config/URS_two_robot_scene.json` and `/Game/Levels/URS_SoccerField`.

## Walking Camera Video

`demos/walk_policy.py` keeps the mos-brain locomotion policy outside Unreal.
It drives only the TCP motor-command API and records the left-eye camera:

```bash
source py_example/.venv-walk/bin/activate
uv run --no-project --active python py_example/demos/walk_policy.py \
  --vx 0.35 --duration 8 --video py_example/out/walk_rp0.mp4
```

It requires `refs/mos-brain/simulation/mujoco/assets/policies/pi_plus_model_40000.pt`.
The policy was trained against the older mos-brain Pi model. It is useful for
exercising the TCP motor and camera path, but is not a validated gait for the
current Pi MJCF until its dynamics and actuator calibration are matched or the
policy is retrained.

## Vision Smoke Test

From the project root:

```bash
uv run --project py_example python Tools/runtime/run_vision_smoke_test.py
```

Starts the simulator, connects via TCP, sends zero commands, captures camera
frames, and validates non-blank RGB content.

## Protocol

Frame format: `[4-byte BE length][1-byte type][payload]`

- Type `0x00`: JSON (state, command, admin)
- Type `0x01`: Camera (binary packed)

Camera layout:
```
[1-byte codec][1-byte num_cams][8-byte LE double sim_time]
per-cam: [width LE16][height LE16][data_len LE32][pixel data]
```

Codec: `0x00` = raw BGRA, `0x01` = JPEG.
