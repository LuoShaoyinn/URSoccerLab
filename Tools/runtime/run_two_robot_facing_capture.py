#!/usr/bin/env python3
"""Launch URS_SoccerField offscreen and capture one camera frame from each robot.

Robots are placed by Config/examples/two_robots_face_to_face.json:
  robot_rp0 at (-1, -0.5, 0.3762) facing +X (toward robot_rp1)
  robot_rp1 at ( 1, -0.5, 0.3762) facing -X (toward robot_rp0)

The ball remains at field origin, offset 0.5 m to the side of their sightline.

Output PNGs:
  py_example/out/two_robot_facing/robot_rp0_camera.png
  py_example/out/two_robot_facing/robot_rp1_camera.png
"""

from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import sys
import threading
import time
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_UE = Path(
    os.environ.get(
        "URS_UE",
        str(Path.home() / "Unreal_Engine_5.7.4/Engine/Binaries/Linux/UnrealEditor"),
    )
)
PROJECT = ROOT / "URSoccerLab.uproject"
MAP_PATH = "/Game/Levels/URS_SoccerField"
OUT_DIR = ROOT / "py_example" / "out" / "two_robot_facing"
DEFAULT_SCENE_CONFIG = ROOT / "Config" / "examples" / "two_robots_face_to_face.json"


def start_simulator(ue: Path, scene_config: Path) -> subprocess.Popen[str]:
    cmd = [
        str(ue),
        str(PROJECT),
        MAP_PATH,
        "-game",
        "-RenderOffscreen",
        "-DDC-ForceMemoryCache",
        "-unattended",
        "-nop4",
        "-nosplash",
        "-NoSound",
        f"-URSSceneConfig={scene_config.resolve()}",
    ]
    print("+", " ".join(cmd), flush=True)
    return subprocess.Popen(
        cmd,
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )


def terminate_process(proc: subprocess.Popen[str], timeout_sec: float = 10.0) -> None:
    if proc.poll() is not None:
        return
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=timeout_sec)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=timeout_sec)


def drain_process_log(
    proc: subprocess.Popen[str],
    log_path: Path,
    ready_markers: tuple[str, ...],
) -> tuple[threading.Event, threading.Thread]:
    ready = threading.Event()

    def run() -> None:
        with log_path.open("w", encoding="utf-8") as log:
            if not proc.stdout:
                return
            for line in proc.stdout:
                log.write(line)
                log.flush()
                if any(marker in line for marker in ready_markers):
                    ready.set()

    thread = threading.Thread(target=run, name="urs-two-robot-log-drain", daemon=True)
    thread.start()
    return ready, thread


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ue", type=Path, default=DEFAULT_UE)
    parser.add_argument("--timeout-ms", type=int, default=30000)
    parser.add_argument("--render-warmup-sec", type=float, default=3.0)
    parser.add_argument("--scene-config", type=Path, default=DEFAULT_SCENE_CONFIG)
    args = parser.parse_args()

    OUT_DIR.mkdir(parents=True, exist_ok=True)

    if not args.scene_config.exists():
        raise FileNotFoundError(args.scene_config)
    sim = start_simulator(args.ue, args.scene_config)
    sim_log_path = ROOT / "Saved" / "Logs" / "URS_TwoRobotFacingRuntime.log"
    sim_log_path.parent.mkdir(parents=True, exist_ok=True)
    sim_ready, log_thread = drain_process_log(
        sim,
        sim_log_path,
        ("[URS TCP] Transport started",),
    )

    captured: dict[str, Path] = {}
    try:
        deadline = time.monotonic() + max(args.timeout_ms / 1000.0, 5.0)
        while time.monotonic() < deadline and not sim_ready.is_set():
            if sim.poll() is not None:
                raise RuntimeError(f"simulator exited early with code {sim.returncode}. See {sim_log_path}")
            time.sleep(0.1)
        if not sim_ready.is_set():
            raise RuntimeError(f"simulator never reported admin RPC ready. See {sim_log_path}")

        if args.render_warmup_sec > 0:
            time.sleep(args.render_warmup_sec)

        for robot in ("robot_rp0", "robot_rp1"):
            out_dir = OUT_DIR / robot
            out_dir.mkdir(parents=True, exist_ok=True)
            client_cmd = [
                "uv",
                "run",
                "python",
                "examples/vision_smoke.py",
                "--host",
                "127.0.0.1",
                "--robot",
                robot,
                "--timeout-ms",
                str(args.timeout_ms),
                "--out",
                str(out_dir),
                "--camera-frame-count",
                "20",
            ]
            print("+", " ".join(client_cmd), flush=True)
            result = subprocess.run(
                client_cmd,
                cwd=ROOT / "py_example",
                text=True,
                capture_output=True,
            )
            print(result.stdout, end="")
            if result.stderr:
                print(result.stderr, end="", file=sys.stderr)
            if result.returncode != 0:
                raise RuntimeError(f"py_example failed for {robot}. See {sim_log_path}")

            camera_path = out_dir / "camera.png"
            if not camera_path.exists() or camera_path.stat().st_size <= 0:
                raise RuntimeError(f"camera.png missing for {robot}")
            captured[robot] = camera_path
    finally:
        terminate_process(sim)
        log_thread.join(timeout=2.0)

    print(json.dumps({k: str(v) for k, v in captured.items()}, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
