#!/usr/bin/env python3
"""Launch a URSoccerLab scene with the production nDisplay vision backend."""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path

from ndisplay_config import write_ndisplay_config


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_UE = Path.home() / "Unreal_Engine_5.7.4/Engine/Binaries/Linux/UnrealEditor"
PROJECT = ROOT / "URSoccerLab.uproject"
MAP_PATH = "/Game/Levels/URS_SoccerField"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--scene-config",
        type=Path,
        default=ROOT / "Config/examples/six_robots_stereo_rgb.json",
    )
    parser.add_argument("--ue", type=Path, default=DEFAULT_UE)
    parser.add_argument("--map", default=MAP_PATH)
    parser.add_argument("--windowed", action="store_true")
    parser.add_argument(
        "--sim-extra-arg",
        action="append",
        default=[],
        help="Additional Unreal argument; may be repeated.",
    )
    args = parser.parse_args()

    scene_path = args.scene_config.resolve()
    config = json.loads(scene_path.read_text(encoding="utf-8"))
    mode = config.get("vision", {}).get("mode", "stereo_rgb")
    robot_count = len(config["robots"])
    rgb_view_count = robot_count * (2 if mode == "stereo_rgb" else 1)
    ndisplay_path = (
        ROOT / "Saved/Generated/NDisplay"
        / f"match_{rgb_view_count}_rgb.ndisplay"
    )
    width, height = write_ndisplay_config(rgb_view_count, ndisplay_path)

    command = [
        str(args.ue),
        str(PROJECT),
        args.map,
        "-game",
        "-ForceRes",
        f"-ResX={width}",
        f"-ResY={height}",
        "-dc_cluster",
        "-dc_dev_mono",
        f"-dc_cfg={ndisplay_path}",
        "-dc_node=node_0",
        "-URSNDisplayCameras",
        f"-URSNDisplayCameraCount={rgb_view_count}",
        "-ExecCmds=MjCamera.AutoReadback 0,DisableAllScreenMessages",
        f"-URSSceneConfig={scene_path}",
        "-NoSound",
        *([] if args.windowed else ["-RenderOffscreen"]),
        *args.sim_extra_arg,
    ]
    if mode == "rgbd":
        left_camera = config.get("vision", {}).get("left_camera", "left_eye")
        command.append(f"-URSNDisplayCameraName={left_camera}")

    print("+", " ".join(command), flush=True)
    return subprocess.run(command, cwd=ROOT, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
