#!/usr/bin/env python3
"""Capture one or more standing robots, optionally sweeping their heads.

Works with any robot type (pi_plus, mos9, ...) — head actuators are discovered
dynamically from the state's ``actuators`` dict by substring-matching
``head_yaw`` / ``head_pitch``.  All non-head actuators receive 0 each frame
(holding the standing pose for position-servo robots).

Connect to as many robots as the scene provides::

    # single robot
    uv run python examples/move_head.py --port 10000 --duration 10

    # two robots face-to-face
    uv run python examples/move_head.py --port 10000 10001 --duration 10
"""
from __future__ import annotations

import argparse
import math
import time
from pathlib import Path

import numpy as np

from ursoccerlab.media import camera_to_rgb, write_video
from ursoccerlab.tcp import RobotClient


def main(default_mode: str = "sweep") -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, nargs="+", default=[10000],
                    help="one or more robot TCP ports")
    ap.add_argument("--duration", type=float, default=10.0)
    ap.add_argument("--mode", choices=("sweep", "static"), default=default_mode,
                    help="sweep head joints, or capture with no motor commands")
    ap.add_argument("--cmd-hz", type=float, default=60.0)
    ap.add_argument("--video-fps", type=int, default=30)
    ap.add_argument("--video", type=Path, default=Path("out/head_demo"),
                    help="output video prefix (one file per robot appended with _N.mp4)")
    args = ap.parse_args()

    clients = [RobotClient(args.host, p) for p in args.port]
    n = len(clients)
    actuator_sets: list[set[str]] = [set() for _ in range(n)]
    head_names: list[tuple[str | None, str | None]] = [(None, None)] * n
    latest_states: list[dict | None] = [None] * n
    all_frames: list[list[np.ndarray]] = [[] for _ in range(n)]

    def pump(i: int) -> None:
        for kind, data in clients[i].recv():
            if kind == "state":
                latest_states[i] = data
                if not actuator_sets[i]:
                    actuator_sets[i] = set(data.get("actuators", {}))
                    hy = next((a for a in actuator_sets[i] if "head_yaw" in a), None)
                    hp = next((a for a in actuator_sets[i] if "head_pitch" in a), None)
                    head_names[i] = (hy, hp)
            elif kind in ("rgb", "camera"):
                cam = data[0]
                if cam.get("data"):
                    all_frames[i].append(camera_to_rgb(cam))

    # Wait for first state on all robots
    print(f"[demo] waiting for state on {n} robot(s) ...", flush=True)
    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline:
        for i in range(n):
            pump(i)
        if all(latest_states[i] for i in range(n)):
            break
        time.sleep(0.01)

    if not all(latest_states[i] for i in range(n)):
        missing = [args.port[i] for i in range(n) if not latest_states[i]]
        print(f"[demo] ERROR: no state on port(s) {missing}", flush=True)
        return 1

    for i in range(n):
        print(f"[demo] robot {i} (port {args.port[i]}): "
              f"{len(actuator_sets[i])} actuators", flush=True)
        if args.mode == "sweep":
            hy, hp = head_names[i]
            if not hy or not hp:
                print(f"[demo] WARNING: robot {i} has no head actuators; skipping sweep",
                      flush=True)

    print(f"[demo] {args.mode} capture for {args.duration:.0f}s ...", flush=True)
    interval = 1.0 / args.cmd_hz
    t0 = time.monotonic()
    next_cmd = t0
    step = 0

    while time.monotonic() - t0 < args.duration:
        now = time.monotonic()
        for i in range(n):
            pump(i)

        if now >= next_cmd:
            t = (now - t0) / args.duration
            head_yaw = 0.0
            head_pitch = 0.0
            if args.mode == "sweep":
                head_yaw = 0.8 * math.sin(2.0 * math.pi * t)
                head_pitch = 0.4 * math.sin(4.0 * math.pi * t)

            for i in range(n):
                hy, hp = head_names[i]
                cmd: dict[str, float] = {}
                for a in actuator_sets[i]:
                    if a == hy or a == hp:
                        continue
                    cmd[a] = 0.0
                if hy and hp:
                    sign = 1.0 if i == 0 else -1.0
                    cmd[hy] = sign * head_yaw
                    cmd[hp] = head_pitch
                clients[i].send_command(cmd)

            step += 1
            if step % 30 == 0:
                parts = []
                for i in range(n):
                    st = latest_states[i] or {}
                    b = st.get("base", {})
                    z = b.get("pos", [0, 0, 0])[2]
                    q = b.get("quat", [1, 0, 0, 0])
                    up = 1.0 - 2.0 * (q[1] ** 2 + q[2] ** 2)
                    parts.append(f"r{i}(z={z:.2f} up={up:.2f} frames={len(all_frames[i])})")
                print(f"  t={t:.1f} yaw={math.degrees(head_yaw):+.0f}° "
                      f"pitch={math.degrees(head_pitch):+.0f}° "
                      f"{' '.join(parts)}", flush=True)
            next_cmd = now + interval
        else:
            time.sleep(0.001)

    print("[demo] done, saving videos ...", flush=True)
    for c in clients:
        c.close()
    for i in range(n):
        out_path = args.video if n == 1 else args.video.with_name(
            f"{args.video.stem}_{i}{args.video.suffix}")
        write_video(all_frames[i], out_path, args.video_fps)
        print(f"  robot {i}: {out_path} ({len(all_frames[i])} frames)", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
