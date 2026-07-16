#!/usr/bin/env python3
"""Run the URSoccerLab end-to-end vision smoke test.

The test creates/refreshes a UE fixture map, starts that map offscreen, runs the
Python zero-command client, and requires that a camera PNG is produced.
"""

from __future__ import annotations

import argparse
import json
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_UE = Path("/home/luoshaoyinn/software/Unreal_Engine_5.7.4/Engine/Binaries/Linux/UnrealEditor")
PROJECT = ROOT / "URSoccerLab.uproject"
MAP_PATH = "/Game/Levels/URS_VisionSmoke"


def run_checked(cmd: list[str], cwd: Path, log_path: Path | None = None) -> None:
    print("+", " ".join(cmd), flush=True)
    if log_path:
        log_path.parent.mkdir(parents=True, exist_ok=True)
        with log_path.open("w", encoding="utf-8") as log:
            proc = subprocess.run(cmd, cwd=cwd, text=True, stdout=log, stderr=subprocess.STDOUT)
    else:
        proc = subprocess.run(cmd, cwd=cwd)
    if proc.returncode != 0:
        suffix = f" See {log_path}" if log_path else ""
        raise RuntimeError(f"command failed with exit code {proc.returncode}.{suffix}")


def start_simulator(ue: Path, extra_args: list[str]) -> subprocess.Popen[str]:
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
        *extra_args,
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

    thread = threading.Thread(target=run, name="urs-ue-log-drain", daemon=True)
    thread.start()
    return ready, thread


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ue", type=Path, default=DEFAULT_UE)
    parser.add_argument("--robot", default="robot_rp0")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--timeout-ms", type=int, default=30000)
    parser.add_argument("--out", type=Path, default=ROOT / "py_example" / "out" / "vision_smoke")
    parser.add_argument("--skip-setup", action="store_true")
    parser.add_argument("--sim-extra-arg", action="append", default=[])
    args = parser.parse_args()

    if not args.ue.exists():
        raise FileNotFoundError(args.ue)

    if not args.skip_setup:
        run_checked(
            [
                str(args.ue),
                str(PROJECT),
                "-NullRHI",
                "-DDC-ForceMemoryCache",
                "-unattended",
                "-nop4",
                "-nosplash",
                '-ExecCmds=Automation RunTests URSoccerLab.E2E.CreateVisionSmokeMap; Quit',
            ],
            ROOT,
            ROOT / "Saved" / "Logs" / "URS_VisionSmokeSetup.log",
        )

    run_checked(["uv", "sync"], ROOT / "py_example")

    args.out.mkdir(parents=True, exist_ok=True)
    sim = start_simulator(args.ue, args.sim_extra_arg)
    sim_log_path = ROOT / "Saved" / "Logs" / "URS_VisionSmokeRuntime.log"
    sim_log_path.parent.mkdir(parents=True, exist_ok=True)
    sim_ready, log_thread = drain_process_log(
        sim,
        sim_log_path,
        (
            "URSoccerLab ZMQ bridge started",
        ),
    )

    try:
        deadline = time.monotonic() + max(args.timeout_ms / 1000.0, 5.0)
        while time.monotonic() < deadline and not sim_ready.is_set():
            if sim.poll() is not None:
                raise RuntimeError(f"simulator exited early with code {sim.returncode}. See {sim_log_path}")
            time.sleep(0.1)

        if sim.poll() is not None:
            raise RuntimeError(f"simulator exited early with code {sim.returncode}. See {sim_log_path}")
        if not sim_ready.is_set():
            raise RuntimeError(f"simulator did not start the URSoccerLab ZMQ bridge. See {sim_log_path}")

        result = subprocess.run(
            [
                "uv",
                "run",
                "python",
                "main.py",
                "--host",
                args.host,
                "--robot",
                args.robot,
                "--timeout-ms",
                str(args.timeout_ms),
                "--out",
                str(args.out),
            ],
            cwd=ROOT / "py_example",
            text=True,
            capture_output=True,
        )
        print(result.stdout, end="")
        if result.stderr:
            print(result.stderr, end="", file=sys.stderr)
        if result.returncode != 0:
            raise RuntimeError(f"py_example failed with exit code {result.returncode}. See {sim_log_path}")
    finally:
        terminate_process(sim)
        log_thread.join(timeout=2.0)

    camera_path = args.out / "camera.png"
    if not camera_path.exists() or camera_path.stat().st_size <= 0:
        meta_path = args.out / "meta.json"
        meta = json.loads(meta_path.read_text()) if meta_path.exists() else {}
        raise RuntimeError(f"camera.png was not produced. Metadata cameras={meta.get('cameras')!r}")

    print(f"vision smoke passed: {camera_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
