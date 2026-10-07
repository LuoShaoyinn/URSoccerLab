# URSoccerLab tools

Run all commands from the project root. `editor/` contains scripts that import
or modify tracked Unreal assets. `runtime/` contains launchers and TCP
diagnostics; runtime tools do not modify Unreal assets.

## Field assets

The authoritative visual scene is the tracked Unreal level
`Content/Levels/URS_SoccerField.umap` together with its assets under
`Content/URSoccerLab/Scenes/SoccerField/`. Edit and save these through Unreal
Editor. The original Blender/GLB import files are intentionally not retained.

After changing `Assets/Scenes/SoccerField/physics/field_physics.xml`, bake the
MuJoCo collision actor into the existing field level:

```bash
UnrealEditor URSoccerLab.uproject \
  -ExecutePythonScript=Tools/editor/bake_field_physics.py
```

## Environment lighting

The hall uses 21 separate emissive lamp meshes on lighting channel 0. Each
has six generated Lumen surface-cache cards; the original combined lamp mesh
had none. The original mesh asset is retained and its actor is hidden.
`Tools/editor/split_emissive_lamps.py` reproduces the split with native Geometry
Scripting (enable the plugin for the editor run). It bakes placement into
recentered meshes, preserves the lamp material, and disables the auxiliary
point lights. Geometry Scripting is only needed for this editor operation.

The current preset uses emissive lighting and fixed exposure compensation +2.5.
`Tools/editor/configure_hall_lighting.py` keeps the 21 auxiliary lights at zero.
The JSON `lamp_intensity_lumens` setting controls those auxiliary point lights,
not the emissive material. Set JSON `emissive_intensity` to the absolute linear
emission value (10 = authored hall value, 0 = off). `convert_emissive_lamps.py` is the older point-light
alternative, using 224 lumens per lamp. Separate lamp channels previously
produced dark patches; all lighting stays on channel 0.

```bash
UnrealEditor-Cmd URSoccerLab.uproject \
  -ExecutePythonScript="$PWD/Tools/editor/convert_emissive_lamps.py" \
  -NullRHI -unattended
```

The production field is indoor-only: its illumination comes from the movable
lamp lights and emissive lamp materials. Remove the complete outdoor stack
(Directional Light, Sky Light, Sky Atmosphere, Exponential Height Fog, and
Volumetric Cloud) and save that choice into the level with:

```bash
UnrealEditor-Cmd URSoccerLab.uproject \
  -ExecutePythonScript=Tools/editor/configure_indoor_production.py \
  -NullRHI -unattended
```

The cleanup and lamp-conversion operations are idempotent. Their reports are
written under the ignored `artifacts/diagnostics/` directory.

## Runtime diagnostics

Run the end-to-end vision smoke test:

```bash
uv run --project py_example python Tools/runtime/run_vision_smoke_test.py
```

This legacy smoke tool checks JPEG/raw diagnostics with `--camera-compress`;
`raw` sends uncompressed BGRA. For the production AV1 path, use
`Tools/runtime/test_av1.py` or the packaged smoke tool
`Tools/packaging/smoke_appimage.py`. JPEG quality is configurable:

```bash
uv run --project py_example python Tools/runtime/run_vision_smoke_test.py \
  --camera-compress jpeg --jpeg-quality 85 \
  --out artifacts/outputs/vision_jpeg_q85

uv run --project py_example python Tools/runtime/run_vision_smoke_test.py \
  --camera-compress raw \
  --out artifacts/outputs/vision_raw
```

While Unreal is serving camera frames, measure message rate, payload bandwidth,
frame intervals, and whether every message contains the expected cameras:

```bash
uv run --project py_example python \
  Tools/runtime/benchmark_camera_transport.py \
  --expected-cameras 2 --duration 15
```

Launch a complete scene with the production nDisplay atlas backend. All
runtime launchers read the `URS_UE` env var for the UnrealEditor binary (or
take `--ue`):

```bash
export URS_UE="$HOME/software/Unreal_Engine_5.7.4/Engine/Binaries/Linux/UnrealEditor"
uv run --project py_example python Tools/runtime/run_scene.py \
  --scene-config Config/examples/six_robots_stereo_rgb.json
```

`run_scene.py` always starts offscreen (`-RenderOffscreen`). Pass `--appimage
dist/URSoccerLab.AppImage` to launch the packaged AppImage (same nDisplay
atlas, no UnrealEditor needed) instead of the editor.

Use `benchmark_match_vision.py` for an end-to-end multi-robot measurement. It
uses the same generated nDisplay atlas and connects one client per robot.

## Packaging (AppImage)

Build through Docker with `python3 Tools/packaging/build_docker.py`; set `URS_UE`
to the Unreal Engine 5.7.4 directory. See [packaging instructions](packaging/README.md).
The package excludes robot assets, external field maps and ball texture overrides,
Python clients and developer tools. The default ball mesh and skin are bundled. GPU/Vulkan drivers come from the host.

Start with `./dist/URSoccerLab.AppImage scene.json`. The launcher accepts exactly
one JSON file and creates both robot and guest camera viewports automatically.
Runtime options belong in JSON. See [Getting started](../docs/Getting_Started.md).

## External robot packages

Robots load directly from external manifests, MJCF and GLBs. No robot editor
import or cooked Blueprint is needed. See [Robot packages](../docs/Robot_Packages.md).
`validate_baked_assets.py` checks only the hall, field and objects, and rejects
a restored cooked robot directory.

Validate the external packages and camera streams with:

```bash
PYTHONPATH=py_example/src py_example/.venv/bin/python Tools/runtime/test_external_robots.py
```

## Dynamic object assets

Dynamic non-robot articulations follow `Assets/Objects/README.md`. Refresh the
soccer-ball Blueprint and its embedded GLB materials with:

```bash
UnrealEditor-Cmd URSoccerLab.uproject \
  -ExecutePythonScript="$PWD/Tools/editor/import_object.py" \
  -NullRHI -Unattended -NoSplash -DDC-ForceMemoryCache
```

The object bake is intentionally clean-only. Before rebuilding an existing
object, move `Content/URSoccerLab/Objects/<object-type>/` outside the project
(for example into `/tmp`), then launch the command. This avoids Unreal's unsafe
in-process deletion of a loaded Blueprint and leaves a recoverable backup.

## External field and ball validation

Scene JSON must specify the field dimensions and external image. Source-runtime
rendering can be checked with:

```bash
py_example/.venv/bin/python Tools/runtime/test_change_map.py
```

This runs two external maps and field sizes, captures camera images, and writes
results under `artifacts/tests/change-map-render/`. It does not package an AppImage.
Unreal automation tests under `URSoccerLab.Scene.Config` validate required fields
and compile a configured ball through the component pipeline to verify its
radius, mass, inertia, friction, and contact settings. Goal tests additionally
require two explicit poses, verify the six static cylinders and their transforms,
and check ball–goalpost contact.

## Generated artifacts

Generated outputs live in the ignored project-root `artifacts/` directory:

| Directory | Contents |
| --- | --- |
| `tests/` | Test reports, fixtures, runtime logs and validation captures |
| `benchmarks/` | Benchmark results |
| `outputs/` | Example camera videos, images and traces |
| `runs/` | Local training outputs |
| `logs/`, `crashes/` | Unreal and launcher diagnostics |
| `diagnostics/`, `debug/` | Editor reports, shader debug files and material stats |
| `generated/` | Generated nDisplay layouts |
| `screenshots/`, `tmp/`, `archive/` | Screenshots, temporary test files and preserved cleanup archives |

Runtime tools and examples default to this tree regardless of their working
directory. Explicit output arguments still select the path supplied by the user.

For an existing or fresh checkout, run from the project root while the simulator
and output-producing jobs are stopped:

```sh
python Tools/runtime/organize_artifacts.py
```

This moves existing files without deleting them and creates relative compatibility
links for Unreal's fixed `Saved/` paths, `py_example/out`, and `runs`. Re-running is
safe; conflicting destinations are rejected rather than overwritten. The links
also keep older commands and saved absolute references usable. Model weights,
source assets, cooked distributions, and Unreal build/cache directories retain
their existing locations.
