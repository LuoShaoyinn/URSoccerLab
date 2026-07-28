#!/usr/bin/env python3
"""Run the URSoccerLab admin RPC smoke test end-to-end.

Boots the tracked URS_SoccerField map and runs
``Tools/runtime/admin_smoke_client.py`` against ``robot_rp0``. Fails unless both
the set_pose and reset RPCs return ``ok: true``.
"""

from __future__ import annotations

import argparse
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_UE = Path("/home/luoshaoyinn/software/Unreal_Engine_5.7.4/Engine/Binaries/Linux/UnrealEditor")
PROJECT = ROOT / "URSoccerLab.uproject"
MAP_PATH = "/Game/Levels/URS_SoccerField"


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

    thread = threading.Thread(target=run, name="urs-admin-log-drain", daemon=True)
    thread.start()
    return ready, thread


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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ue", type=Path, default=DEFAULT_UE)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--robot", default="robot_rp0")
    parser.add_argument("--timeout-ms", type=int, default=30000)
    parser.add_argument("--scene-config", type=Path, default=ROOT / "Config" / "URS_scene.json")
    parser.add_argument("--sim-extra-arg", action="append", default=[])
    parser.add_argument("--render-warmup-sec", type=float, default=2.0)
    args = parser.parse_args()

    if not args.ue.exists():
        raise FileNotFoundError(args.ue)

    if not args.scene_config.exists():
        raise FileNotFoundError(args.scene_config)
    run_checked([sys.executable, str(ROOT / "Tools" / "editor" / "validate_baked_assets.py")], ROOT)

    sim = start_simulator(
        args.ue,
        [*args.sim_extra_arg, f"-URSSceneConfig={args.scene_config.resolve()}"],
    )
    sim_log_path = ROOT / "Saved" / "Logs" / "URS_AdminSmokeRuntime.log"
    sim_log_path.parent.mkdir(parents=True, exist_ok=True)
    sim_ready, log_thread = drain_process_log(
        sim,
        sim_log_path,
        ("[URS TCP] Admin listening",),
    )

    try:
        deadline = time.monotonic() + max(args.timeout_ms / 1000.0, 5.0)
        while time.monotonic() < deadline and not sim_ready.is_set():
            if sim.poll() is not None:
                raise RuntimeError(
                    f"simulator exited early with code {sim.returncode}. See {sim_log_path}"
                )
            time.sleep(0.1)

        if sim.poll() is not None:
            raise RuntimeError(
                f"simulator exited early with code {sim.returncode}. See {sim_log_path}"
            )
        if not sim_ready.is_set():
            raise RuntimeError(
                f"simulator did not expose admin RPC. See {sim_log_path}"
            )

        if args.render_warmup_sec > 0:
            time.sleep(args.render_warmup_sec)

        client_cmd = [
            "uv",
            "run",
            "python",
            str(ROOT / "Tools" / "runtime" / "admin_smoke_client.py"),
            "--host",
            args.host,
            "--robot",
            args.robot,
            "--timeout-ms",
            str(args.timeout_ms),
        ]
        result = subprocess.run(client_cmd, cwd=ROOT / "py_example", text=True, capture_output=True)
        print(result.stdout, end="")
        if result.stderr:
            print(result.stderr, end="", file=sys.stderr)
        if result.returncode != 0:
            raise RuntimeError(
                f"admin smoke client exited with {result.returncode}. See {sim_log_path}"
            )
    finally:
        terminate_process(sim)
        log_thread.join(timeout=2.0)

    print(f"admin smoke passed against {args.robot}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
