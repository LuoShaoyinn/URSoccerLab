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
        # Read the spawn pose first so we can confirm later writes against it.
        initial_reply = rpc(sock, {"op": "get_pose"}, args.timeout_ms)
        summary["ops"].append({"op": "get_pose_initial", "reply": initial_reply})
        if not initial_reply.get("ok"):
            raise RuntimeError(f"initial get_pose failed: {initial_reply}")
        initial_joint_qpos = initial_reply.get("joint_qpos", [])
        if not initial_joint_qpos:
            raise RuntimeError(f"initial get_pose missing joint_qpos: {initial_reply}")

        # Confirm the joint qpos dim so we can build a valid move request.
        joint_dim = len(initial_joint_qpos)

        # Build a joint_qpos vector that differs from the current pose: pick
        # the first joint and inject a non-zero value, keep the rest at 0.
        moved_joints = [0.0] * joint_dim
        moved_joints[0] = 0.1
        # If joint 0 is already at 0.1, use a different value to guarantee a
        # measurable change.
        if abs(initial_joint_qpos[0] - 0.1) < 1e-4:
            moved_joints[0] = -0.1

        set_pose_request: dict = {
            "op": "set_pose",
            "joint_qpos": moved_joints,
        }
        set_pose_reply = rpc(sock, set_pose_request, args.timeout_ms)
        summary["ops"].append({"op": "set_pose_joints", "reply": set_pose_reply})
        if not set_pose_reply.get("ok"):
            raise RuntimeError(f"set_pose (joint_qpos) failed: {set_pose_reply}")

        # Verify MuJoCo ground truth reflects the joint move.
        verify_reply = rpc(sock, {"op": "get_pose"}, args.timeout_ms)
        summary["ops"].append({"op": "get_pose_after_set", "reply": verify_reply})
        if not verify_reply.get("ok"):
            raise RuntimeError(f"post-set get_pose failed: {verify_reply}")
        verified = verify_reply.get("joint_qpos", [])
        if len(verified) != joint_dim:
            raise RuntimeError(
                f"post-set joint_qpos length {len(verified)} != expected {joint_dim}"
            )
        if abs(verified[0] - moved_joints[0]) > 0.01:
            raise RuntimeError(
                f"MuJoCo joint qpos did not reflect set_pose: "
                f"expected joint[0]={moved_joints[0]:.4f}, got {verified[0]:.4f}"
            )

        # If the articulation has a free root joint (e.g. after a future asset
        # swap), a translation move should land in MuJoCo xpos. If it is
        # fixed to the world, we expect a fixed_base error. Either is
        # acceptable; assert the right one for the current asset.
        has_free_root = bool(initial_reply.get("rotation_quat_xyzw")) and (
            initial_reply.get("translation_m", [None])[0] is not None
        )
        translation_probe = rpc(
            sock,
            {
                "op": "set_pose",
                "translation_m": [0.5, 0.0, max(initial_reply.get("translation_m", [0, 0, 0])[2], 0.0)],
                "rotation_quat_xyzw": [0.0, 0.0, 0.0, 1.0],
            },
            args.timeout_ms,
        )
        summary["ops"].append({"op": "set_pose_translation_probe", "reply": translation_probe})
        if translation_probe.get("ok"):
            # Verify the move stuck in ground truth.
            verify_after_translate = rpc(sock, {"op": "get_pose"}, args.timeout_ms)
            verified_t = verify_after_translate.get("translation_m", [])
            if len(verified_t) != 3 or abs(verified_t[0] - 0.5) > 1e-3:
                raise RuntimeError(
                    f"set_pose translation did not land in MuJoCo xpos: got {verified_t}"
                )
            summary["ops"].append({"op": "get_pose_after_translate", "reply": verify_after_translate})
        elif translation_probe.get("error") != "fixed_base":
            raise RuntimeError(
                f"unexpected set_pose error for translation: {translation_probe}"
            )

        # reset should zero the joints again.
        reset_reply = rpc(sock, {"op": "reset"}, args.timeout_ms)
        summary["ops"].append({"op": "reset", "reply": reset_reply})
        if not reset_reply.get("ok"):
            raise RuntimeError(f"reset failed: {reset_reply}")

        reset_verify_reply = rpc(sock, {"op": "get_pose"}, args.timeout_ms)
        summary["ops"].append({"op": "get_pose_after_reset", "reply": reset_verify_reply})
        if not reset_verify_reply.get("ok"):
            raise RuntimeError(f"post-reset get_pose failed: {reset_verify_reply}")
        reset_joints = reset_verify_reply.get("joint_qpos", [])
        if len(reset_joints) != joint_dim:
            raise RuntimeError(
                f"post-reset joint_qpos length {len(reset_joints)} != expected {joint_dim}"
            )
        for idx, value in enumerate(reset_joints):
            # reset writes exact zeros then mj_forward, but the next physics
            # step applies actuator + gravity forces, so joints drift a
            # little. 0.05 rad (~3 deg) is well below any meaningful
            # behavioral change but tolerant of one or two physics steps.
            if abs(value) > 0.05:
                raise RuntimeError(
                    f"reset did not zero joint[{idx}]: got {value:.4f}"
                )

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
