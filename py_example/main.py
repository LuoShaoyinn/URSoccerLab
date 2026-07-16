from __future__ import annotations

import argparse
import json
import struct
import time
from pathlib import Path
from typing import Any

import zmq
from PIL import Image


MAGIC = 0x4D535255
VERSION = 1


def client_endpoint(bind_endpoint: str, host: str) -> str:
    return bind_endpoint.replace("tcp://0.0.0.0:", f"tcp://{host}:").replace(
        "tcp://*:", f"tcp://{host}:"
    )


def encode_zero_command(sequence: int, motor_count: int) -> bytes:
    header = struct.pack("<IHHQdI", MAGIC, VERSION, 0, sequence, time.time(), motor_count)
    body = struct.pack(f"<{motor_count}f", *([0.0] * motor_count)) if motor_count else b""
    return header + body


def recv_json(socket: zmq.Socket, timeout_ms: int) -> tuple[str, dict[str, Any]]:
    if socket.poll(timeout_ms) == 0:
        raise TimeoutError(f"timed out after {timeout_ms} ms")
    topic, payload = socket.recv_multipart()
    return topic.decode("utf-8").strip(), json.loads(payload.decode("utf-8"))


def recv_frame(socket: zmq.Socket, timeout_ms: int) -> tuple[str, bytes]:
    if socket.poll(timeout_ms) == 0:
        raise TimeoutError(f"timed out after {timeout_ms} ms")
    topic, payload = socket.recv_multipart()
    return topic.decode("utf-8").strip(), payload


def wait_for_meta(ctx: zmq.Context, host: str, port: int, robot: str, timeout_ms: int) -> dict[str, Any]:
    sub = ctx.socket(zmq.SUB)
    sub.setsockopt_string(zmq.SUBSCRIBE, f"meta/{robot}")
    sub.connect(f"tcp://{host}:{port}")
    try:
        _, meta = recv_json(sub, timeout_ms)
        return meta
    finally:
        sub.close(linger=0)


def save_bgra_png(payload: bytes, width: int, height: int, path: Path) -> None:
    expected = width * height * 4
    if len(payload) != expected:
        raise ValueError(f"camera payload has {len(payload)} bytes, expected {expected}")
    image = Image.frombytes("RGBA", (width, height), payload, "raw", "BGRA")
    image.convert("RGB").save(path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--robot", default="robot_rp0")
    parser.add_argument("--meta-port", type=int, default=10101)
    parser.add_argument("--timeout-ms", type=int, default=5000)
    parser.add_argument("--sequence", type=int, default=1)
    parser.add_argument("--out", default="py_example/out")
    parser.add_argument("--camera-index", type=int, default=0)
    parser.add_argument("--camera-endpoint", default="")
    parser.add_argument("--camera-topic", default="")
    parser.add_argument("--camera-width", type=int, default=640)
    parser.add_argument("--camera-height", type=int, default=480)
    args = parser.parse_args()

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    ctx = zmq.Context()
    try:
        meta = wait_for_meta(ctx, args.host, args.meta_port, args.robot, args.timeout_ms)
        (out_dir / "meta.json").write_text(json.dumps(meta, indent=2, sort_keys=True))

        motors = len(meta.get("actuator_names", []))
        push = ctx.socket(zmq.PUSH)
        push.connect(client_endpoint(meta["command_endpoint"], args.host))

        state_sub = ctx.socket(zmq.SUB)
        state_sub.setsockopt_string(zmq.SUBSCRIBE, meta["state_topic"])
        state_sub.connect(client_endpoint(meta["state_endpoint"], args.host))

        try:
            push.send(encode_zero_command(args.sequence, motors))
            state_topic, state = recv_json(state_sub, args.timeout_ms)
            (out_dir / "state.json").write_text(json.dumps(state, indent=2, sort_keys=True))
        finally:
            push.close(linger=0)
            state_sub.close(linger=0)

        cameras = list(meta.get("cameras", []))
        camera_endpoint = args.camera_endpoint
        camera_topic = args.camera_topic
        camera_width = args.camera_width
        camera_height = args.camera_height
        camera_format = "bgra8"

        if not camera_endpoint and cameras:
            camera = cameras[args.camera_index]
            camera_endpoint = camera["endpoint"]
            camera_topic = camera["topic"]
            camera_width = int(camera["width"])
            camera_height = int(camera["height"])
            camera_format = camera.get("format", "bgra8")

        result: dict[str, Any] = {
            "robot": args.robot,
            "motors": motors,
            "state_topic": state_topic,
            "state_sequence": state.get("sequence"),
            "state_path": str(out_dir / "state.json"),
            "meta_path": str(out_dir / "meta.json"),
        }

        if camera_endpoint and camera_topic:
            cam_sub = ctx.socket(zmq.SUB)
            cam_sub.setsockopt_string(zmq.SUBSCRIBE, camera_topic)
            cam_sub.connect(client_endpoint(camera_endpoint, args.host))
            try:
                frame_topic, frame = recv_frame(cam_sub, args.timeout_ms)
            finally:
                cam_sub.close(linger=0)

            result["camera_topic"] = frame_topic
            result["camera_bytes"] = len(frame)
            if camera_format == "bgra8":
                image_path = out_dir / "camera.png"
                save_bgra_png(frame, camera_width, camera_height, image_path)
                result["camera_path"] = str(image_path)
            else:
                raw_path = out_dir / "camera.raw"
                raw_path.write_bytes(frame)
                result["camera_path"] = str(raw_path)
                result["camera_format"] = camera_format
        else:
            result["camera_path"] = None
            result["camera_note"] = "No camera advertised in metadata; add/enable a UMjCamera or pass --camera-endpoint/--camera-topic."

        print(json.dumps(result, indent=2, sort_keys=True))
        return 0
    finally:
        ctx.term()


if __name__ == "__main__":
    raise SystemExit(main())
