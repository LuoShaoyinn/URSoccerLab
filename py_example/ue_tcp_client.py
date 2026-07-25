#!/usr/bin/env python3
"""URSoccerLab TCP protocol client.

Per-robot port (10000+i):
  Bidirectional, length-prefixed frames:
    [4-byte BE length][1-byte type][payload]
    type=0x00: JSON (command in, state out)
    type=0x01: Camera (binary, sim→client)

Admin port (11000):
  Same framing, all JSON request/reply:
    {"command":"set_pose","args":{"actor_id":"...","translation_m":[...],...}}
"""
import argparse
import json
import socket
import struct
import sys
import time
from io import BytesIO


class FrameConnection:
    def __init__(self, host: str, port: int):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock.connect((host, port))
        self.sock.settimeout(0.05)
        self._buf = bytearray()

    def _try_read(self):
        try:
            while True:
                chunk = self.sock.recv(65536)
                if not chunk:
                    return False
                self._buf.extend(chunk)
        except (BlockingIOError, socket.timeout):
            pass
        return True

    def recv_frames(self):
        alive = self._try_read()
        while len(self._buf) >= 5:
            frame_len = struct.unpack(">I", self._buf[:4])[0]
            if len(self._buf) < 4 + frame_len:
                break
            ftype = self._buf[4]
            payload = bytes(self._buf[5 : 4 + frame_len])
            del self._buf[: 4 + frame_len]
            yield ftype, payload
        if not alive and not self._buf:
            return

    def send_frame(self, ftype: int, payload: bytes):
        header = struct.pack(">IB", 1 + len(payload), ftype)
        self.sock.sendall(header + payload)

    def send_json(self, obj: dict):
        self.send_frame(0x00, json.dumps(obj, separators=(",", ":")).encode("utf-8"))

    def close(self):
        self.sock.close()


def parse_camera_frame(payload: bytes):
    codec = payload[0]
    flags = payload[1]
    width = payload[2] | (payload[3] << 8)
    height = payload[4] | (payload[5] << 8)
    data = payload[6:]
    return {
        "codec": "jpeg" if codec == 1 else "raw",
        "keyframe": bool(flags & 1),
        "width": width,
        "height": height,
        "data": data,
    }


def main():
    ap = argparse.ArgumentParser(description="URSoccerLab TCP test client")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--robot-port", type=int, default=10000)
    ap.add_argument("--admin-port", type=int, default=11000)
    ap.add_argument("--duration", type=float, default=5.0)
    ap.add_argument("--save-frames", type=int, default=3, help="Number of camera frames to save as PNG")
    args = ap.parse_args()

    print(f"[tcp] admin connect {args.host}:{args.admin_port}")
    admin = FrameConnection(args.host, args.admin_port)

    admin.send_json({"command": "get_pose", "args": {"actor_id": "robot_rp0"}})
    for ftype, payload in admin.recv_frames():
        reply = json.loads(payload.decode("utf-8"))
        print(f"[admin] get_pose: {json.dumps(reply, indent=2)[:500]}")
        break

    print(f"[tcp] robot connect {args.host}:{args.robot_port}")
    robot = FrameConnection(args.host, args.robot_port)

    robot.send_json({f"actuator_{i}": 0.0 for i in range(20)})

    state_count = 0
    camera_count = 0
    saved = 0
    start = time.time()

    try:
        while time.time() - start < args.duration:
            for ftype, payload in robot.recv_frames():
                if ftype == 0x00:
                    state = json.loads(payload.decode("utf-8"))
                    state_count += 1
                    if state_count <= 3 or state_count % 60 == 0:
                        joints_sample = list(state.get("joints", {}).items())[:2]
                        base = state.get("base", {})
                        print(f"[state #{state_count}] t={state.get('sim_time',0):.3f} "
                              f"base=({base.get('pos',[0,0,0])[0]:.2f},{base.get('pos',[0,0,0])[1]:.2f},{base.get('pos',[0,0,0])[2]:.2f}) "
                              f"joints={joints_sample}")
                elif ftype == 0x01:
                    cam = parse_camera_frame(payload)
                    camera_count += 1
                    if saved < args.save_frames:
                        try:
                            from PIL import Image
                            import io as _io
                            if cam["codec"] == "jpeg":
                                img = Image.open(_io.BytesIO(cam["data"]))
                            else:
                                img = Image.frombytes("RGBA", (cam["width"], cam["height"]),
                                                      bytes(cam["data"]), "raw", "BGRA")
                            fname = f"/tmp/opencode/urs_tcp_frame_{saved}.png"
                            img.save(fname)
                            print(f"[camera #{camera_count}] {cam['width']}x{cam['height']} {cam['codec']} "
                                  f"({len(cam['data'])} bytes) saved {fname}")
                            saved += 1
                        except ImportError:
                            print(f"[camera #{camera_count}] {cam['width']}x{cam['height']} {cam['codec']} "
                                  f"({len(cam['data'])} bytes)")

            time.sleep(0.001)
    except KeyboardInterrupt:
        pass

    elapsed = time.time() - start
    print(f"\n[tcp] {state_count} state frames, {camera_count} camera frames in {elapsed:.1f}s "
          f"({state_count/max(elapsed,0.001):.1f} Hz state, {camera_count/max(elapsed,0.001):.1f} Hz camera)")

    robot.close()
    admin.close()


if __name__ == "__main__":
    main()
