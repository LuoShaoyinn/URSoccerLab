# URSoccerLab ZMQ Runtime Adapter Plan

## Goal

URSoccerLab should run like a real robot-facing simulator:

- MuJoCo advances continuously in URLab live mode at a fixed simulation rate.
- External policies send direct motor commands, not speed commands and not walk-policy commands.
- Each robot has an isolated inbound command endpoint on a TCP port above 10000.
- State, proprioception, metadata, and camera streams are available asynchronously.
- The packaged simulator should run without the Unreal Editor or a full UE development environment.

Dynamic runtime URDF or MJCF import is intentionally out of scope for the first implementation. Robots and scenes are cooked into the Unreal package first; external robot files can be added later after the runtime protocol is stable.

## Architecture

Keep the project-specific protocol in the `URSoccerLab` game module and consume URLab runtime APIs from there. This keeps `Plugins/UnrealRoboticsLab` close to upstream and lets URSoccerLab own soccer-specific naming, port assignment, and client contracts.

The adapter is an actor component intended to live in the same world as `AAMjManager`.

- `AAMjManager` owns the URLab MuJoCo model, data, and articulation list.
- `UURSZmqRobotBridgeComponent` discovers URLab articulations and their actuators after the model is ready.
- A per-robot command socket receives the newest motor command.
- On each MuJoCo pre-step, the bridge applies the newest valid command to that robot's URLab actuators.
- On each post-step or game-thread tick, the bridge publishes state snapshots and metadata.

## Ports

Default command ports:

| Robot | Port |
| --- | ---: |
| `robot_rp0` | 10000 |
| `robot_rp1` | 10001 |
| `robot_rp2` | 10002 |
| `robot_rp3` | 10003 |
| `robot_rp4` | 10004 |
| `robot_rp5` | 10005 |
| `robot_rp6` | 10006 |
| `robot_bp0` | 10007 |
| `robot_bp1` | 10008 |
| `robot_bp2` | 10009 |
| `robot_bp3` | 10010 |
| `robot_bp4` | 10011 |
| `robot_bp5` | 10012 |
| `robot_bp6` | 10013 |

Shared outbound defaults:

- State PUB: `tcp://0.0.0.0:10100`
- Metadata PUB: `tcp://0.0.0.0:10101`
- Cameras: use URLab camera streaming endpoints, with robot identity documented in metadata.

The command-port allocator should reject base ports below 10000.

## Motor Command Protocol

Initial payload format is compact binary for fixed-rate control:

```text
uint32 magic       "URSM"
uint16 version     1
uint16 flags       reserved
uint64 sequence
double stamp_sec
uint32 num_motors
float32 motors[num_motors]
```

Validation rules:

- `magic` must match.
- `version` must be supported.
- `num_motors` must match the discovered actuator count for that robot.
- Motor values must be finite.
- `sequence` must be newer than the last accepted sequence for that robot.
- If no valid command arrives before timeout, the robot falls back to zero command.

## Metadata Protocol

Metadata is JSON because it is low-rate and intended for client discovery:

- robot name
- command endpoint
- state topic
- actuator names in command order
- actuator ids
- actuator control ranges
- camera names and endpoints, when available

State topics use `state/<robot_name>`.

## Test Plan

Add low-level automation tests before the live ZMQ bridge:

- default robot names map to command ports `10000..10013`
- configured base ports below 10000 are rejected
- stale sequences are rejected
- wrong motor counts are rejected
- non-finite motor values are rejected
- timeout detection changes robot command health
- one robot command buffer does not mutate another robot command buffer

After live integration, add a Python smoke client that:

- sends a motor command to `tcp://127.0.0.1:10000`
- subscribes to `state/robot_rp0`
- verifies that metadata, sequence, and timestamps move forward

## Commit Steps

1. Commit this plan.
2. Commit protocol/config/types and pure unit tests.
3. Commit the C++ ZMQ command receiver and endpoint cache.
4. Commit state and metadata publisher.
5. Commit URLab live-mode pre-step/post-step integration.
6. Commit smoke client and final validation notes.
