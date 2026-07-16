#!/usr/bin/env python3
"""Minimal URSoccerLab ZMQ smoke client.

Run the simulator with UURSZmqRobotBridgeComponent active, then:

    python Tools/zmq_smoke_client.py --robot robot_rp0

The client waits for metadata, sends one zero motor command to the robot's
command port, then waits for one state packet on the robot state topic.
"""

from __future__ import annotations

import argparse
import json
import struct
import time
from dataclasses import dataclass

import zmq


MAGIC = 0x4D535255
VERSION = 1


@dataclass
class RobotMeta:
    robot: str
    command_endpoint: str
    state_endpoint: str
    state_topic: str
    actuator_names: list[str]


def endpoint_for_client(bind_endpoint: str, host: str) -> str:
    return bind_endpoint.replace("tcp://0.0.0.0:", f"tcp://{host}:").replace(
        "tcp://*:", f"tcp://{host}:"
    )


def encode_motor_command(sequence: int, stamp_sec: float, motors: list[float]) -> bytes:
    header = struct.pack("<IHHQdI", MAGIC, VERSION, 0, sequence, stamp_sec, len(motors))
    body = struct.pack(f"<{len(motors)}f", *motors) if motors else b""
    return header + body


def recv_json_with_topic(socket: zmq.Socket, timeout_ms: int) -> tuple[str, dict]:
    if socket.poll(timeout_ms) == 0:
        raise TimeoutError(f"timed out after {timeout_ms} ms")
    topic, payload = socket.recv_multipart()
    return topic.decode("utf-8"), json.loads(payload.decode("utf-8"))


def wait_for_meta(ctx: zmq.Context, robot: str, meta_endpoint: str, timeout_ms: int) -> RobotMeta:
    sub = ctx.socket(zmq.SUB)
    sub.setsockopt_string(zmq.SUBSCRIBE, f"meta/{robot}")
    sub.connect(meta_endpoint)
    try:
        topic, payload = recv_json_with_topic(sub, timeout_ms)
    finally:
        sub.close(linger=0)

    if topic != f"meta/{robot}":
        raise RuntimeError(f"unexpected metadata topic {topic!r}")
    return RobotMeta(
        robot=payload["robot"],
        command_endpoint=payload["command_endpoint"],
        state_endpoint=payload["state_endpoint"],
        state_topic=payload["state_topic"],
        actuator_names=list(payload.get("actuator_names", [])),
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--robot", default="robot_rp0")
    parser.add_argument("--meta-port", type=int, default=10101)
    parser.add_argument("--timeout-ms", type=int, default=5000)
    parser.add_argument("--sequence", type=int, default=1)
    args = parser.parse_args()

    ctx = zmq.Context()
    meta_endpoint = f"tcp://{args.host}:{args.meta_port}"
    meta = wait_for_meta(ctx, args.robot, meta_endpoint, args.timeout_ms)

    command_endpoint = endpoint_for_client(meta.command_endpoint, args.host)
    state_endpoint = endpoint_for_client(meta.state_endpoint, args.host)

    push = ctx.socket(zmq.PUSH)
    push.connect(command_endpoint)

    sub = ctx.socket(zmq.SUB)
    sub.setsockopt_string(zmq.SUBSCRIBE, meta.state_topic)
    sub.connect(state_endpoint)

    try:
        motors = [0.0] * len(meta.actuator_names)
        push.send(encode_motor_command(args.sequence, time.time(), motors))
        topic, state = recv_json_with_topic(sub, args.timeout_ms)
    finally:
        push.close(linger=0)
        sub.close(linger=0)
        ctx.term()

    print(
        json.dumps(
            {
                "robot": meta.robot,
                "actuators": len(meta.actuator_names),
                "command_endpoint": command_endpoint,
                "state_topic": topic,
                "state_sequence": state.get("sequence"),
                "sim_time": state.get("sim_time"),
                "command_timed_out": state.get("command_timed_out"),
            },
            indent=2,
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
