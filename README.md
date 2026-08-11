# URSoccerLab

URSoccerLab couples MuJoCo robot physics with Unreal Engine rendering. MuJoCo
owns dynamics, contacts, joints, and actuators; Unreal Engine 5.7 owns the
indoor scene, robot visuals, lighting, and camera output. The default runtime
spawns the scene from JSON and exposes one bidirectional TCP connection per
robot.

## Runtime architecture

```text
Config/URS_scene.json
        |
        v
GameMode -> robot/object registries -> baked Unreal Blueprints
        |                                  |
        v                                  v
URLab MuJoCo physics thread ------> coherent render snapshot ------> Unreal cameras
        ^                                                           |
        |                                                           v
motor JSON <----- one TCP port per robot ---- state + RGB/depth frames
```

Physics and rendering are deliberately decoupled. URLab steps MuJoCo on its
physics thread and publishes a coherent snapshot for Unreal. Camera rendering,
image compression, or a slow client may skip an output opportunity, but does
not stall the integrator or grow an unbounded queue.

The default vision mode is stereo RGB: two 640x480 images per robot at 30 Hz,
JPEG quality 85. State is published independently at 60 Hz. Robot `i` uses TCP
port `10000 + i`; the optional global administration API uses port `11000`.
URSoccerLab disables URLab's legacy ZMQ, shared-memory, and RPC listeners.

## Repository layout

| Path | Purpose |
| --- | --- |
| `Assets/` | Authoritative MJCF and GLB source assets |
| `Content/` | Baked Unreal assets and the production `.umap` (Git LFS) |
| `Config/` | Default and example runtime scene configurations |
| `Source/URSoccerLab/` | Project runtime, scene, transport, and tests |
| `Plugins/UnrealRoboticsLab/` | Pinned URLab submodule |
| `py_example/` | Python 3.12 client library and runnable examples |
| `Tools/editor/` | Unreal asset-import and scene-building utilities |
| `Tools/runtime/` | Launchers, smoke tests, and profiling tools |
| `Tools/packaging/` | AppImage cook + packaging pipeline |
| `dist/` | Built AppImage (gitignored) |
| `docs/` | Architecture and protocol references |

`Assets` and `Content` have different roles: source MJCF/GLB files are edited
under `Assets`; Unreal imports them into tracked `.uasset`/`.umap` files under
`Content`. The full background scene is authored directly in the production
level and its referenced Content assets. MuJoCo uses a separate flat-plane
field MJCF and never simulates the detailed background geometry.

## Prerequisites

- Linux with Vulkan-capable graphics drivers
- Unreal Engine 5.7 (the project was developed with 5.7.4)
- Git LFS and initialized Git submodules
- [`uv`](https://docs.astral.sh/uv/) for the Python examples

Fetch a fresh checkout completely:

```bash
git lfs install
git submodule update --init --recursive
git lfs pull
```

Build the editor target:

```bash
UE_ROOT=/path/to/Unreal_Engine_5.7.4
"$UE_ROOT/Engine/Build/BatchFiles/Linux/Build.sh" \
  URSoccerLabEditor Linux Development "$PWD/URSoccerLab.uproject" -WaitMutex
```

## Quick start (AppImage)

The prebuilt AppImage bundles the cooked simulator (binary + assets + MuJoCo /
ZMQ / CoACD + the C++ runtime). No Unreal Engine installation needed — only a
Vulkan-capable GPU (AMD or NVIDIA) and FUSE on the host. GPU/Vulkan drivers are
provided by the host and intentionally not bundled, so one image runs on both
vendors.

```bash
# Terminal 1 — start the simulator (headless; nDisplay atlas auto-enabled):
./dist/URSoccerLab.AppImage py_example/examples/standing/scene.json

# Terminal 2 — run a Python client
cd py_example && uv sync && uv run python examples/standing/standing.py \
  --port 10000 --duration 5
```

The positional `<scene.json>` is **required**. The AppRun parses it, generates a
tightly-packed nDisplay camera atlas (`view_count = robots × cameras`), and
injects every runtime flag (`-URSSceneConfig=`, `-dc_cluster -dc_cfg=...
-dc_node=node_0`, `-RenderOffscreen`, `-NoSound`, camera readback). Pass
`-URSSceneCapture` to force the lower-throughput per-camera fallback, or append
any extra UE flags after the scene. On a host without libfuse2 (e.g. a stock
Ubuntu 22.04), install `libfuse2` or append `--appimage-extract-and-run`. See
[`Tools/packaging/README.md`](Tools/packaging/README.md) to rebuild the AppImage
from source.

## Run a scene (from source)

```bash
export URS_UE=/path/to/Unreal_Engine_5.7.4/Engine/Binaries/Linux/UnrealEditor
uv run --project py_example python Tools/runtime/run_scene.py \
  --scene-config py_example/examples/move_head/scene.json
```

## Python examples

```bash
cd py_example
uv sync
uv run python examples/standing/standing.py --port 10000 --duration 5
uv run python examples/move_head/move_head.py --port 10000 --duration 10
```

Walking examples need one mutually exclusive PyTorch backend:

```bash
uv sync --extra torch_rocm   # or torch_cpu / torch_cuda
uv run --extra torch_rocm python examples/pi_walk/pi_walk.py --duration 15
uv run python examples/mos9_walk/mos9_walk.py --vx 0.4 --duration 15
```

See [the Python guide](py_example/README.md) for scene selection, output files,
controller parameters (PD gains / actuator mode), and client API examples.

## Validation

Run the Python protocol tests:

```bash
uv run --project py_example python -m unittest discover -s py_example/tests
```

Run the Unreal automation suite after building:

```bash
"$UE_ROOT/Engine/Binaries/Linux/UnrealEditor-Cmd" \
  "$PWD/URSoccerLab.uproject" -NullRHI -unattended -nop4 -nosplash \
  -ExecCmds="Automation RunTests URSoccerLab.; Quit"
```

Validate the source-to-baked asset contract with:

```bash
python3 Tools/editor/validate_baked_assets.py
```

## Documentation

Start with [the documentation index](docs/README.md). Asset-specific
conventions live beside their sources in
[Assets/Robots/README.md](Assets/Robots/README.md) and
[Assets/Objects/README.md](Assets/Objects/README.md).

## License

Original URSoccerLab work is available under the
[Apache License, Version 2.0](LICENSE). Please retain the attribution in
[NOTICE](NOTICE) when redistributing it.

Unreal Engine, the forked UnrealRoboticsLab plugin, bundled libraries, and
imported assets may have separate terms. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and the license files shipped
with those components.
