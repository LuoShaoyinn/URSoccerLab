#!/usr/bin/env python3
"""Run a URSoccerLab experiment against the production nDisplay sim, guaranteeing
the simulator stops when the experiment ends.

The wrapper starts the simulator in its own OS process group, waits for the TCP
transport to come up, runs the supplied client command, and then kills the sim's
whole process group. The sim is killed on:

  - normal client return,
  - client failure / non-zero exit,
  - this wrapper being interrupted (SIGINT) or terminated (SIGTERM), and
  - a hard wall-clock cap (``--sim-timeout``), which fires even if the wrapper
    itself is killed before it can clean up (the sim is launched under
    ``timeout(1)`` as a safety net).

Usage (everything after ``--`` is the client command):

    uv run --project py_example python Tools/runtime/run_with_sim.py \
        --ue '$HOME/software/Unreal_Engine_5.7.4/Engine/Binaries/Linux/UnrealEditor' \
        --scene-config Config/examples/walker_and_observer.json \
        -- python examples/walk_policy.py --vx 0.35 --duration 15 \
             --video out/walker.mp4 --observer-video out/observer.mp4

Run from the project root. Relative client paths resolve from the repo root.
"""

from __future__ import annotations

import argparse
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

from ndisplay_config import write_ndisplay_config


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_UE = Path(
    os.environ.get(
        "URS_UE",
        str(Path.home() / "Unreal_Engine_5.7.4/Engine/Binaries/Linux/UnrealEditor"),
    )
)
PROJECT = ROOT / "URSoccerLab.uproject"
MAP_PATH = "/Game/Levels/URS_SoccerField"
NDISPLAY_DIR = ROOT / "Saved/Generated/NDisplay"
READY_MARKER = "Robot 'robot_rp0' listening on port"


def _read_scene(scene_config: Path) -> dict:
    import json
    return json.loads(scene_config.read_text(encoding="utf-8"))


def _build_sim_command(args: argparse.Namespace, ndisplay_path: Path) -> list[str]:
    config = _read_scene(args.scene_config)
    mode = config.get("vision", {}).get("mode", "stereo_rgb")
    robot_count = len(config["robots"])
    view_count = robot_count * (2 if mode == "stereo_rgb" else 1)

    command = [
        "timeout", "--signal=KILL", "--kill-after=5", str(args.sim_timeout),
        str(args.ue),
        str(PROJECT),
        MAP_PATH,
        "-game",
        "-ForceRes",
        f"-ResX={args.res_x}",
        f"-ResY={args.res_y}",
        "-dc_cluster",
        "-dc_dev_mono",
        f"-dc_cfg={ndisplay_path}",
        "-dc_node=node_0",
        "-URSNDisplayCameras",
        f"-URSNDisplayCameraCount={view_count}",
        "-ExecCmds=MjCamera.AutoReadback 0,DisableAllScreenMessages",
        f"-URSSceneConfig={args.scene_config}",
        "-NoSound",
        "-RenderOffscreen",
        *args.sim_extra_arg,
    ]
    if mode == "rgbd":
        left_camera = config.get("vision", {}).get("left_camera", "left_eye")
        command.append(f"-URSNDisplayCameraName={left_camera}")
    return command


def _wait_for_ready(log_path: Path, marker: str, deadline: float) -> bool:
    while time.time() < deadline:
        try:
            text = log_path.read_text(errors="ignore")
        except FileNotFoundError:
            text = ""
        if marker in text:
            return True
        time.sleep(0.5)
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ue", type=Path, default=DEFAULT_UE)
    parser.add_argument("--scene-config", type=Path, required=True)
    parser.add_argument("--sim-timeout", type=float, default=240.0,
                        help="wall-clock cap after which the sim is force-killed")
    parser.add_argument("--ready-timeout", type=float, default=180.0,
                        help="how long to wait for the TCP transport to come up")
    parser.add_argument("--res-x", type=int, default=1920)
    parser.add_argument("--res-y", type=int, default=960)
    parser.add_argument("--sim-extra-arg", action="append", default=[])
    parser.add_argument("client", nargs=argparse.REMAINDER,
                        help="client command (precede with '--')")
    args = parser.parse_args()
    if args.client and args.client[0] == "--":
        args.client = args.client[1:]
    if not args.client:
        parser.error("a client command is required (use '--' then the command)")

    import json
    config = _read_scene(args.scene_config)
    mode = config.get("vision", {}).get("mode", "stereo_rgb")
    view_count = len(config["robots"]) * (2 if mode == "stereo_rgb" else 1)
    NDISPLAY_DIR.mkdir(parents=True, exist_ok=True)
    ndisplay_path = NDISPLAY_DIR / f"match_{view_count}_rgb.ndisplay"
    write_ndisplay_config(view_count, ndisplay_path)

    sim_cmd = _build_sim_command(args, ndisplay_path)
    log_path = ROOT / "Saved/Logs/run_with_sim.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_file = log_path.open("w")

    print("+", " ".join(map(str, sim_cmd)), flush=True)
    sim = subprocess.Popen(
        sim_cmd,
        cwd=ROOT,
        stdout=log_file,
        stderr=subprocess.STDOUT,
        stdin=subprocess.DEVNULL,
        start_new_session=True,  # own process group -> killable as a unit
    )

    def kill_sim() -> None:
        try:
            os.killpg(os.getpgid(sim.pid), signal.SIGKILL)
        except ProcessLookupError:
            pass
        except Exception:
            try:
                sim.kill()
            except Exception:
                pass

    def on_signal(signum, _frame):
        kill_sim()
        sys.exit(128 + signum)

    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)
    import atexit
    atexit.register(kill_sim)

    try:
        robot_count = len(config["robots"])
        last_port = 10000 + robot_count - 1
        ready_marker = f"listening on port {last_port}"
        ready_deadline = time.time() + args.ready_timeout
        if not _wait_for_ready(log_path, ready_marker, ready_deadline):
            print(f"simulator did not signal '{ready_marker}' within "
                  f"{args.ready_timeout:.0f}s", file=sys.stderr)
            return 2
        print(f"[run_with_sim] transport ready (sim pgid {os.getpgid(sim.pid)})", flush=True)

        client = subprocess.run(args.client, cwd=ROOT)
        return client.returncode
    finally:
        kill_sim()


if __name__ == "__main__":
    raise SystemExit(main())
