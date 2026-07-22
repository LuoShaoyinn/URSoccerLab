# URSoccerLab Scene Building API

Describes how the soccer scene is assembled at runtime: field-only
`.umap`, JSON scene config, robot type registry, and the GameMode
bootstrap that spawns robots before URLab compiles the MuJoCo model.

## Architecture

```
/Game/Levels/URS_SoccerField.umap        (baked: field + sky + manager + components)
Config/URS_scene.json                    (runtime: robot list, types, poses)
Assets/MosBrainCameraTest/pi_plus/...xml (source MJCF for each robot type)
/Game/MuJoCoImports/pi_plus_stereo_camera (cooked Blueprint per robot type)
```

At simulator startup:

1. Engine loads `URS_SoccerField.umap`. Level actors (field meshes,
   skylight, `AAMjManager` with `UURSZmqRobotBridgeComponent` +
   `UURSSceneConfigComponent`) go through `PreInitializeComponents` →
   `InitializeComponent` → `PostInitializeComponents`.
2. `AURSSoccerGameMode::InitGame` runs (before any `BeginPlay`).
3. It finds `UURSSceneConfigComponent`, calls `ApplyConfig`.
4. `ApplyConfig` reads `Config/URS_scene.json`, resolves robot types via
   `FURSRobotTypeRegistry`, and spawns `AMjArticulation` actors via
   `World->SpawnActor` + `LoadClass`.
5. Each spawned robot configures its own cameras (ZMQ endpoints,
   resolution, FOV) and hides imported field geoms.
6. `World->BeginPlay` fires. `AAMjManager::BeginPlay` compiles the MuJoCo
   model — the dynamically-spawned robots are discovered via
   `TActorIterator<AMjArticulation>` and enter `mjModel`.
7. `UURSZmqRobotBridgeComponent::BeginPlay` starts the bridge: binds
   per-robot motor PULL + admin REP sockets, state PUB, metadata PUB.

The key ordering guarantee: **robots exist in the world before
`AAMjManager::BeginPlay` compiles**, because `InitGame` runs before
`BeginPlay`.

## Config file format

`Config/URS_scene.json`:

```json
{
  "version": "urs_scene_v1",
  "robots": [
    {
      "actor_id": "robot_rp0",
      "type": "pi_plus",
      "translation_m": [-1.0, 0.0, 0.3762],
      "rotation_quat_xyzw": [0.0, 0.0, 0.0, 1.0]
    },
    {
      "actor_id": "robot_rp1",
      "type": "pi_plus",
      "translation_m": [1.0, 0.0, 0.3762],
      "rotation_quat_xyzw": [0.0, 0.0, 1.0, 0.0]
    }
  ]
}
```

| Field | Type | Required | Default if absent |
| --- | --- | :---: | --- |
| `version` | string | yes | — (must be `"urs_scene_v1"`) |
| `robots` | array | yes | — |
| `robots[].actor_id` | string | yes | — (must be unique) |
| `robots[].type` | string | yes | — (must exist in the registry) |
| `robots[].translation_m` | `[x, y, z]` meters (MuJoCo frame) | no | `[0, 0, Type.DefaultBaseHeightM]` |
| `robots[].rotation_quat_xyzw` | `[x, y, z, w]` | no | `[0, 0, 0, 1]` (identity) |

### Coordinate frame

All values are in the MuJoCo/robot frame (`+X` forward, `+Y` left,
`+Z` up), in meters. URLab converts to UE centimeters + handedness flip
internally — project code never adds another Y flip.

### Quaternion convention

Wire order is `[x, y, z, w]`. MuJoCo internally stores free-joint qpos as
`[x, y, z, qw, qx, qy, qz]`; the admin API and scene config handle the
repack — config writers always use `xyzw`.

### Rotation examples

| Facing direction | `rotation_quat_xyzw` |
| --- | --- |
| Identity (+X forward) | `[0, 0, 0, 1]` |
| 180° yaw (face -X) | `[0, 0, 1, 0]` |
| 90° yaw left (face +Y) | `[0, 0, 0.7071, 0.7071]` |
| 90° yaw right (face -Y) | `[0, 0, -0.7071, 0.7071]` |

## Robot type registry

`FURSRobotTypeRegistry` is a singleton that maps short type names to
Blueprint asset paths. Adding a new robot type is one line in
`FURSoccerLabModule::StartupModule`.

### Registered types

| Name | Blueprint | Default base height |
| --- | --- | ---: |
| `pi_plus` | `/Game/MuJoCoImports/pi_plus_stereo_camera.pi_plus_stereo_camera` | 0.3762 m |

### C++ API

```cpp
namespace URSoccerLab
{
struct FURSRobotType
{
    FString Name;               // "pi_plus"
    FString BlueprintAssetPath; // "/Game/MuJoCoImports/..."
    double DefaultBaseHeightM = 0.0;
};

class FURSRobotTypeRegistry
{
public:
    static FURSRobotTypeRegistry& Get();
    void Register(const FURSRobotType& Type);
    void RegisterDefaultTypes();             // idempotent
    const FURSRobotType* Find(const FString& Name) const;
    TArray<FString> GetRegisteredNames() const;
};
}
```

### Registering a new robot type

In `Source/URSoccerLab/URSoccerLab.cpp`:

```cpp
void FURSoccerLabModule::StartupModule()
{
    URSoccerLab::FURSRobotTypeRegistry::Get().RegisterDefaultTypes();

    // Add a new type:
    URSoccerLab::FURSRobotType K1;
    K1.Name = TEXT("k1");
    K1.BlueprintAssetPath = TEXT("/Game/MuJoCoImports/k1_camera.k1_camera");
    K1.DefaultBaseHeightM = 0.35;
    URSoccerLab::FURSRobotTypeRegistry::Get().Register(K1);
}
```

Then reference it in the config:

```json
{ "actor_id": "robot_rp0", "type": "k1", "translation_m": [0, 0, 0.35] }
```

## Scene config component

`UURSSceneConfigComponent` lives on `AAMjManager`. It owns the config
file path, the spawn logic, and the initial-pose stash used by the admin
`reset` RPC.

### UPROPERTIES

| Property | Type | Default | Purpose |
| --- | --- | --- | --- |
| `ConfigPath` | `FString` | `"Config/URS_scene.json"` | Project-relative path to the JSON config |
| `bAutoApplyOnBeginPlay` | `bool` | `false` | If true, auto-applies on component BeginPlay. Off by default because the GameMode owns the ordering. |

### Blueprint-callable functions

| Function | Returns | Description |
| --- | --- | --- |
| `ApplyConfig(OutError)` | `bool` | Reload config from disk, destroy stale actors, spawn robots, fire `OnSceneConfigApplied`. |
| `ReloadConfig(OutError)` | `bool` | Reload config from disk into `ActiveConfig` without spawning. |
| `GetInitialPose(ActorId, OutTrans, OutRot)` | `bool` | Look up the spawn translation + rotation for a robot. Used by admin `reset`. |

### C++-only accessors

```cpp
const TMap<FString, FURSSpawnedRobotInfo>& GetSpawnedRobots() const;
const URSoccerLab::FURSSceneConfig& GetActiveConfig() const;
const TSet<FString>& GetKnownActorIds() const;
```

### Delegate

```cpp
// Fires after ApplyConfig completes successfully. The ZMQ bridge listens
// to this to pull RobotNames and rebind motor + admin sockets.
FOnSceneConfigApplied OnSceneConfigApplied;
```

### ApplyConfig behavior

1. `ReloadConfig` — parse JSON, validate (unique actor_ids, known types,
   finite translations).
2. Destroy actors whose `ActorId` is in the new config (idempotent
   re-spawn) or was spawned previously but is now absent from the config
   (stale cleanup across reloads).
3. For each robot in config:
   - Resolve type via `FURSRobotTypeRegistry::Find`.
   - Compute final translation (explicit or `[0, 0, DefaultBaseHeightM]`).
   - `LoadClass<AActor>(BlueprintAssetPath + "_C")`.
   - `World->SpawnActor<AMjArticulation>` with MJ→UE converted transform.
   - Set `ActorId`, rename actor, set label.
   - `ConfigureRobotCameras` — ZMQ endpoints, resolution, FOV, ray tracing.
   - `HideImportedFieldGeoms` — hide `floor`/`vision_floor`/`vision_marker` geoms.
   - Stash initial pose for admin `reset`.
4. Broadcast `OnSceneConfigApplied`.

## GameMode bootstrap

`AURSSoccerGameMode` is set as `WorldSettings::DefaultGameMode` in the
baked field level. Its `InitGame` override is the sole caller of
`ApplyConfig` at simulator startup.

```cpp
class AURSSoccerGameMode : public AGameModeBase
{
public:
    AURSSoccerGameMode();          // sets DefaultPawnClass = ASpectatorPawn

    virtual void InitGame(
        const FString& MapName, const FString& Options,
        FString& ErrorMessage) override;
};
```

The constructor sets `DefaultPawnClass = ASpectatorPawn` to suppress
UE's default `ADefaultPawn`, which spawns a visible Sphere mesh
(`/Engine/BasicShapes/Sphere`) at world origin with a null material.
In a headless simulator (`-game`) there is no human player, so the
default pawn would appear as an unwanted object in robot camera output.

### InitGame sequence

```
Super::InitGame                         // standard UE init
→ RegisterDefaultTypes()                // populate robot type registry
→ Find AAMjManager in world
→ Find UURSSceneConfigComponent on manager
→ SceneComp->ApplyConfig(Error)        // spawn robots
   ← fires OnSceneConfigApplied
   ← bridge pulls RobotNames, rebinds sockets
// ... later, World->BeginPlay fires ...
// AAMjManager::BeginPlay → Compile() discovers all articulations
// UURSZmqRobotBridgeComponent::BeginPlay → StartBridge()
```

## What goes in the `.umap`

Only static, non-robot content:

| Content | Source |
| --- | --- |
| Field geometry (static meshes) | `Assets/Scenes/SoccerField/source/field.glb` |
| Skylight | `URSSceneBakeLibrary::SpawnSpecifiedCubemapSkyLight` |
| `AAMjManager` | with `UURSZmqRobotBridgeComponent` + `UURSSceneConfigComponent` |
| `WorldSettings` | `DefaultGameMode = AURSSoccerGameMode` |

No `AMjArticulation` actors are baked. The bake fixture
(`URSoccerLab.E2E.CreateVisionSmokeMap`) imports the pi_plus Blueprint so
it is available for `LoadClass` at runtime, but does not place the robot
in the level.

## Rebuilding the field

The field geometry is baked from `Assets/Scenes/SoccerField/source/field.glb`
into `/Game/Levels/URS_SoccerField.umap` by the bake script
`Tools/ue_bake_soccer_field_scene.py` (executed inside UE Editor).

```bash
# Re-import field meshes from the GLB and rebuild the level
python3 Tools/create_soccer_field_scene.py --nullrhi

# Then re-run the vision smoke test (imports Blueprint, spawns manager, captures camera)
UV_CACHE_DIR=/tmp/uv-cache uv run python Tools/run_vision_smoke_test.py --out py_example/out/vision_smoke
```

The Interchange glTF importer (`bBakeMeshes = true` by default) bakes
each glTF scene-node's full transform — translation, rotation, scale,
and the 100x m-to-cm conversion — directly into mesh vertices. The bake
script therefore spawns every mesh actor at the origin with identity
rotation and unit scale so the node transform is not double-applied.

The `--skip-setup` flag on `run_vision_smoke_test.py` skips the
`CreateVisionSmokeMap` automation test (which re-imports the pi_plus
Blueprint, adds the manager + bridge, and saves the level) and launches
directly on the existing map.

## File layout

```
Config/
  URS_scene.json                 default scene config (two robots facing)
  URS_two_robot_scene.json       alternate config (same layout, kept for reference)
  DefaultEngine.ini              GlobalDefaultGameMode = AURSSoccerGameMode

Assets/
  MosBrainCameraTest/
    pi_plus/
      pi_plus_stereo_camera.xml  source MJCF (fixed-base, no freejoint)
  Scenes/
    SoccerField/
      source/field.glb           field geometry

Source/URSoccerLab/
  Public/
    URSSoccerGameMode.h
    Scene/
      URSSceneConfig.h
      URSSceneConfigComponent.h
      URSRobotTypeRegistry.h
  Private/
    URSSoccerGameMode.cpp
    Scene/
      URSSceneConfig.cpp
      URSSceneConfigComponent.cpp
      URSRobotTypeRegistry.cpp
```
