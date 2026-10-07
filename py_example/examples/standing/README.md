# standing

Capture one or more standing robots without sweeping their heads. Every
actuator is held at `0` each frame so position-servo robots keep their
configured pose. Useful as a minimal "is the simulator + camera path alive?"
smoke test.

## Scene

`scene.json` — two `pi_plus` robots (`robot_rp0`, `robot_rp1`) placed face to
face. `robot_rp0` listens on port `10000`, `robot_rp1` on `10001`.

## Run

Supply the external robot packages and field maps referenced by `scene.json`.
Start the packaged simulator from the project root:

```bash
./dist/URSoccerLab.AppImage py_example/examples/standing/scene.json
```

Then run the client:

```bash
cd py_example
uv run python examples/standing/standing.py --port 10000 10001 --duration 5 \
  --video ../artifacts/outputs/standing
```

## Options

| flag | default | notes |
|------|---------|-------|
| `--port` | `10000` | one or more robot TCP ports (nargs `+`) |
| `--duration` | `5` | capture length in seconds |
| `--cmd-hz` | `60` | actuator-hold command rate |
| `--video` | `../artifacts/outputs/standing` | video prefix; `_N.mp4` appended per robot when >1 |

## Output

One H.264 MP4 per robot under `../artifacts/outputs/` (left-eye camera).
