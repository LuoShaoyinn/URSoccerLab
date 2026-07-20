#!/usr/bin/env python3
"""Create the reusable UE soccer-field scene map.

This runs an editor-only automation command that imports the field GLB, places
the visual meshes, adds a default UE skylight, and saves
/Game/Levels/URS_SoccerField.
"""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_UE = Path("/home/luoshaoyinn/software/Unreal_Engine_5.7.4/Engine/Binaries/Linux/UnrealEditor")
PROJECT = ROOT / "URSoccerLab.uproject"
LOG_PATH = ROOT / "Saved" / "Logs" / "URS_CreateSoccerFieldScene.log"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ue", type=Path, default=DEFAULT_UE)
    args = parser.parse_args()

    if not args.ue.exists():
        raise FileNotFoundError(args.ue)
    if not PROJECT.exists():
        raise FileNotFoundError(PROJECT)

    cmd = [
        str(args.ue),
        str(PROJECT),
        "-NullRHI",
        "-DDC-ForceMemoryCache",
        "-unattended",
        "-nop4",
        "-nosplash",
        "-ExecCmds=Automation RunTests URSoccerLab.Scene.CreateSoccerField; Quit",
    ]

    print("+", " ".join(cmd), flush=True)
    LOG_PATH.parent.mkdir(parents=True, exist_ok=True)
    with LOG_PATH.open("w", encoding="utf-8") as log:
        proc = subprocess.run(cmd, cwd=ROOT, text=True, stdout=log, stderr=subprocess.STDOUT)
    if proc.returncode != 0:
        raise RuntimeError(f"scene creation failed with exit code {proc.returncode}. See {LOG_PATH}")

    print("created /Game/Levels/URS_SoccerField")
    print(f"log: {LOG_PATH}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
