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

import numpy as np
from PIL import Image

from ursoccerlab.media import camera_to_rgb, depth_to_meters
from ursoccerlab.tcp import RobotClient


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=10000)
    parser.add_argument("--robot", default="robot_rp0")
    parser.add_argument("--timeout-ms", type=int, default=30000)
    parser.add_argument("--out", type=Path, default=Path("out/vision_smoke"))
    parser.add_argument("--camera-frame-count", type=int, default=1)
    parser.add_argument("--expected-codec", choices=("jpeg", "raw"))
    parser.add_argument("--expect-depth", action="store_true")
    args = parser.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)

    client = RobotClient(args.host, args.port)
    deadline = time.monotonic() + args.timeout_ms / 1000.0
    cameras_saved = 0
    state_saved = False
    image_saved = False
    codec_counts: dict[str, int] = {}
    payload_bytes = 0
    depth_result: dict[str, float | int | str] | None = None

    while (
        time.monotonic() < deadline
        and (
            cameras_saved < args.camera_frame_count
            or (args.expect_depth and depth_result is None)
        )
    ):
        for kind, payload in client.recv():
            if kind == "state" and not state_saved:
                state = payload
                (args.out / "state.json").write_text(json.dumps(state, indent=2))
                state_saved = True
            elif kind in ("rgb", "camera"):
                for camera in payload:
                    if camera["data"] and camera["width"] > 0 and camera["height"] > 0:
                        codec = str(camera["codec"])
                        if args.expected_codec and codec != args.expected_codec:
                            print(
                                f"Expected {args.expected_codec} camera data, received {codec}"
                            )
                            client.close()
                            return 1
                        if not image_saved:
                            Image.fromarray(camera_to_rgb(camera)).save(
                                args.out / "camera.png"
                            )
                            image_saved = True
                        codec_counts[codec] = codec_counts.get(codec, 0) + 1
                        payload_bytes += len(camera["data"])
                        cameras_saved += 1
            elif kind == "depth" and depth_result is None and payload:
                depth = payload[0]
                meters = depth_to_meters(depth)
                finite = meters[np.isfinite(meters)]
                if finite.size:
                    depth_result = {
                        "codec": str(depth["codec"]),
                        "pixel_format": str(depth["pixel_format"]),
                        "width": int(depth["width"]),
                        "height": int(depth["height"]),
                        "payload_bytes": len(depth["data"]),
                        "min_m": float(finite.min()),
                        "max_m": float(finite.max()),
                    }

        time.sleep(0.001)

    client.close()

    if cameras_saved == 0:
        print(f"No camera frames received for {args.robot}")
        return 1
    if args.expect_depth and depth_result is None:
        print(f"No depth frame received for {args.robot}")
        return 1

    result = {
        "camera_frames": cameras_saved,
        "codec_counts": codec_counts,
        "payload_bytes": payload_bytes,
        "mean_payload_bytes": payload_bytes / cameras_saved,
        "depth": depth_result,
    }
    (args.out / "transport.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n"
    )
    print(f"vision smoke: saved {cameras_saved} camera frame(s) to {args.out}")
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
