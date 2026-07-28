# URSoccerLab Tools

`editor/` contains scripts executed by Unreal Editor to import source assets
and save tracked runtime assets. `runtime/` contains headless launchers and
TCP smoke clients; they never modify UE assets.

Run a runtime smoke test from the project root:

```bash
uv run --project py_example python Tools/runtime/run_vision_smoke_test.py
```

Rebuild the field map only after changing `Assets/Scenes/SoccerField/source/field.glb`:

```bash
python3 Tools/editor/create_soccer_field_scene.py --nullrhi
```
