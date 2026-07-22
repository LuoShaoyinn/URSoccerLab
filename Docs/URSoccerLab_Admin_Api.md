# URSoccerLab Admin RPC API

## Purpose

The admin RPC surface is a low-rate, request/reply control channel for
debug/admin clients. It runs alongside the high-rate motor command PULL
sockets and exposes force-move and reset operations that are awkward to
express in the binary motor protocol.

It is **not** a replacement for the motor command channel. Do not stream
`set_pose` at control rate; that blocks the physics thread on every call.
Use the motor PULL socket for streaming control and the admin REP socket
for occasional state resets.

## Transport

ZMQ only. No HTTP.

| Role | Socket type | Wire |
| --- | --- | --- |
| Server (in-simulator) | `ZMQ_REP` | one UTF-8 JSON frame per request, one UTF-8 JSON frame per reply |
| Client | `ZMQ_REQ` |  |

`zmq_setsockopt(..., ZMQ_LINGER, 0)` is set on every server socket so a
simulator shutdown never blocks on a pending reply.

## Port assignment

One REP socket per robot. The base port and per-robot offset mirror the
existing motor command PULL pattern.

| Robot | Admin port (default) | Motor port |
| --- | ---: | ---: |
| `robot_rp0` | 11000 | 10000 |
| `robot_rp1` | 11001 | 10001 |
| `robot_rp2` | 11002 | 10002 |
| `robot_rp3` | 11003 | 10003 |
| `robot_rp4` | 11004 | 10004 |
| `robot_rp5` | 11005 | 10005 |
| `robot_rp6` | 11006 | 10006 |
| `robot_bp0` | 11007 | 10007 |
| `robot_bp1` | 11008 | 10008 |
| `robot_bp2` | 11009 | 10009 |
| `robot_bp3` | 11010 | 10010 |
| `robot_bp4` | 11011 | 10011 |
| `robot_bp5` | 11012 | 10012 |
| `robot_bp6` | 11013 | 10013 |

### Discovery

The admin endpoint for each robot is advertised on the shared metadata
PUB (`tcp://0.0.0.0:10101` by default, topic `meta/<robot>`) as
`admin_endpoint`. Always resolve the port from metadata rather than
hard-coding `11000` — the simulator owner can override `AdminBasePort` on
`UURSZmqRobotBridgeComponent`.

Example metadata fragment:

```json
{
  "robot": "robot_rp0",
  "command_endpoint": "tcp://0.0.0.0:10000",
  "admin_endpoint": "tcp://0.0.0.0:11000",
  "state_topic": "state/robot_rp0",
  "actuator_names": ["head_yaw_joint", "head_pitch_joint", "..."]
}
```

For client use, replace the bind host `0.0.0.0` with the simulator host
(e.g. `tcp://127.0.0.1:11000`).

### Validation rules

The bridge rejects the following configurations at startup:

- `AdminBasePort < 11000` (`MinAdminPort`).
- Any admin port colliding with the motor command range
  (`CommandBasePort .. CommandBasePort + RobotCount - 1`).
- Any admin port colliding with the shared state PUB port (`StatePort`,
  default `10100`) or metadata PUB port (`MetaPort`, default `10101`).

If any of those fail, `StartBridge` returns false and logs an error; no
admin sockets are bound.

## Wire format

One ZMQ frame per request, one ZMQ frame per reply. Bodies are UTF-8 JSON.
No multipart, no binary headers.

### Request shape

```json
{ "op": "<op_name>", ...op-specific fields }
```

`op` is required. Unknown ops return `unknown_op`.

### Reply shape — success

```json
{ "ok": true, "op": "<op_name>", ...op-specific fields }
```

`ok` is a real JSON boolean, not a string.

### Reply shape — error

```json
{
  "ok": false,
  "op": "<op_name>",
  "error": "<short_code>",
  "message": "<human-readable detail>"
}
```

The `op` field always echoes the request's `op` (or `"unknown"` if the
request could not be parsed past the `op` field).

## Ops

### `get_pose` — read robot pose and joint positions

Read the robot's current pose from MuJoCo ground truth. The root body is
resolved via `mjModel::body_rootid` of the articulation's first joint body,
so the read works for both free-base and fixed-base articulations.

#### Request

```json
{ "op": "get_pose" }
```

No additional fields.

#### Reply

```json
{
  "ok": true,
  "op": "get_pose",
  "actor_id": "robot_rp0",
  "translation_m": [0.0, 0.0, 0.3762],
  "rotation_quat_xyzw": [0.0, 0.0, 0.0, 1.0],
  "joint_qpos": [0.0, 0.1, -0.1, "..."],
  "sim_time_sec": 3.276
}
```

| Field | Source |
| --- | --- |
| `translation_m` | `mjData::xpos[root_body_id]` (MuJoCo frame, meters). When the articulation has no resolvable root body, falls back to the free-joint qpos slots or `[0, 0, 0]`. |
| `rotation_quat_xyzw` | `mjData::xquat[root_body_id]` (MuJoCo stores `w x y z`; the reply repacks to the wire's `x y z w`). |
| `joint_qpos` | Non-root qpos slots in joint-ID order. Length equals the non-root qpos dimension reported by `set_pose`'s `dim_mismatch` error. |

For a fixed-base robot (e.g. `base_link` welded to the world with no
`<freejoint/>`), `translation_m` and `rotation_quat_xyzw` reflect the
world-fixed pose; `joint_qpos` is the meaningful field.

#### Errors

| `error` | When |
| --- | --- |
| `not_ready` | The articulation, `AAMjManager`, `UMjPhysicsEngine`, `mjModel`, or `mjData` is missing. |

### `set_pose` — force-write robot pose and joint positions

Force-write the robot's root pose and/or joint positions. Writes happen
under `AAMjManager::PhysicsEngine->CallbackMutex`, the same lock URLab's
`set_qpos` / `reset` RPCs take, so the write is atomic with respect to the
physics step. The robot's qvel is zeroed in any case — force-move means
"no carried velocity".

#### Request

All fields except `op` are optional. Absent fields are treated as zero
(identity quaternion for rotation).

```json
{
  "op": "set_pose",
  "translation_m": [0.5, 0.0, 0.3762],
  "rotation_quat_xyzw": [0.0, 0.0, 0.0, 1.0],
  "joint_qpos": [0.0, 0.1, -0.1, "..."]
}
```

| Field | Type | Required | Default if absent |
| --- | --- | :---: | --- |
| `translation_m` | array[3] of number (meters, MuJoCo frame) | no | `[0.0, 0.0, 0.0]` |
| `rotation_quat_xyzw` | array[4] of number (`x y z w`, unit quaternion) | no | `[0.0, 0.0, 0.0, 1.0]` (identity) |
| `joint_qpos` | array[N] of number (radians for hinge, meters for slide) | no | `[0.0, 0.0, ..., 0.0]` (length N) |

For a robot whose root joint is `mjJNT_FREE`, the free-joint qpos is laid
out as MuJoCo's documented order `[x, y, z, qw, qx, qy, qz]` — note the
quaternion's `w` component comes first inside MuJoCo, even though the
wire convention is `x y z w`. The handler performs the repack. `joint_qpos`
covers every remaining non-root joint in joint-ID order.

For a robot whose `base_link` is welded to the world (no `<freejoint/>`),
`translation_m` and `rotation_quat_xyzw` cannot be applied — the handler
returns `fixed_base` if either is present. Send `joint_qpos` only for
fixed-base robots.

#### Reply

```json
{
  "ok": true,
  "op": "set_pose",
  "actor_id": "robot_rp0",
  "applied_translation_m": [0.5, 0.0, 0.3762],
  "applied_rotation_quat_xyzw": [0.0, 0.0, 0.0, 1.0],
  "applied_joint_qpos": [0.0, 0.1, -0.1, "..."],
  "sim_time_sec": 3.276
}
```

`applied_joint_qpos` mirrors the request's `joint_qpos` field (non-root
only), expanded with zeros if the field was absent. Use `get_pose` to
verify what MuJoCo actually settled to after the next physics step.

#### Errors

| `error` | When |
| --- | --- |
| `bad_request` | Request body is not valid JSON, `op` is missing or empty, `op` is not a known op, `translation_m` does not have exactly 3 finite numbers, `rotation_quat_xyzw` does not have exactly 4 finite numbers, or any `joint_qpos` element is non-finite. The `message` field contains the specific parse failure (`NotJson`, `MissingOp`, `UnknownOp`, `BadTranslation`, `BadRotation`, `BadJointQpos`). |
| `dim_mismatch` | `joint_qpos` is present but its length does not equal the articulation's non-root qpos dimension. The `message` reports both lengths. |
| `fixed_base` | `translation_m` or `rotation_quat_xyzw` is present but the articulation has no free root joint. Send `joint_qpos` only. |
| `not_ready` | The articulation, `AAMjManager`, `UMjPhysicsEngine`, `mjModel`, or `mjData` is missing. Usually means the request arrived before `StartBridge` completed or after `StopBridge`. |

### `reset` — return robot to its initial state

Reset this robot to the spawn pose recorded by
`UURSSceneConfigComponent`. If the scene config component is not
attached (e.g. the level predates the component) or has no record for
this `actor_id`, the robot is reset to all-zero qpos (origin translation,
identity rotation, zero joints).

In both cases the implementation reuses `set_pose`, so the write
semantics, locking, and reply shape are identical.

#### Request

```json
{ "op": "reset" }
```

No additional fields. Any extra fields are ignored.

#### Reply

Same shape as `set_pose`. The `applied_*` fields show the values the
robot was reset to — useful for confirming whether the scene-config
spawn pose or the zero fallback was used.

```json
{
  "ok": true,
  "op": "reset",
  "actor_id": "robot_rp0",
  "applied_translation_m": [0.0, 0.0, 0.3762],
  "applied_rotation_quat_xyzw": [0.0, 0.0, 0.0, 1.0],
  "applied_joint_qpos": [0.0, "..."],
  "sim_time_sec": 4.812
}
```

#### Errors

Same `bad_request` and `not_ready` categories as `set_pose`. There is no
"no initial pose" error in this release — the handler falls back to zero
qpos and returns `ok: true`. Fixed-base robots will still return `ok: true`
for `reset` because the fallback omits `translation_m`/`rotation_quat_xyzw`
and only writes `joint_qpos`.

## Concurrency

- Admin sockets are drained once per game-thread tick from
  `UURSZmqRobotBridgeComponent::TickComponent`. Draining runs regardless
  of `bUsePhysicsCallbacks`, so admin RPCs work both in the
  physics-callback path (the packaged simulator default) and in the
  game-thread tick path.
- Each drain pass calls `zmq_poll(..., timeout=0)` on every admin REP
  socket. Readable sockets are served serially: `recv` → parse → handle
  → `send`. REP's strict recv→send ordering means a slow handler blocks
  only its own socket.
- Handlers that touch `mjData` (`set_pose`, `reset`) take
  `Manager->PhysicsEngine->CallbackMutex` for the duration of the read or
  write plus the trailing `mj_forward(m, d)`. This is the same lock
  URLab's `set_qpos` / `reset` RPCs take; no new lock ordering is
  introduced. The lock is taken from the game thread, so a long physics
  step will briefly stall the admin handler — acceptable for a low-rate
  debug channel.
- `mj_forward` is called inside the lock so the next render snapshot and
  the next `state/<robot>` PUB packet reflect the new pose immediately.

## Client example

End-to-end Python example using `pyzmq`. The host defaults to localhost;
for remote simulators, replace it with the simulator host.

```python
import json, zmq

ctx = zmq.Context()

# 1. Discover the admin endpoint from the metadata PUB.
sub = ctx.socket(zmq.SUB)
sub.setsockopt_string(zmq.SUBSCRIBE, "meta/robot_rp0")
sub.connect("tcp://127.0.0.1:10101")
topic, payload = sub.recv_multipart()
meta = json.loads(payload.decode("utf-8"))
sub.close(linger=0)

admin_endpoint = meta["admin_endpoint"].replace("tcp://0.0.0.0:", "tcp://127.0.0.1:")

# 2. Open the REQ socket.
sock = ctx.socket(zmq.REQ)
sock.setsockopt(zmq.LINGER, 0)
sock.connect(admin_endpoint)

# 3. Read current pose.
sock.send_string(json.dumps({"op": "get_pose"}))
print(json.loads(sock.recv()))

# 4. Move joint 0 by 0.1 rad (this robot is fixed-base, so we can only
#    address joint_qpos; a translation_m field would return fixed_base).
sock.send_string(json.dumps({
    "op": "set_pose",
    "joint_qpos": [0.1] + [0.0] * 21,
}))
reply = json.loads(sock.recv())
assert reply["ok"], reply

# 5. Return to spawn (zeros joints for this fixed-base robot).
sock.send_string(json.dumps({"op": "reset"}))
reply = json.loads(sock.recv())
assert reply["ok"], reply

sock.close(linger=0)
ctx.term()
```

## Reference client

`Tools/admin_smoke_client.py` is a complete runnable client that:

1. Subscribes to `meta/<robot>` and parses `admin_endpoint`.
2. Calls `get_pose` to read the spawn pose and joint qpos dimension.
3. Calls `set_pose` with a `joint_qpos` vector that perturbs joint 0,
   then calls `get_pose` again and asserts MuJoCo ground-truth qpos
   moved accordingly.
4. Calls `set_pose` with `translation_m`. If the articulation has a free
   root joint, asserts the move landed in `xpos`; otherwise asserts the
   reply is `error: fixed_base`.
5. Calls `reset`, then `get_pose` and asserts every joint qpos is back
   near zero (within 0.05 rad of physics-step drift).
6. Sends an intentionally malformed `set_pose` (`joint_qpos` of length 1)
   and asserts the reply's `error` is `dim_mismatch`.

`Tools/run_admin_smoke_test.py` wraps that client with a simulator
launcher (same pattern as `run_vision_smoke_test.py`):

```bash
python Tools/run_admin_smoke_test.py
```

The wrapper starts `/Game/Levels/URS_SoccerField` offscreen, waits for
the `URSoccerLab admin RPC ready` log line, runs the client through the
`py_example` venv, and fails unless every step succeeds.

## Configuration reference

`UURSZmqRobotBridgeComponent` exposes one property for the admin surface:

| Property | Type | Default | Notes |
| --- | --- | ---: | --- |
| `AdminBasePort` | `int32` | `11000` | Clamped to `[11000, 65535]`. The allocator rejects any base that would collide with the motor command range or the state/metadata PUB ports. |

There is no auth on the admin surface in this release. The intended
deployment is a trusted LAN or localhost; do not expose the admin ports
to untrusted networks.

## Future extension

The current shape leaves these as small, localised additions:

- New op: add an `EAdminOp` enum value, a branch in
  `UURSZmqRobotBridgeComponent::HandleAdminRequest`, and a parser case
  in `FAdminProtocol::ParseRequest`. No socket or drain-loop changes.
- New request field on `set_pose`: add a `TOptional<...>` on
  `FAdminPoseRequest` and a reader in `ParseRequest`. Old clients that
  omit the field keep working.
- Free-base robots: when the asset gets a `<freejoint/>`,
  `set_pose` with `translation_m`/`rotation_quat_xyzw` becomes
  accepted; the smoke test already verifies the resulting `xpos` change.
- Scene-wide ops (e.g. a global reset, config reload): add a second REP
  socket in the same drain loop, mirroring the per-robot pattern.
- Auth: add an optional `AdminAuthToken` UPROPERTY on the bridge and
  check a `token` field on every request inside `HandleAdminRequest`
  before dispatching.
