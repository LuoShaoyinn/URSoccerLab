# URSoccerLab ZMQ Runtime Validation

## Implemented

- Inbound command isolation: one `ZMQ_PULL` command socket per active robot.
- Default robot command ports: `robot_rp0..robot_rp6` on `10000..10006`, `robot_bp0..robot_bp6` on `10007..10013`.
- Shared state publisher: `tcp://0.0.0.0:10100`, topic `state/<robot>`.
- Shared metadata publisher: `tcp://0.0.0.0:10101`, topic `meta/<robot>`.
- Commands are drained in URLab `PreStep` callbacks and written to URLab actuator network controls before URLab applies controls.
- State is published from URLab `PostStep` callbacks at `StatePublishRateHz`.
- Fallback game-thread tick path remains available by setting `bUsePhysicsCallbacks=false`.
- Per-robot admin RPC: one `ZMQ_REP` socket per active robot on `tcp://0.0.0.0:<AdminBasePort+idx>` (default base `11000`, so `robot_rp0` is on `11000`). Ops:
  - `set_pose` — force-write root translation, root rotation (`rotation_quat_xyzw`), and non-root `joint_qpos` under `CallbackMutex` + `mj_forward`. Any field absent from the request is treated as zero (identity for rotation).
  - `reset` — return this robot to its initial spawn pose (looked up from `UURSSceneConfigComponent` when present) or all-zero qpos as a fallback.
- Scene config component (`UURSSceneConfigComponent`) reads `Config/URS_scene.json` on `BeginPlay` and spawns registered robot types. `pi_plus` is registered by the game module startup against `/Game/MuJoCoImports/pi_plus_stereo_camera`.

## Motor Command Payload

Binary little-endian:

```text
uint32 magic       0x4D535255
uint16 version     1
uint16 flags       0
uint64 sequence
double stamp_sec
uint32 num_motors
float32 motors[num_motors]
```

## Quick Client Check

Install `pyzmq` in the policy/client Python environment, run the simulator, then:

```bash
python Tools/zmq_smoke_client.py --robot robot_rp0
```

The script waits for `meta/robot_rp0`, sends one zero motor vector to that robot's command endpoint, then waits for one `state/robot_rp0` packet.
`py_example/main.py` also supports a time-varying motor command stream using
`--motion-regex`, `--motion-duration-sec`, and related options.

## Admin RPC Quick Check

The admin RPC surface mirrors the per-robot command surface: one REP per
robot on ports `11000..11013` by default. Clients use `ZMQ_REQ`. The
metadata PUB now advertises each robot's `admin_endpoint`. The wire format
is single-frame UTF-8 JSON, op-driven.

Minimal example (`pyzmq`):

```python
import json, zmq
ctx = zmq.Context()
sock = ctx.socket(zmq.REQ)
sock.connect("tcp://127.0.0.1:11000")
sock.send_string(json.dumps({
    "op": "set_pose",
    "translation_m": [0.5, 0.0, 0.3762],
    "rotation_quat_xyzw": [0.0, 0.0, 0.0, 1.0],
}))
print(json.loads(sock.recv()))
```

Reset back to initial pose:

```python
sock.send_string(json.dumps({"op": "reset"}))
print(json.loads(sock.recv()))
```

End-to-end admin smoke test from the project root:

```bash
python Tools/run_admin_smoke_test.py
```

That command starts `/Game/Levels/URS_SoccerField` offscreen, waits for the
admin RPC to come up, then runs `Tools/admin_smoke_client.py` against
`robot_rp0`. The client exercises `set_pose` with a fresh translation,
`reset`, and a deliberate `dim_mismatch` error to confirm the validation
path. The wrapper fails unless every step returns `ok: true` (except the
deliberate error).

## Dynamic Scene Config

`UURSSceneConfigComponent` reads `Config/URS_scene.json` on `BeginPlay` and
spawns the listed robots via the registered robot types. Today only
`pi_plus` is registered (against
`/Game/MuJoCoImports/pi_plus_stereo_camera`); adding new types is a single
line in `URSoccerLabModule::StartupModule`.

```json
{
  "version": "urs_scene_v1",
  "robots": [
    {
      "actor_id": "robot_rp0",
      "type": "pi_plus",
      "translation_m": [0.0, 0.0, 0.3762],
      "rotation_quat_xyzw": [0.0, 0.0, 0.0, 1.0]
    }
  ]
}
```

Translation and rotation are optional; missing translation falls back to
the type's `DefaultBaseHeightM`, missing rotation to identity. The
component stashes the initial pose per `actor_id` so the admin `reset` op
can return to spawn.

## End-to-End Vision Smoke Test

Run from the project root:

```bash
uv run python Tools/run_vision_smoke_test.py
```

The runner:

- creates or refreshes `/Game/Levels/URS_SoccerField` with `robot_rp0`, `AAMjManager`, `UURSZmqRobotBridgeComponent`, and a ZMQ-enabled `UMjCamera`;
- starts that map with `-game -RenderOffscreen`;
- runs `py_example/main.py` to send all-zero motor commands and receive state/camera data;
- drains multiple camera frames and fails unless `py_example/out/vision_smoke/camera.png` has nonblank RGB content.

Motor command plus camera validation:

```bash
uv run python Tools/run_vision_smoke_test.py \
  --motion-regex head \
  --require-nonzero-command \
  --out py_example/out/motor_vision_smoke
```

The project-owned `pi_plus` fixture unlocks `head_yaw_joint` and
`head_pitch_joint`, and mounts `left_eye` and `right_eye` under
`head_pitch_link`.
For this phase, Pi is treated as a built-in fixed robot type; those joint and
link names are hardcoded in the project fixture instead of dynamically resolved.
Metadata reports clean component names such as `head_pitch_joint`; generated
URLab import prefixes are stripped at the URSoccerLab bridge boundary. The
camera topics are `robot_rp0/camera/left_eye` and
`robot_rp0/camera/right_eye`.
Motion smoke defaults to a constant amplitude `1.0` command for `3.0` seconds
and writes `camera_before.png`, `camera_after.png`, and `camera.png` without
image-diff assertions.

The saved soccer-field scene is baked from
`Assets/Scenes/SoccerField/source/field.glb` by
`Tools/ue_bake_soccer_field_scene.py`, launched through
`Tools/create_soccer_field_scene.py`. The GLB exported from Blender is Y-up:
`X` is field length, `Y` is vertical, and `Z` is field width. The bake script
converts that once into the UE level frame: `UE.X = 100 * GLB.X`,
`UE.Y = 100 * GLB.Z`, and `UE.Z = 100 * GLB.Y`. With URLab's robot transform
conversion, that corresponds to the project robot/MuJoCo convention where `+X`
points to the opponent goal, `+Y` is robot-left, and `+Z` is up.

The vision smoke robot spawn location is passed to `URLabLevelOps::SpawnActorSync`
in meters. URLab converts that to Unreal centimeters internally. The Pi Plus
zero-pose base height is therefore `0.3762 m`, not `37.62 cm` in the API call.

## Validation Run

Commands used during implementation:

```bash
make URSoccerLabEditor-Linux-Development ARGS="-NoHotReload -DisableUnity"
/home/luoshaoyinn/software/Unreal_Engine_5.7.4/Engine/Binaries/Linux/UnrealEditor /home/luoshaoyinn/workspace/URSoccerLab/URSoccerLab.uproject -NullRHI -unattended -nop4 -nosplash -ExecCmds="Automation RunTests URSoccerLab.Runtime.Protocol; Quit"
```

Result:

- Build succeeded with `-DisableUnity`.
- `URSoccerLab.Runtime.Protocol` found 3 tests.
- All 3 focused protocol tests passed.

Note: a normal editor unity build still exposes an existing URLabEditor test helper symbol collision between `MjActuatorInheritanceTests.cpp` and `MjMuscleTests.cpp`. The implementation validation used `-DisableUnity` to avoid changing URLab upstream code for this project-owned adapter work.
