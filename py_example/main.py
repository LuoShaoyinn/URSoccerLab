from __future__ import annotations

import argparse
import json
import math
import re
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


def encode_motor_command(sequence: int, motors: list[float]) -> bytes:
    header = struct.pack("<IHHQdI", MAGIC, VERSION, 0, sequence, time.time(), len(motors))
    body = struct.pack(f"<{len(motors)}f", *motors) if motors else b""
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


def recv_latest_frame(socket: zmq.Socket, timeout_ms: int, frame_count: int) -> tuple[str, bytes, int]:
    frame_count = max(frame_count, 1)
    topic = ""
    payload = b""
    for index in range(frame_count):
        per_frame_timeout = timeout_ms if index == 0 else max(timeout_ms // 3, 1000)
        topic, payload = recv_frame(socket, per_frame_timeout)
    return topic, payload, frame_count


def select_motion_indices(
    actuator_names: list[str],
    pattern: str,
) -> list[int]:
    if not pattern:
        return []

    compiled = re.compile(pattern, re.IGNORECASE)
    indices = [index for index, name in enumerate(actuator_names) if compiled.search(name)]
    if indices:
        return indices

    raise RuntimeError(f"no actuator name matched --motion-regex {pattern!r}")


def build_motion_command(
    motor_count: int,
    motion_indices: list[int],
    amplitude: float,
    frequency_hz: float,
    elapsed_sec: float,
) -> list[float]:
    motors = [0.0] * motor_count
    if not motion_indices:
        return motors

    value = amplitude * math.sin(2.0 * math.pi * frequency_hz * elapsed_sec)
    for offset, index in enumerate(motion_indices):
        motors[index] = value if offset % 2 == 0 else -value
    return motors


def recv_state_at_sequence(
    socket: zmq.Socket,
    timeout_ms: int,
    min_sequence: int,
) -> tuple[str, dict[str, Any]]:
    deadline = time.monotonic() + timeout_ms / 1000.0
    latest: tuple[str, dict[str, Any]] | None = None
    while True:
        remaining_ms = int(max((deadline - time.monotonic()) * 1000.0, 0.0))
        if remaining_ms <= 0:
            if latest is not None:
                return latest
            raise TimeoutError(f"timed out after {timeout_ms} ms")
        topic, state = recv_json(socket, remaining_ms)
        latest = (topic, state)
        if int(state.get("sequence", 0)) >= min_sequence:
            return topic, state


def recv_available_json(socket: zmq.Socket) -> tuple[str, dict[str, Any]] | None:
    if socket.poll(0) == 0:
        return None
    topic, payload = socket.recv_multipart()
    return topic.decode("utf-8").strip(), json.loads(payload.decode("utf-8"))


def max_abs(values: list[float]) -> float:
    return max((abs(value) for value in values), default=0.0)


def state_motor_max_abs(state: dict[str, Any]) -> float:
    return max_abs([float(value) for value in state.get("motor_command", [])])


def send_motion_stream_and_recv_state(
    push: zmq.Socket,
    state_sub: zmq.Socket,
    timeout_ms: int,
    sequence: int,
    motor_count: int,
    motion_indices: list[int],
    amplitude: float,
    frequency_hz: float,
    duration_sec: float,
    rate_hz: float,
) -> tuple[int, list[float], str, dict[str, Any]]:
    if duration_sec <= 0.0 or not motion_indices:
        motors = [0.0] * motor_count
        push.send(encode_motor_command(sequence, motors))
        state_topic, state = recv_state_at_sequence(state_sub, timeout_ms, sequence)
        return sequence, motors, state_topic, state

    interval_sec = 1.0 / max(rate_hz, 1.0)
    deadline = time.monotonic() + timeout_ms / 1000.0
    motion_deadline = time.monotonic() + duration_sec
    start = time.monotonic()
    next_send = start
    latest: tuple[str, dict[str, Any]] | None = None
    last_sequence = sequence - 1
    last_motors = [0.0] * motor_count

    while time.monotonic() < deadline:
        now = time.monotonic()
        if now >= next_send and now <= motion_deadline:
            last_sequence += 1
            elapsed = now - start
            last_motors = build_motion_command(
                motor_count, motion_indices, amplitude, frequency_hz, elapsed
            )
            push.send(encode_motor_command(last_sequence, last_motors))
            next_send = now + interval_sec

        received = recv_available_json(state_sub)
        if received is not None:
            latest = received
            _, state = received
            if int(state.get("sequence", 0)) >= last_sequence and state_motor_max_abs(state) > 1.0e-5:
                return last_sequence, last_motors, received[0], state

        if now > motion_deadline and latest is not None:
            return last_sequence, last_motors, latest[0], latest[1]

        time.sleep(0.002)

    if latest is not None:
        return last_sequence, last_motors, latest[0], latest[1]
    raise TimeoutError(f"timed out after {timeout_ms} ms")


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
    parser.add_argument("--camera-frame-count", type=int, default=1)
    parser.add_argument("--motion-regex", default="")
    parser.add_argument("--motion-amplitude", type=float, default=0.25)
    parser.add_argument("--motion-frequency-hz", type=float, default=0.5)
    parser.add_argument("--motion-duration-sec", type=float, default=0.0)
    parser.add_argument("--motion-rate-hz", type=float, default=30.0)
    args = parser.parse_args()

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    ctx = zmq.Context()
    try:
        meta = wait_for_meta(ctx, args.host, args.meta_port, args.robot, args.timeout_ms)
        (out_dir / "meta.json").write_text(json.dumps(meta, indent=2, sort_keys=True))

        actuator_names = list(meta.get("actuator_names", []))
        motors = len(actuator_names)
        motion_indices = select_motion_indices(actuator_names, args.motion_regex)
        push = ctx.socket(zmq.PUSH)
        push.connect(client_endpoint(meta["command_endpoint"], args.host))

        state_sub = ctx.socket(zmq.SUB)
        state_sub.setsockopt_string(zmq.SUBSCRIBE, meta["state_topic"])
        state_sub.connect(client_endpoint(meta["state_endpoint"], args.host))

        try:
            last_sequence, last_command, state_topic, state = send_motion_stream_and_recv_state(
                push,
                state_sub,
                args.timeout_ms,
                args.sequence,
                motors,
                motion_indices,
                args.motion_amplitude,
                args.motion_frequency_hz,
                args.motion_duration_sec,
                args.motion_rate_hz,
            )
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
            "motion_actuators": [actuator_names[index] for index in motion_indices],
            "motion_indices": motion_indices,
            "last_command_sequence": last_sequence,
            "last_command_max_abs": max((abs(value) for value in last_command), default=0.0),
        }

        if camera_endpoint and camera_topic:
            cam_sub = ctx.socket(zmq.SUB)
            cam_sub.setsockopt_string(zmq.SUBSCRIBE, camera_topic)
            cam_sub.connect(client_endpoint(camera_endpoint, args.host))
            try:
                frame_topic, frame, camera_frame_count = recv_latest_frame(
                    cam_sub, args.timeout_ms, args.camera_frame_count
                )
            finally:
                cam_sub.close(linger=0)

            result["camera_topic"] = frame_topic
            result["camera_bytes"] = len(frame)
            result["camera_frame_count"] = camera_frame_count
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
