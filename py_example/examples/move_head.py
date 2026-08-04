#!/usr/bin/env python3
"""Capture one or more standing robots, optionally sweeping their heads.

Works with any robot type (pi_plus, mos9, ...) — head actuators are discovered
dynamically from the state's ``actuators`` dict by substring-matching
``head_yaw`` / ``head_pitch``.  Actuator suffix (``_servo`` or ``_motor``)
does not matter.

All non-head actuators receive 0 each frame (holding the standing pose for
position-servo robots like pi_plus and mos9).

Connect to as many robots as the scene provides::

    # single robot
    uv run python examples/move_head.py --port 10000 --duration 10

    # two robots face-to-face
    uv run python examples/move_head.py --port 10000 10001 --duration 10
"""
from __future__ import annotations

import argparse
import json
import math
import time
from pathlib import Path

import numpy as np

from ursoccerlab.media import camera_to_rgb, write_video
from ursoccerlab.tcp import (
    IMAGE_MESSAGE_VERSION,
    FrameConn,
    TYPE_JSON,
    TYPE_RGB,
    parse_camera,
    parse_image_message,
)


class RobotConnection:
    """Per-robot TCP connection: motor commands out, state + camera in."""

    def __init__(self, host: str, port: int):
        self.conn = FrameConn(host, port)
        self.actuator_names: list[str] = []
        self.frames: list[np.ndarray] = []
        self.latest_z: float = 0.0
        self.latest_up: float = 0.0

    def send_command(self, cmd: dict[str, float]):
        self.conn.send_json(cmd)

    def pump(self):
        """Read available frames.  Learns actuator names from first state."""
        for ftype, payload in self.conn.receive_available():
            if ftype == TYPE_JSON:
                st = json.loads(payload.decode("utf-8"))
                if not self.actuator_names:
                    self.actuator_names = list(st.get("actuators", {}).keys())
                b = st.get("base", {})
                self.latest_z = b.get("pos", [0, 0, 0])[2]
                q = b.get("quat", [1, 0, 0, 0])
                self.latest_up = 1.0 - 2.0 * (q[1] ** 2 + q[2] ** 2)
            elif ftype == TYPE_RGB:
                cams = (
                    parse_image_message(payload)
                    if payload and payload[0] == IMAGE_MESSAGE_VERSION
                    else parse_camera(payload)
                )
                if not cams:
                    continue
                cam0 = cams[0]
                if not cam0["data"]:
                    continue
                self.frames.append(camera_to_rgb(cam0))

    def close(self):
        self.conn.close()


def main(default_mode: str = "sweep") -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, nargs="+", default=[10000],
                    help="one or more robot TCP ports")
    ap.add_argument("--duration", type=float, default=10.0)
    ap.add_argument("--mode", choices=("sweep", "static"), default=default_mode,
                    help="sweep head joints, or capture with no motor commands")
    ap.add_argument("--cmd-hz", type=float, default=60.0)
    ap.add_argument("--video-fps", type=int, default=15)
    ap.add_argument("--video", type=Path, default=Path("out/head_demo"),
                    help="output video prefix (one file per port appended with _N.mp4)")
    args = ap.parse_args()

    robots = [RobotConnection(args.host, p) for p in args.port]
    n = len(robots)

    # Wait for first state to learn actuator names
    print(f"[demo] waiting for state on {n} robot(s) ...", flush=True)
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        for r in robots:
            r.pump()
        if all(r.actuator_names for r in robots):
            break
        time.sleep(0.01)

    if not all(r.actuator_names for r in robots):
        missing = [args.port[i] for i, r in enumerate(robots) if not r.actuator_names]
        print(f"[demo] ERROR: no state on port(s) {missing}", flush=True)
        return 1

    for i, r in enumerate(robots):
        print(f"[demo] robot {i} (port {args.port[i]}): "
              f"{len(r.actuator_names)} actuators", flush=True)

    # Discover head actuators per robot
    head_names: list[tuple[str | None, str | None]] = []
    for r in robots:
        hy = next((a for a in r.actuator_names if "head_yaw" in a), None)
        hp = next((a for a in r.actuator_names if "head_pitch" in a), None)
        head_names.append((hy, hp))

    for i, (hy, hp) in enumerate(head_names):
        if args.mode == "sweep" and (not hy or not hp):
            print(f"[demo] WARNING: robot {i} has no head actuators; skipping sweep",
                  flush=True)

    print(f"[demo] {args.mode} capture for {args.duration:.0f}s ...", flush=True)
    interval = 1.0 / args.cmd_hz
    t0 = time.monotonic()
    next_cmd = t0
    step = 0

    while time.monotonic() - t0 < args.duration:
        now = time.monotonic()
        if now >= next_cmd:
            for r in robots:
                r.pump()

            t = (now - t0) / args.duration
            head_yaw = 0.0
            head_pitch = 0.0
            if args.mode == "sweep":
                head_yaw = 0.8 * math.sin(2.0 * math.pi * t)
                head_pitch = 0.4 * math.sin(4.0 * math.pi * t)

            for i, r in enumerate(robots):
                # Send 0 to all non-head actuators (hold standing pose)
                cmd: dict[str, float] = {}
                hy, hp = head_names[i]
                for a in r.actuator_names:
                    if a == hy or a == hp:
                        continue
                    cmd[a] = 0.0

                # Head sweep
                if hy and hp:
                    sign = 1.0 if i == 0 else -1.0
                    cmd[hy] = sign * head_yaw
                    cmd[hp] = head_pitch

                r.send_command(cmd)

            step += 1
            if step % 30 == 0:
                parts = [f"r{i}(z={r.latest_z:.2f} up={r.latest_up:.2f} "
                         f"frames={len(r.frames)})"
                         for i, r in enumerate(robots)]
                print(f"  t={t:.1f} yaw={math.degrees(head_yaw):+.0f}° "
                      f"pitch={math.degrees(head_pitch):+.0f}° "
                      f"{' '.join(parts)}", flush=True)
            next_cmd = now + interval
        else:
            for r in robots:
                r.pump()
            time.sleep(0.001)

    print("[demo] done, saving videos ...", flush=True)
    for r in robots:
        r.close()
    for i, r in enumerate(robots):
        out_path = args.video if n == 1 else args.video.with_name(
            f"{args.video.stem}_{i}{args.video.suffix}")
        write_video(r.frames, out_path, args.video_fps)
        print(f"  robot {i}: {out_path} ({len(r.frames)} frames)", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
