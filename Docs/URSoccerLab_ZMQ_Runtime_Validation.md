# URSoccerLab ZMQ Runtime Validation

## Implemented

- Inbound command isolation: one `ZMQ_PULL` command socket per active robot.
- Default robot command ports: `robot_rp0..robot_rp6` on `10000..10006`, `robot_bp0..robot_bp6` on `10007..10013`.
- Shared state publisher: `tcp://0.0.0.0:10100`, topic `state/<robot>`.
- Shared metadata publisher: `tcp://0.0.0.0:10101`, topic `meta/<robot>`.
- Commands are drained in URLab `PreStep` callbacks and written to URLab actuator network controls before URLab applies controls.
- State is published from URLab `PostStep` callbacks at `StatePublishRateHz`.
- Fallback game-thread tick path remains available by setting `bUsePhysicsCallbacks=false`.

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
