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
  -run=PythonScript \
  -Script=Tools/editor/convert_emissive_lamps.py \
  -NullRHI -unattended
```

Apply the project’s overcast sky, fog, cloud, and sun defaults:

```bash
UnrealEditor-Cmd URSoccerLab.uproject \
  -run=PythonScript \
  -Script=Tools/editor/tune_environment_lighting.py \
  -NullRHI -unattended
```

Both operations are idempotent. Their tunable defaults live at the top of each
script, and their reports are written under the ignored
`Saved/Diagnostics/` directory. The resulting map, imported environment assets,
and cloud material are tracked with Git LFS.

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

## Robot assets

The source-of-truth robot layout and MJCF naming rules are documented in
[`Assets/Robots/README.md`](../Assets/Robots/README.md). A robot consists of one
MJCF XML plus a sibling `meshes/` directory; Unreal visual meshes are referenced
by empty `visual__<name>` frames in that XML.

Import or refresh the baked Unreal assets with:

```bash
UnrealEditor-Cmd URSoccerLab.uproject \
  -ExecutePythonScript=Tools/editor/import_robot.py \
  -NullRHI -Unattended -NoSplash -DDC-ForceMemoryCache
```

The importer places the generated Blueprint at
`/Game/URSoccerLab/Robots/pi_plus/pi_plus` and its visual meshes in the sibling
`Meshes/` directory. Validate both source and baked assets with:

```bash
python3 Tools/editor/validate_baked_assets.py
```
