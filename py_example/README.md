# Simulator examples

This directory contains example controllers, scene JSON files and policy fixtures.
Reusable Python/C++ connector code and Python wheel builds have moved to the
[`main-cli` branch](https://github.com/LuoShaoyinn/URSoccerLab/tree/main-cli).
See the [Python connector guide](https://github.com/LuoShaoyinn/URSoccerLab/blob/main-cli/python/README.md)
for API details.

Use Python 3.12 and the shipped wheel; do not install this directory as a package:

```bash
python -m pip install ursoccerlab_client-0.1.1-py3-none-any.whl
```

Start the AppImage with the example's scene in one terminal. In a second terminal,
using the environment with the wheel installed:

```bash
python py_example/examples/standing/standing.py --port 10000 10001 --duration 5
python py_example/examples/move_head/move_head.py --port 10000 10001 --duration 10
python py_example/examples/inspector/receive.py --host 127.0.0.1 --port 12000 --duration 10
```

Scenes reference external robot packages and field textures, which must exist on
the local machine. Walking/dribbling examples additionally need their policy
files and ML dependencies. Install ONNX Runtime/Ultralytics or a suitable PyTorch
build separately as required by the example. Those dependencies are not needed
for guest viewing or basic robot control. Outputs belong in `artifacts/`.

The simulator accepts only the scene JSON argument. See
[Getting started](../docs/Getting_Started.md) for installation and resources.
