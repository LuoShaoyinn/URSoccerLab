#!/usr/bin/env python3
"""Minimal TCP-based vision smoke client for URSoccerLab.

Connects to a robot TCP port, receives state + camera frames, and saves the
first camera frame as camera.png.
"""
from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

from PIL import Image

from ursoccerlab.media import camera_to_rgb
from ursoccerlab.tcp import RobotClient


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=10000)
    parser.add_argument("--robot", default="robot_rp0")
    parser.add_argument("--timeout-ms", type=int, default=30000)
    parser.add_argument("--out", type=Path, default=Path("out/vision_smoke"))
    parser.add_argument("--camera-frame-count", type=int, default=1)
    args = parser.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)

    client = RobotClient(args.host, args.port)
    deadline = time.monotonic() + args.timeout_ms / 1000.0
    cameras_saved = 0
    state_saved = False

    while time.monotonic() < deadline and cameras_saved < args.camera_frame_count:
        for kind, payload in client.recv():
            if kind == "state" and not state_saved:
                state = payload
                (args.out / "state.json").write_text(json.dumps(state, indent=2))
                state_saved = True
            elif kind == "camera":
                for camera in payload:
                    if camera["data"] and camera["width"] > 0 and camera["height"] > 0:
                        Image.fromarray(camera_to_rgb(camera)).save(args.out / "camera.png")
                        cameras_saved += 1

        time.sleep(0.001)

    client.close()

    if cameras_saved == 0:
        print(f"No camera frames received for {args.robot}")
        return 1

    print(f"vision smoke: saved {cameras_saved} camera frame(s) to {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
