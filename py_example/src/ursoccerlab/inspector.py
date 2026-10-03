"""Client for the inspector v1 endpoint."""
from __future__ import annotations

import json
import math

from .tcp import FrameConn, TYPE_JSON, TYPE_RGB, parse_image_message


class InspectorClient:
    """Own camera controls and RGB reception; no robot/admin commands."""

    def __init__(self, host: str = "127.0.0.1", port: int = 12000):
        self.conn = FrameConn(host, port)

    def set_camera(self, translation_m, rotation_quat_xyzw):
        """Set an absolute MuJoCo-world pose: local +X forward, +Z up."""
        position = tuple(float(v) for v in translation_m)
        rotation = tuple(float(v) for v in rotation_quat_xyzw)
        if len(position) != 3 or len(rotation) != 4:
            raise ValueError("camera pose requires 3 position and 4 quaternion values")
        if not all(math.isfinite(v) for v in (*position, *rotation)):
            raise ValueError("camera pose must be finite")
        if any(abs(v) > 100 for v in position):
            raise ValueError("camera position must be within +/-100 metres")
        scale = max(abs(v) for v in rotation)
        if scale < 1e-12:
            raise ValueError("camera quaternion must be nonzero")
        rotation = tuple(v / scale for v in rotation)
        norm = math.hypot(*rotation)
        if norm < 1e-12:
            raise ValueError("camera quaternion must be nonzero")
        self.conn.send_json({
            "version": 1,
            "command": "set_camera",
            "args": {
                "translation_m": list(position),
                "rotation_quat_xyzw": [v / norm for v in rotation],
            },
        })

    def recv(self):
        """Yield ('status', JSON) or ('rgb', image dictionaries); polls once."""
        for kind, payload in self.conn.recv_frames():
            if kind == TYPE_JSON:
                yield "status", json.loads(payload.decode("utf-8"))
            elif kind == TYPE_RGB:
                images = parse_image_message(payload)
                if any(image["pixel_format"] != "bgra8" for image in images):
                    raise ValueError("inspector RGB must use BGRA8 pixel format")
                yield "rgb", images
            else:
                raise ValueError(f"unexpected inspector message type: {kind}")

    @property
    def alive(self):
        return self.conn.alive

    def close(self):
        self.conn.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
