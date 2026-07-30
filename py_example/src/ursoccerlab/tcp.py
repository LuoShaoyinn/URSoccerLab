#!/usr/bin/env python3
"""Reusable URSoccerLab TCP protocol client.

Frame format:  [4-byte BE length][1-byte type][payload]
  type=0x00: JSON (state, command, admin)
  type=0x01: RGB image message
  type=0x02: Depth image message
"""
from __future__ import annotations

import json
import socket
import struct
import time
from collections.abc import Generator


TYPE_JSON = 0x00
TYPE_RGB = 0x01
TYPE_DEPTH = 0x02
# Compatibility name for the pre-v2 packed camera message.
TYPE_CAMERA = TYPE_RGB

CODEC_RAW = 0x00
CODEC_JPEG = 0x01
CODEC_ZLIB = 0x02

PIXEL_BGRA8 = 0x00
PIXEL_DEPTH_F32_M = 0x01
PIXEL_DEPTH_U16_MM = 0x02

IMAGE_MESSAGE_VERSION = 0x02

_CODEC_NAMES = {
    CODEC_RAW: "raw",
    CODEC_JPEG: "jpeg",
    CODEC_ZLIB: "zlib",
}
_PIXEL_FORMAT_NAMES = {
    PIXEL_BGRA8: "bgra8",
    PIXEL_DEPTH_F32_M: "depth_f32_m",
    PIXEL_DEPTH_U16_MM: "depth_u16_mm",
}


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
    """Parse a legacy packed multi-camera frame.

    Format: [codec][num_cameras][sim_time LE float64] per-cam: [width LE16][height LE16][data_len LE32][data]
    All cameras in one message are from the same physics step (synchronized).
    """
    if len(payload) < 10:
        raise ValueError("legacy camera payload is shorter than its header")
    codec = "jpeg" if payload[0] == CODEC_JPEG else "raw"
    num_cams = payload[1]
    sim_time = struct.unpack("<d", payload[2:10])[0]
    offset = 10
    cameras = []
    for i in range(num_cams):
        if offset + 8 > len(payload):
            raise ValueError(f"legacy camera entry {i} has a truncated header")
        width = payload[offset] | (payload[offset + 1] << 8)
        height = payload[offset + 2] | (payload[offset + 3] << 8)
        data_len = struct.unpack("<I", payload[offset + 4:offset + 8])[0]
        if offset + 8 + data_len > len(payload):
            raise ValueError(f"legacy camera entry {i} has a truncated payload")
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
    if offset != len(payload):
        raise ValueError("legacy camera payload has trailing bytes")
    return cameras


def parse_image_message(payload: bytes) -> list[dict]:
    """Parse a version-2 RGB or depth image message.

    Header:
      ``version:u8, count:u8, flags:u16, sequence:u32, sim_time:f64``

    Each entry is:
      ``name_len:u8, name:utf8, codec:u8, pixel_format:u8, reserved:u8,``
      ``width:u16, height:u16, uncompressed_len:u32, data_len:u32, data``
    """
    if len(payload) < 16:
        raise ValueError("image payload is shorter than its v2 header")
    version, count, _flags, sequence, sim_time = struct.unpack_from(
        "<BBHId", payload, 0
    )
    if version != IMAGE_MESSAGE_VERSION:
        raise ValueError(f"unsupported image message version: {version}")

    offset = 16
    entries: list[dict] = []
    for index in range(count):
        if offset >= len(payload):
            raise ValueError(f"image entry {index} has no name length")
        name_len = payload[offset]
        offset += 1
        fixed_len = 15
        if offset + name_len + fixed_len > len(payload):
            raise ValueError(f"image entry {index} has a truncated header")
        try:
            camera_name = payload[offset:offset + name_len].decode("utf-8")
        except UnicodeDecodeError as exc:
            raise ValueError(f"image entry {index} has an invalid UTF-8 name") from exc
        offset += name_len

        codec_id, pixel_id, _reserved, width, height, raw_len, data_len = (
            struct.unpack_from("<BBBHHII", payload, offset)
        )
        offset += fixed_len
        if codec_id not in _CODEC_NAMES:
            raise ValueError(f"image entry {index} has unknown codec {codec_id}")
        if pixel_id not in _PIXEL_FORMAT_NAMES:
            raise ValueError(
                f"image entry {index} has unknown pixel format {pixel_id}"
            )
        if offset + data_len > len(payload):
            raise ValueError(f"image entry {index} has a truncated payload")

        entries.append({
            "cam_index": index,
            "camera_name": camera_name,
            "codec": _CODEC_NAMES[codec_id],
            "pixel_format": _PIXEL_FORMAT_NAMES[pixel_id],
            "sequence": sequence,
            "sim_time": sim_time,
            "width": width,
            "height": height,
            "uncompressed_len": raw_len,
            "data": payload[offset:offset + data_len],
        })
        offset += data_len

    if offset != len(payload):
        raise ValueError("image payload has trailing bytes")
    return entries


class RobotClient:
    """Per-robot TCP client.  Command in, state + camera out."""

    def __init__(self, host: str, port: int = 10000):
        self.conn = FrameConn(host, port)

    def send_command(self, named_values: dict[str, float]):
        self.conn.send_json(named_values)

    def recv(self):
        """Yield ``(kind, data)`` tuples for state, RGB, depth, or legacy camera."""
        for ftype, payload in self.conn.recv_frames():
            if ftype == TYPE_JSON:
                yield "state", json.loads(payload.decode("utf-8"))
            elif ftype == TYPE_RGB:
                if payload and payload[0] == IMAGE_MESSAGE_VERSION:
                    yield "rgb", parse_image_message(payload)
                else:
                    yield "camera", parse_camera(payload)
            elif ftype == TYPE_DEPTH:
                yield "depth", parse_image_message(payload)

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
