#!/usr/bin/env python3
"""Minimal TCP-based vision smoke client for URSoccerLab.

Connects to a robot TCP port, receives state + camera frames, and saves the
first camera frame as camera.png.
"""
from __future__ import annotations

import argparse
import io
import json
import socket
import struct
import time
from pathlib import Path

from PIL import Image


TYPE_JSON = 0x00
TYPE_CAMERA = 0x01


def recv_all(sock: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        try:
            chunk = sock.recv(n - len(buf))
        except (BlockingIOError, socket.timeout):
            break
        if not chunk:
            break
        buf.extend(chunk)
    return bytes(buf)


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

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5.0)
    sock.connect((args.host, args.port))
    sock.settimeout(0.1)

    buf = bytearray()
    deadline = time.monotonic() + args.timeout_ms / 1000.0
    cameras_saved = 0
    state_saved = False

    while time.monotonic() < deadline and cameras_saved < args.camera_frame_count:
        try:
            chunk = sock.recv(131072)
            if not chunk:
                break
            buf.extend(chunk)
        except (BlockingIOError, socket.timeout):
            pass

        while len(buf) >= 5:
            frame_len = struct.unpack(">I", buf[:4])[0]
            if len(buf) < 4 + frame_len:
                break
            ftype = buf[4]
            payload = bytes(buf[5 : 4 + frame_len])
            del buf[: 4 + frame_len]

            if ftype == TYPE_JSON and not state_saved:
                state = json.loads(payload.decode("utf-8"))
                (args.out / "state.json").write_text(json.dumps(state, indent=2))
                state_saved = True

            elif ftype == TYPE_CAMERA:
                if len(payload) < 10:
                    continue
                codec = payload[0]
                num_cams = payload[1]
                offset = 10  # skip codec + num_cams + 8-byte sim_time
                for i in range(num_cams):
                    if offset + 8 > len(payload):
                        break
                    width = payload[offset] | (payload[offset + 1] << 8)
                    height = payload[offset + 2] | (payload[offset + 3] << 8)
                    data_len = struct.unpack("<I", payload[offset + 4 : offset + 8])[0]
                    cam_data = payload[offset + 8 : offset + 8 + data_len]
                    offset += 8 + data_len

                    if cam_data and width > 0 and height > 0:
                        if codec == 1:
                            img = Image.open(io.BytesIO(cam_data)).convert("RGB")
                        else:
                            img = Image.frombytes("RGBA", (width, height), cam_data).convert("RGB")
                        img.save(args.out / "camera.png")
                        cameras_saved += 1

        time.sleep(0.001)

    sock.close()

    if cameras_saved == 0:
        print(f"No camera frames received for {args.robot}")
        return 1

    print(f"vision smoke: saved {cameras_saved} camera frame(s) to {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
