#!/usr/bin/env python3
"""Capture one full-field image from a running URSoccerLab guest inspector."""

from __future__ import annotations

import argparse
import math
import time
from pathlib import Path

from PIL import Image

from ursoccerlab import InspectorClient
from ursoccerlab.media import camera_to_rgb


def look_rotation(position: tuple[float, float, float],
                  target: tuple[float, float, float]) -> tuple[float, float, float, float]:
    """Quaternion for a camera whose local +X points forward and +Z points up."""
    dx, dy, dz = (target[i] - position[i] for i in range(3))
    yaw = math.atan2(dy, dx)
    pitch = math.atan2(-dz, math.hypot(dx, dy))
    sy, cy = math.sin(yaw * 0.5), math.cos(yaw * 0.5)
    sp, cp = math.sin(pitch * 0.5), math.cos(pitch * 0.5)
    return (-sy * sp, cy * sp, sy * cp, cy * cp)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=12000)
    parser.add_argument("--timeout", type=float, default=30.0,
                        help="seconds to wait for a rendered frame")
    parser.add_argument("--position", nargs=3, type=float, default=(-5.7, -8.0, 6.5),
                        metavar=("X", "Y", "Z"), help="camera position in metres")
    parser.add_argument("--target", nargs=3, type=float, default=(0.0, 0.0, 0.0),
                        metavar=("X", "Y", "Z"), help="world point the camera looks at")
    parser.add_argument("--out", type=Path, default=Path("artifacts/outputs/field_preview.png"))
    args = parser.parse_args()

    position = tuple(args.position)
    target = tuple(args.target)
    rotation = look_rotation(position, target)
    out_path = args.out.expanduser().resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)

    try:
        with InspectorClient(args.host, args.port) as inspector:
            inspector.set_camera(position, rotation)
            deadline = time.monotonic() + args.timeout
            while time.monotonic() < deadline:
                for kind, payload in inspector.recv():
                    if kind == "status" and not payload.get("ok", False):
                        raise RuntimeError(f"inspector rejected camera pose: {payload}")
                    if kind == "rgb" and payload:
                        Image.fromarray(camera_to_rgb(payload[0])).save(out_path)
                        print(f"Saved full-field preview: {out_path}")
                        return 0
                if not inspector.alive:
                    raise ConnectionError("guest inspector disconnected")
                time.sleep(0.01)
    except OSError as exc:
        raise SystemExit(
            f"Cannot connect to guest inspector at {args.host}:{args.port}. "
            "Start the simulator and check guest_inspector.enabled/port first."
        ) from exc

    raise SystemExit(
        f"No camera frame arrived within {args.timeout:g}s. "
        "Check that the simulator is rendering and guest_inspector is enabled."
    )


if __name__ == "__main__":
    raise SystemExit(main())
