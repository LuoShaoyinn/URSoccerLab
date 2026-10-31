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
python standing/standing.py --port 10000 10001 --duration 5
python move_head/move_head.py --port 10000 10001 --duration 10
python inspector/receive.py --host 127.0.0.1 --port 12000 --duration 10
```

Scenes reference external robot packages and field textures, which must exist on
the local machine. Walking/dribbling examples additionally need their policy
files and ML dependencies. Install ONNX Runtime/Ultralytics or a suitable PyTorch
build separately as required by the example. Those dependencies are not needed
for guest viewing or basic robot control. Outputs belong in `artifacts/`.

The simulator accepts only the scene JSON argument. See
[Getting started](https://github.com/LuoShaoyinn/URSoccerLab/blob/main/docs/Getting_Started.md) for installation and resources.

Each example has its own `pyproject.toml`. From an example directory, run `uv sync`, then install the distributed client wheel into that environment with `uv pip install /path/to/ursoccerlab_client-0.1.1-py3-none-any.whl`. Run the commands below from the repository root with the corresponding example environment active.

The K1 checkpoint is used by the MOS-Brain MuJoCo reference in `refs/mos-brain/simulation/mujoco`: install its `requirements.txt` and run `python sim2sim_runner.py --robot-type k1 --team-size 3` from that directory. Its `app/runtime_config.py` defines the joint order, gains, observation scales and 50 Hz control settings. The URSoccerLab adaptation is in `walk/k1_walk`; see its README for usage and validation limits.

Walking examples are grouped in `walk/`: `k1_walk`, `pi_walk`, and `mos9_walk`.

Example scenes set `command_timeout_sec` to 1.0 second to tolerate brief command interruptions. This requires the updated simulator release; on timeout, position-mode targets become zero.
