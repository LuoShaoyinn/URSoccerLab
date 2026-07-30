#!/usr/bin/env python3
"""Reusable URSoccerLab TCP protocol client.

Frame format:  [4-byte BE length][1-byte type][payload]
  type=0x00: JSON (state, command, admin)
  type=0x01: Camera (binary: codec + flags + width + height + pixels)
"""
from __future__ import annotations

import json
import socket
import struct
import time
from collections.abc import Generator


TYPE_JSON = 0x00
TYPE_CAMERA = 0x01

CODEC_RAW = 0x00
CODEC_JPEG = 0x01


class FrameConn:
    """Low-level length-prefixed frame connection."""

    def __init__(self, host: str, port: int, timeout: float = 3.0):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock.settimeout(timeout)
        self.sock.connect((host, port))
        self.sock.settimeout(0)  # non-blocking after connect
        self._buf = bytearray()
        self._alive = True

    def send_frame(self, ftype: int, payload: bytes):
        header = struct.pack(">IB", 1 + len(payload), ftype)
        self.sock.sendall(header + payload)

    def send_json(self, obj: dict):
        self.send_frame(TYPE_JSON, json.dumps(obj, separators=(",", ":")).encode("utf-8"))

    def _try_read(self):
        if not self._alive:
            return False
        try:
            for _ in range(8):
                chunk = self.sock.recv(131072)
                if not chunk:
                    self._alive = False
                    return False
                self._buf.extend(chunk)
        except (BlockingIOError, socket.timeout):
            pass
        except OSError:
            self._alive = False
            return False
        return True

    def recv_frames(self) -> Generator[tuple[int, bytes], None, None]:
        self._try_read()
        while len(self._buf) >= 5:
            frame_len = struct.unpack(">I", self._buf[:4])[0]
            if len(self._buf) < 4 + frame_len:
                break
            ftype = self._buf[4]
            payload = bytes(self._buf[5 : 4 + frame_len])
            del self._buf[: 4 + frame_len]
            yield ftype, payload

    def receive_available(self) -> list[tuple[int, bytes]]:
        """Return all complete frames currently available without blocking."""
        return list(self.recv_frames())

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass
        self._alive = False

    @property
    def alive(self) -> bool:
        return self._alive


def parse_camera(payload: bytes) -> list[dict]:
    """Parse packed multi-camera frame. Returns list of camera dicts.

    Format: [codec][num_cameras][sim_time LE float64] per-cam: [width LE16][height LE16][data_len LE32][data]
    All cameras in one message are from the same physics step (synchronized).
    """
    codec = "jpeg" if payload[0] == CODEC_JPEG else "raw"
    num_cams = payload[1]
    sim_time = struct.unpack("<d", payload[2:10])[0]
    offset = 10
    cameras = []
    for i in range(num_cams):
        width = payload[offset] | (payload[offset + 1] << 8)
        height = payload[offset + 2] | (payload[offset + 3] << 8)
        data_len = struct.unpack("<I", payload[offset + 4:offset + 8])[0]
        data = payload[offset + 8:offset + 8 + data_len]
        cameras.append({
            "cam_index": i,
            "codec": codec,
            "sim_time": sim_time,
            "width": width,
            "height": height,
            "data": data,
        })
        offset += 8 + data_len
    return cameras


class RobotClient:
    """Per-robot TCP client.  Command in, state + camera out."""

    def __init__(self, host: str, port: int = 10000):
        self.conn = FrameConn(host, port)

    def send_command(self, named_values: dict[str, float]):
        self.conn.send_json(named_values)

    def recv(self):
        """Yield (kind, data) tuples: kind is 'state' (dict) or 'camera' (dict)."""
        for ftype, payload in self.conn.recv_frames():
            if ftype == TYPE_JSON:
                yield "state", json.loads(payload.decode("utf-8"))
            elif ftype == TYPE_CAMERA:
                yield "camera", parse_camera(payload)

    def close(self):
        self.conn.close()


class AdminClient:
    """Global admin TCP client."""

    def __init__(self, host: str, port: int = 11000):
        self.conn = FrameConn(host, port)

    def _request(self, command: str, args: dict) -> dict:
        self.conn.send_json({"command": command, "args": args})
        deadline = time.time() + 3.0
        while time.time() < deadline:
            for ftype, payload in self.conn.recv_frames():
                if ftype == TYPE_JSON:
                    return json.loads(payload.decode("utf-8"))
            time.sleep(0.001)
        raise TimeoutError(f"admin request '{command}' timed out")

    def set_pose(self, actor_id: str, translation_m=None, rotation_quat_xyzw=None, joint_qpos=None):
        args: dict = {"actor_id": actor_id}
        if translation_m is not None:
            args["translation_m"] = list(translation_m)
        if rotation_quat_xyzw is not None:
            args["rotation_quat_xyzw"] = list(rotation_quat_xyzw)
        if joint_qpos is not None:
            args["joint_qpos"] = list(joint_qpos)
        return self._request("set_pose", args)

    def get_pose(self, actor_id: str) -> dict:
        return self._request("get_pose", {"actor_id": actor_id})

    def reset(self, actor_id: str) -> dict:
        return self._request("reset", {"actor_id": actor_id})

    def lock_pose(self, actor_id: str, translation_m=None, rotation_quat_xyzw=None, joint_qpos=None):
        args: dict = {"actor_id": actor_id}
        if translation_m is not None: args["translation_m"] = list(translation_m)
        if rotation_quat_xyzw is not None: args["rotation_quat_xyzw"] = list(rotation_quat_xyzw)
        if joint_qpos is not None: args["joint_qpos"] = list(joint_qpos)
        return self._request("lock_pose", args)

    def unlock_pose(self, actor_id: str) -> dict:
        return self._request("unlock_pose", {"actor_id": actor_id})

    def close(self):
        self.conn.close()
