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

After importing an environment GLB whose lamp meshes use a non-zero glTF
`EmissiveFactor`, create one movable point light per disconnected physical lamp
volume and save the field level:

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
written under the ignored `Saved/Diagnostics/` directory. The old
`tune_environment_lighting.py` tool is retained as a historical tuning
reference. It expects an atmospheric sun to already exist and is not the
inverse of the indoor cleanup tool.

## Runtime diagnostics

Run the end-to-end vision smoke test:

```bash
uv run --project py_example python Tools/runtime/run_vision_smoke_test.py
```

Select and verify the camera wire encoding with `--camera-compress`. JPEG
quality is configurable; `raw` sends uncompressed BGRA:

```bash
uv run --project py_example python Tools/runtime/run_vision_smoke_test.py \
  --camera-compress jpeg --jpeg-quality 85 \
  --out py_example/out/vision_jpeg_q85

uv run --project py_example python Tools/runtime/run_vision_smoke_test.py \
  --camera-compress raw \
  --out py_example/out/vision_raw
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

`run_scene.py` always starts offscreen (`-RenderOffscreen`).

Use `benchmark_match_vision.py` for an end-to-end multi-robot measurement. It
uses the same generated nDisplay atlas and connects one client per robot.

## Robot assets

The source-of-truth robot layout and MJCF naming rules are documented in
[`Assets/Robots/README.md`](../Assets/Robots/README.md). A robot consists of one
MJCF XML plus a sibling `meshes/` directory; Unreal visual meshes are referenced
by empty `visual__<name>` frames in that XML.

Import or refresh the baked Unreal assets with:

```bash
UnrealEditor-Cmd URSoccerLab.uproject \
  -ExecutePythonScript="$PWD/Tools/editor/import_robot.py" \
  -NullRHI -Unattended -NoSplash -DDC-ForceMemoryCache
```

The importer places the generated Blueprint at
`/Game/URSoccerLab/Robots/pi_plus/pi_plus` and its visual meshes in the sibling
`Meshes/` directory. Validate both source and baked assets with:

```bash
python3 Tools/editor/validate_baked_assets.py
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
