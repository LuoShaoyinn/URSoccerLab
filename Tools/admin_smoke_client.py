#!/usr/bin/env python3
"""Minimal URSoccerLab admin RPC smoke client.

Run the simulator with UURSZmqRobotBridgeComponent active, then:

    python Tools/admin_smoke_client.py --robot robot_rp0

The client waits for metadata on the meta PUB, resolves the robot's
``admin_endpoint``, then exercises both admin RPCs:

    1. POST set_pose with a fresh translation
    2. POST reset to return to the spawn pose

Both replies must report ``ok: true`` for the smoke test to pass.
"""

from __future__ import annotations

import argparse
import json
import time
from dataclasses import dataclass

import zmq


@dataclass
class RobotMeta:
    robot: str
    admin_endpoint: str


def endpoint_for_client(bind_endpoint: str, host: str) -> str:
    return bind_endpoint.replace("tcp://0.0.0.0:", f"tcp://{host}:").replace(
        "tcp://*:", f"tcp://{host}:"
    )


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
    if "admin_endpoint" not in payload:
        raise RuntimeError("metadata did not advertise admin_endpoint; rebuild the simulator")
    return RobotMeta(robot=payload["robot"], admin_endpoint=payload["admin_endpoint"])


def rpc(socket: zmq.Socket, request: dict, timeout_ms: int) -> dict:
    socket.send_string(json.dumps(request))
    if socket.poll(timeout_ms) == 0:
        raise TimeoutError(f"admin RPC '{request.get('op')}' timed out after {timeout_ms} ms")
    reply = json.loads(socket.recv().decode("utf-8"))
    return reply


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--robot", default="robot_rp0")
    parser.add_argument("--meta-port", type=int, default=10101)
    parser.add_argument("--timeout-ms", type=int, default=5000)
    args = parser.parse_args()

    ctx = zmq.Context()
    meta_endpoint = f"tcp://{args.host}:{args.meta_port}"
    meta = wait_for_meta(ctx, args.robot, meta_endpoint, args.timeout_ms)
    admin_endpoint = endpoint_for_client(meta.admin_endpoint, args.host)

    sock = ctx.socket(zmq.REQ)
    sock.setsockopt(zmq.LINGER, 0)
    sock.connect(admin_endpoint)

    summary: dict = {"robot": meta.robot, "admin_endpoint": admin_endpoint, "ops": []}
    try:
        moved_translation = [0.5, 0.0, 0.3762]
        set_pose_request: dict = {
            "op": "set_pose",
            "translation_m": moved_translation,
            "rotation_quat_xyzw": [0.0, 0.0, 0.0, 1.0],
        }
        set_pose_reply = rpc(sock, set_pose_request, args.timeout_ms)
        summary["ops"].append({"op": "set_pose", "reply": set_pose_reply})
        if not set_pose_reply.get("ok"):
            raise RuntimeError(f"set_pose failed: {set_pose_reply}")
        applied = set_pose_reply.get("applied_translation_m", [])
        if len(applied) != 3 or abs(applied[0] - moved_translation[0]) > 1e-6:
            raise RuntimeError(f"set_pose applied_translation_m mismatch: {applied}")

        reset_reply = rpc(sock, {"op": "reset"}, args.timeout_ms)
        summary["ops"].append({"op": "reset", "reply": reset_reply})
        if not reset_reply.get("ok"):
            raise RuntimeError(f"reset failed: {reset_reply}")
        reset_applied = reset_reply.get("applied_translation_m", [])
        if len(reset_applied) != 3:
            raise RuntimeError(f"reset did not report applied_translation_m: {reset_applied}")

        bad_reply = rpc(
            sock,
            {"op": "set_pose", "joint_qpos": [99.0]},
            args.timeout_ms,
        )
        summary["ops"].append({"op": "set_pose_bad_dim", "reply": bad_reply})
        if bad_reply.get("ok") or bad_reply.get("error") != "dim_mismatch":
            raise RuntimeError(f"expected dim_mismatch error, got: {bad_reply}")

        summary["ok"] = True
    finally:
        sock.close(linger=0)
        ctx.term()
        print(json.dumps(summary, indent=2, sort_keys=True))

    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0 if summary.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
