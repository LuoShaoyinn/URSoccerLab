#!/usr/bin/env python3
"""Capture one or more standing robots without sweeping their heads.

All actuators (head included) are held at 0 each frame so position-servo robots
keep their configured pose. Connect to as many robots as the scene provides::

    uv run python examples/standing/standing.py --port 10000 10001 --duration 5

Run Unreal with this folder's ``scene.json``.
"""
from __future__ import annotations

import argparse
import time
from pathlib import Path

import numpy as np

from ursoccerlab.media import camera_to_rgb, write_video
from ursoccerlab.tcp import RobotClient


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, nargs="+", default=[10000], help="one or more robot TCP ports")
    parser.add_argument("--duration", type=float, default=5.0)
    parser.add_argument("--cmd-hz", type=float, default=60.0)
    parser.add_argument("--video-fps", type=int, default=30)
    parser.add_argument("--video", type=Path, default=Path("out/standing"),
                        help="output video path or prefix (.mp4 always applied; _N appended per robot when >1)")
    args = parser.parse_args()

    clients = [RobotClient(args.host, p) for p in args.port]
    n = len(clients)
    actuator_sets: list[list[str]] = [[] for _ in range(n)]
    latest_states: list[dict | None] = [None] * n
    all_frames: list[list[np.ndarray]] = [[] for _ in range(n)]

    def pump(i: int) -> None:
        for kind, data in clients[i].recv():
            if kind == "state":
                latest_states[i] = data
                if not actuator_sets[i]:
                    actuator_sets[i] = list(data.get("actuators", {}).keys())
            elif kind in ("rgb", "camera"):
                cam = data[0]
                if cam.get("data"):
                    all_frames[i].append(camera_to_rgb(cam))

    print(f"[standing] waiting for state on {n} robot(s) ...", flush=True)
    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline:
        for i in range(n):
            pump(i)
        if all(latest_states[i] for i in range(n)):
            break
        time.sleep(0.01)
    if not all(latest_states[i] for i in range(n)):
        missing = [args.port[i] for i in range(n) if not latest_states[i]]
        print(f"[standing] ERROR: no state on port(s) {missing}", flush=True)
        return 1

    for i in range(n):
        print(f"[standing] robot {i} (port {args.port[i]}): {len(actuator_sets[i])} actuators", flush=True)

    print(f"[standing] static capture for {args.duration:.0f}s ...", flush=True)
    interval = 1.0 / args.cmd_hz
    t0 = time.monotonic()
    next_cmd = t0
    while time.monotonic() - t0 < args.duration:
        now = time.monotonic()
        for i in range(n):
            pump(i)
        if now >= next_cmd:
            for i in range(n):
                clients[i].send_command({a: 0.0 for a in actuator_sets[i]})
            next_cmd = now + interval
        else:
            time.sleep(0.001)

    for c in clients:
        c.close()
    print("[standing] done, saving videos ...", flush=True)
    for i in range(n):
        out_path = args.video.parent / (
            args.video.stem if n == 1 else f"{args.video.stem}_{i}"
        )
        out_path = out_path.with_suffix(".mp4")
        if all_frames[i]:
            write_video(all_frames[i], out_path, args.video_fps)
            print(f"  robot {i}: {out_path} ({len(all_frames[i])} frames)", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
