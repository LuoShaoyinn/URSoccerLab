# URSoccerLab Dynamic Scene + Admin RPC Plan

## Scope

Two features, minimal surface, extension-friendly structure.

1. **Dynamic scene build from config.** Load a baked field, spawn a list of
   predefined robot types at configured poses. Robots are referenced by a
   short type name registered in code, not by raw Blueprint paths in the
   config file.
2. **Admin RPC over ZMQ, one REP per robot.** Two ops only:
   - `set_pose` — force-set robot translation, rotation, and joint qpos.
     Any field absent in the request is treated as zero.
   - `reset` — return the robot to its initial spawn pose (from the scene
     config) with zeroed velocity.

Non-goals: cameras/lights in config, scene reload, keyframes, auth, HTTP,
scene-admin port. The code shape leaves room for them; this phase does not
ship them.

## Architecture

### Ports

| Surface | Pattern | Default | Status |
| --- | --- | ---: | --- |
| Motor command (per robot) | PUSH→PULL | 10000–10013 | existing |
| State PUB | PUB→SUB | 10100 | existing |
| Metadata PUB | PUB→SUB | 10101 | existing |
| **Admin RPC (per robot)** | **REQ→REP** | **11000–11013** | **NEW** |

Admin REP sockets live inside the existing `UURSZmqRobotBridgeComponent`,
sharing its `ZmqContext`, its `AAMjManager` handle, its per-robot endpoint
cache, and the physics-thread `CallbackMutex`. No new actor.

### Coordinate frame

All values in the config and the admin RPC are MuJoCo-frame meters, exactly
as documented in `URLab_Builtin_Behavior.md` (`+X` forward, `+Y` left,
`+Z` up). URLab's spawn API already converts to UE cm and applies the
handedness flip; URSoccerLab never adds another flip.

### Lifecycle

1. Game loads baked `/Game/Levels/URS_SoccerField` (field + sky only).
2. `AAMjManager` compiles the field-only model.
3. `UURSSceneConfigComponent` (new, attached to `AAMjManager`) reads
   `Config/URS_scene.json`, looks up each robot's type in the registry, and
   spawns the articulations on the game thread.
4. `UURSZmqRobotBridgeComponent::StartBridge` discovers the spawned
   articulations, binds motor PULL + admin REP per robot, binds state/meta
   PUB, starts the metadata publish.

## Module layout (extension-friendly)

```
URSoccerLab/
  Public/Scene/
    URSSceneConfig.h          structs + JSON IO
    URSRobotTypeRegistry.h    type registry (extension point)
    URSSceneConfigComponent.h actor component that spawns from config
  Public/Runtime/
    URSRobotProtocol.h        EXTEND: admin port constants
    URSAdminProtocol.h        NEW: set_pose/reset JSON contracts
    URSZmqRobotBridgeComponent.h  EXTEND: admin REP sockets
  Private/Scene/...
  Private/Runtime/URSAdminProtocol.cpp
  Private/Runtime/URSZmqRobotBridgeComponent.cpp  EXTEND
  Private/Tests/
    URSSceneConfigTests.cpp
    URSAdminProtocolTests.cpp
```

The split between `Scene/` and `Runtime/` keeps future scene-side features
(cameras, lights, ball) separate from transport-side features (state
publishing, more admin ops). The registry is the only place that needs to
change when a new robot type is added.

## Feature 1 — Scene Config

### Robot type registry (extension point)

In `Public/Scene/URSRobotTypeRegistry.h`:

```cpp
namespace URSoccerLab
{
struct FURSRobotType
{
    FString Name;                              // "pi_plus"
    FString BlueprintPath;                     // /Game/MuJoCoImports/pi_plus_stereo_camera...
    double DefaultBaseHeightM = 0.0;          // safe spawn Z if config omits translation
};

class FURSRobotTypeRegistry
{
public:
    static FURSRobotTypeRegistry& Get();
    void Register(const FURSRobotType& Type);
    const FURSRobotType* Find(const FString& Name) const;
    TArray<FString> GetRegisteredNames() const;
private:
    TMap<FString, FURSRobotType> Types;
};
} // namespace URSoccerLab
```

The game module's `StartupModule` registers every shipped robot type. Adding
a new robot type is one line in the module startup — no other code changes.

### Config file

`Config/URS_scene.json`:

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

`type` resolves through the registry. `translation_m` / `rotation_quat_xyzw`
are optional; missing translation falls back to
`[0, 0, Type.DefaultBaseHeightM]`, missing rotation to identity. The
`actor_id` must be unique and is what every other system (ZMQ bridge,
admin RPC) uses to address the robot.

### Config structs

In `Public/Scene/URSSceneConfig.h`:

```cpp
namespace URSoccerLab
{
struct FURSRobotSpawn
{
    FString ActorId;
    FString Type;
    TOptional<FVector> TranslationMeters;
    TOptional<FQuat> RotationQuatXyzw;
};

struct FURSSceneConfig
{
    FString Version = TEXT("urs_scene_v1");
    TArray<FURSRobotSpawn> Robots;
};

class FURSSceneConfigIo
{
public:
    static bool LoadFromFile(const FString& AbsPath, FURSSceneConfig& Out, FString& OutError);
    static FURSSceneConfig MakeDefault();   // current single-robot_rp0 smoke layout
};
} // namespace URSoccerLab
```

`FURSRobotSpawn` is deliberately a small struct. Future fields (cameras,
team, keyframe) slot in without breaking the loader; the loader ignores
unknown keys so old configs keep working on new code.

### Scene config component

`UURSSceneConfigComponent` (attached to `AAMjManager`):

- `UPROPERTY ConfigPath` defaults to `Config/URS_scene.json`.
- `UPROPERTY bool bAutoApplyOnBeginPlay = true`.
- `ApplyConfig()`:
  1. Destroy any existing articulations whose `ActorId` appears in the new
     config (idempotent re-apply).
  2. For each spawn, resolve the type via the registry. Fail loudly if the
     type name is unknown.
  3. Compute the final transform: explicit field if present, else
     `[0, 0, Type.DefaultBaseHeightM]` + identity rotation.
  4. Spawn via `URLabLevelOps::SpawnActorSync` in editor, or
     `World->SpawnActor<AMjArticulation>` from the Blueprint generated class
     (`<BlueprintPath>_C`) in packaged builds; set `ActorId` and rename the
     actor to match.
  5. Stash the initial pose per `ActorId` so the admin `reset` op can
     return to it.
  6. Fire `FOnSceneConfigApplied` delegate; `UURSZmqRobotBridgeComponent`
     listens and calls `RebuildEndpointCache()` + `StartBridge()`.

## Feature 2 — Admin RPC

### Protocol constants

`Public/Runtime/URSRobotProtocol.h` gains:

```cpp
static constexpr int32 MinAdminPort = 11000;
static constexpr int32 DefaultAdminBasePort = 11000; // 11000..11013
```

`FRobotRuntimeConfig` gains `int32 AdminBasePort = DefaultAdminBasePort;`.
`FRobotProtocol` gains:

```cpp
static bool IsValidAdminBasePort(int32 BasePort, int32 RobotCount);
static bool BuildAdminPortAssignments(const FRobotRuntimeConfig& Config,
    TArray<FRobotPortAssignment>& OutAssignments);
```

The allocator rejects base ports below 11000 and any overlap with the
command (10000–10013), state (10100), or meta (10101) ranges. The existing
`BuildTcpBindEndpoint(Port)` helper is reused.

### Wire format

One ZMQ frame per request, one ZMQ frame per reply, UTF-8 JSON, op-driven.
Same low-rate JSON style as the existing metadata PUB.

Request: `{"op": "set_pose", "translation_m": [...], "rotation_quat_xyzw": [...], "joint_qpos": [...]}`

Reply (success): `{"ok": true, "op": "...", "actor_id": "..."}`

Reply (error): `{"ok": false, "op": "...", "error": "..."}`

### Socket type

`ZMQ_REP` per robot on `tcp://0.0.0.0:<admin_port>`. Clients use `ZMQ_REQ`.
REP's strict recv→process→send cycle is exactly what RPC needs. Linger is
set to 0 (same as the existing command sockets). Admin endpoints are
discovered via the existing `meta/<robot>` PUB, which gains an
`admin_endpoint` field.

### Ops

Two ops only. The actor_id is implied by the socket; clients do not send it.

#### `set_pose` — force-set robot pose and joints

Request (every field optional):
```json
{
  "op": "set_pose",
  "translation_m": [0.5, 0.0, 0.3762],
  "rotation_quat_xyzw": [0.0, 0.0, 0.0, 1.0],
  "joint_qpos": [0.1, -0.1]
}
```

Semantics — **any field absent is treated as zero**:

- Resolve the articulation's root joint. If it is `mjJNT_FREE`, the free
  joint qpos slot is the first 7 values: `[tx, ty, tz, qx, qy, qz, qw]`.
  - `translation_m` present → overwrite slots 0–2. Absent → set to 0.
  - `rotation_quat_xyzw` present → overwrite slots 3–6. Absent → identity
    `[0, 0, 0, 1]`.
- `joint_qpos` overwrites the non-root joint qpos slots in order. If
  absent, all non-root joints are set to 0. Length must equal the
  articulation's non-root qpos dimension, else `dim_mismatch`.
- After the write: zero this robot's `qvel` slots (force-move means "no
  carried velocity"), then `mj_forward(m, d)` so the next render/state
  publish reflects the new pose.

All mjData access happens under `Manager->PhysicsEngine->CallbackMutex`,
same lock ordering as URLab's `set_qpos` RPC.

Reply:
```json
{
  "ok": true, "op": "set_pose", "actor_id": "robot_rp0",
  "applied_translation_m": [0.5, 0.0, 0.3762],
  "applied_rotation_quat_xyzw": [0.0, 0.0, 0.0, 1.0],
  "applied_joint_qpos": [0.1, -0.1],
  "sim_time_sec": 1.23
}
```

#### `reset` — return to initial spawn state

Request: `{"op": "reset"}`.

Looks up this robot's stashed spawn pose from
`UURSSceneConfigComponent` (translation + rotation from the config) and
behaves exactly like `set_pose` with that translation+rotation and absent
`joint_qpos` (i.e. all joints to 0, velocity to 0).

Reply:
```json
{
  "ok": true, "op": "reset", "actor_id": "robot_rp0",
  "applied_translation_m": [0.0, 0.0, 0.3762],
  "applied_rotation_quat_xyzw": [0.0, 0.0, 0.0, 1.0],
  "sim_time_sec": 1.23
}
```

If the robot was not spawned by the scene config component (no stashed
initial pose), `reset` returns
`{"ok": false, "error": "no_initial_pose"}`.

### Drain loop

`UURSZmqRobotBridgeComponent` gains `DrainAdminSockets()`, called from
`TickComponent` and from `PostStepPhysics` when `bUsePhysicsCallbacks=true`.
It uses `zmq_poll` with timeout 0 over every admin REP socket. For each
readable socket: `recv` → parse JSON → dispatch by `op` → `send` reply.
Handlers take `CallbackMutex` for any mjData access. REP's strict ordering
means a slow handler blocks only its own socket, not the others.

### Bridge component changes

`URSZmqRobotBridgeComponent.h` gains:

```cpp
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab|Admin",
    meta = (ClampMin = "11000", ClampMax = "65535"))
int32 AdminBasePort = URSoccerLab::DefaultAdminBasePort;

UFUNCTION(BlueprintCallable, Category = "URSoccerLab|Admin")
int32 DrainAdminSockets();
```

The private `FRobotRuntimeEndpoint` struct gains `void* AdminSocket = nullptr;`.
`FURSRobotEndpointInfo` (the Blueprint-visible struct) gains
`FString AdminEndpoint;` and `PublishMetadata` includes it in
`meta/<robot>`.

`StartBridge` binds admin REP sockets after the motor PULL sockets;
`StopBridge` closes them. Bind/close share the existing
`BindCommandSockets`/`CloseCommandSockets` pattern in two new
`BindAdminSockets`/`CloseAdminSockets` helpers.

## Future extension (intentionally not built now)

The structure leaves these as one-line or one-method additions:

- New robot type → `FURSRobotTypeRegistry::Register(...)` in module startup.
- New config field on robots (cameras, team, keyframe) → add to
  `FURSRobotSpawn`; loader ignores unknown keys so old configs still parse.
- New admin op → add enum value + handler in the dispatch switch; no socket
  changes.
- Scene-wide ops → add one more REP socket (`SceneAdminSocket`) in the same
  drain loop, mirroring the per-robot pattern.
- Auth → optional `AdminAuthToken` field on the bridge; the drain loop
  checks a `token` field on every request.

## Test plan

### Pure C++ unit tests

`URSSceneConfigTests.cpp`:
- `MakeDefault()` returns one robot (`robot_rp0`, type `pi_plus`) matching
  the current smoke fixture.
- `LoadFromFile` round-trips through the registry; rejects unknown version,
  duplicate `actor_id`, unknown `type`, non-finite translation.
- Missing translation falls back to the registered default base height.
- Registry rejects duplicate type names.

`URSAdminProtocolTests.cpp`:
- `BuildAdminPortAssignments` assigns 11000–11013 to the 14 default robots.
- `IsValidAdminBasePort` rejects 1099 and below.
- Allocator flags overlap with 10100 (state) and 10010 (inside command).
- `ParseAdminRequest`/`BuildAdminReply` round-trip both ops; unknown op →
  `unknown_op` reply.
- `set_pose` with absent `translation_m` applies zeros; with absent
  `rotation_quat_xyzw` applies identity; with absent `joint_qpos` zeros
  joints.
- `set_pose` with wrong `joint_qpos` length → `dim_mismatch`.

### Editor integration

Add `URSoccerLab.E2E.ApplyDefaultSceneConfig` next to the existing
`FURSVisionSmokeCreateMap`. It loads the field level, calls
`ApplyConfig(MakeDefault())`, then asserts the bridge sees exactly
`robot_rp0` with `AdminEndpoint = tcp://0.0.0.0:11000`. The existing smoke
test is refactored to call `ApplyConfig(MakeDefault())` instead of its
hardcoded spawn block.

### Python smoke

New `Tools/admin_smoke_client.py` (uses only `pyzmq`):

```python
ctx = zmq.Context()
admin = ctx.socket(zmq.REQ)
admin.connect("tcp://127.0.0.1:11000")
admin.send_json({"op": "set_pose", "translation_m": [0.5, 0.0, 0.3762]})
print(admin.recv_json())
admin.send_json({"op": "reset"})
print(admin.recv_json())
```

`Tools/run_admin_smoke_test.py` wraps this in the same simulator-launch
pattern as `run_vision_smoke_test.py`, waits for the
`URSoccerLab admin RPC ready` log line, and fails unless both ops return
`ok: true`.

## Commit steps

1. **Commit this plan.**
2. **Scene config + registry.** `URSSceneConfig.h/.cpp`,
   `URSRobotTypeRegistry.h/.cpp`, `Config/URS_scene.json` sample, module
   startup registration of `pi_plus`. Pure unit tests in
   `URSSceneConfigTests.cpp`.
3. **Scene config component.** `UURSSceneConfigComponent` with editor +
   packaged spawn paths, initial-pose stash, `FOnSceneConfigApplied`
   delegate. Refactor existing `FURSVisionSmokeCreateMap` to call
   `ApplyConfig(MakeDefault())`.
4. **Admin protocol constants + JSON.** `URSRobotProtocol.h` port
   constants + allocator; `URSAdminProtocol.h/.cpp` request/reply JSON +
   op enum. Pure unit tests in `URSAdminProtocolTests.cpp`.
5. **Admin sockets + ops.** Extend `UURSZmqRobotBridgeComponent` with
   admin REP sockets, `DrainAdminSockets`, and the `set_pose` + `reset`
   handlers. Metadata PUB gains `admin_endpoint`.
6. **Python smoke + docs.** `admin_smoke_client.py`,
   `run_admin_smoke_test.py`. Update
   `URSoccerLab_ZMQ_Runtime_Validation.md` with the admin RPC quickstart
   (port table, both ops, snippet) and a "Dynamic Scene Config" section
   pointing at `Config/URS_scene.json`.
