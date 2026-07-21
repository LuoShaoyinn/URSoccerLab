#!/usr/bin/env python3
"""Create the reusable UE soccer-field scene map."""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_UE = Path("/home/luoshaoyinn/software/Unreal_Engine_5.7.4/Engine/Binaries/Linux/UnrealEditor")
PROJECT = ROOT / "URSoccerLab.uproject"
LOG_PATH = ROOT / "Saved" / "Logs" / "URS_CreateSoccerFieldScene.log"
BAKE_SCRIPT = ROOT / "Tools" / "ue_bake_soccer_field_scene.py"
SUCCESS_MARKER = ROOT / "Saved" / "Logs" / "URS_CreateSoccerFieldScene.done"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ue", type=Path, default=DEFAULT_UE)
    parser.add_argument("--nullrhi", action="store_true")
    args = parser.parse_args()

    if not args.ue.exists():
        raise FileNotFoundError(args.ue)
    if not PROJECT.exists():
        raise FileNotFoundError(PROJECT)
    if not BAKE_SCRIPT.exists():
        raise FileNotFoundError(BAKE_SCRIPT)

    cmd = [
        str(args.ue),
        str(PROJECT),
        "-DDC-ForceMemoryCache",
        "-unattended",
        "-nop4",
        "-nosplash",
        f"-ExecutePythonScript={BAKE_SCRIPT}",
    ]
    if args.nullrhi:
        cmd.insert(2, "-NullRHI")

    print("+", " ".join(cmd), flush=True)
    LOG_PATH.parent.mkdir(parents=True, exist_ok=True)
    SUCCESS_MARKER.unlink(missing_ok=True)
    with LOG_PATH.open("w", encoding="utf-8") as log:
        proc = subprocess.run(cmd, cwd=ROOT, text=True, stdout=log, stderr=subprocess.STDOUT)
    if proc.returncode != 0:
        raise RuntimeError(f"scene creation failed with exit code {proc.returncode}. See {LOG_PATH}")
    if not SUCCESS_MARKER.exists():
        raise RuntimeError(f"scene creation did not write success marker. See {LOG_PATH}")

    print("created /Game/Levels/URS_SoccerField")
    print(f"log: {LOG_PATH}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
