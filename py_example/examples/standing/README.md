# standing

Capture one or more standing robots without sweeping their heads. Every
actuator is held at `0` each frame so position-servo robots keep their
configured pose. Useful as a minimal "is the simulator + camera path alive?"
smoke test.

## Scene

`scene.json` — two `pi_plus` robots (`robot_rp0`, `robot_rp1`) placed face to
face. `robot_rp0` listens on port `10000`, `robot_rp1` on `10001`.

## Run

Start the simulator offscreen in a separate terminal (project root):

```bash
uv run --project py_example python Tools/runtime/run_scene.py \
  --scene-config py_example/examples/standing/scene.json
```

Then run the client:

```bash
cd py_example
uv run python examples/standing/standing.py --port 10000 10001 --duration 5 \
  --video out/standing
```

## Options

| flag | default | notes |
|------|---------|-------|
| `--port` | `10000` | one or more robot TCP ports (nargs `+`) |
| `--duration` | `5` | capture length in seconds |
| `--cmd-hz` | `60` | actuator-hold command rate |
| `--video` | `out/standing` | video prefix; `_N.mp4` appended per robot when >1 |

## Output

One H.264 MP4 per robot under `out/` (left-eye camera).
