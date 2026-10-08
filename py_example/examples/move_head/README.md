# move_head

Capture one or more standing robots while sweeping their head joints. All
non-head actuators receive `0` each frame; `head_yaw` / `head_pitch` actuators
are discovered dynamically (works with any robot type). On a two-robot scene,
the second robot's yaw is mirrored.

## Scene

`scene.json` — two `pi_plus` robots face to face. For a mos9 variant, point the
runtime at `Config/examples/mos9_face_to_face.json`.

## Run

Supply the external robot packages and field maps referenced by `scene.json`.
Start the packaged simulator from the project root:

```bash
./dist/URSoccerLab.AppImage py_example/examples/move_head/scene.json
```

Then run the client:

```bash
cd py_example
python examples/move_head/move_head.py --port 10000 10001 --duration 10 \
  --video ../artifacts/outputs/head_motion
```

For a single robot: `--port 10000`.

## Options

| flag | default | notes |
|------|---------|-------|
| `--port` | `10000` | one or more robot TCP ports |
| `--duration` | `10` | sweep length in seconds |
| `--mode` | `sweep` | `sweep` or `static` (no head commands) |
| `--cmd-hz` | `60` | command rate |
| `--video` | `../artifacts/outputs/head_demo` | video prefix; `_N.mp4` appended per robot when >1 |

## Output

One H.264 MP4 per robot under `../artifacts/outputs/`.
