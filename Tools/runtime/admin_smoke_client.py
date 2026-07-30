#!/usr/bin/env python3
"""Minimal URSoccerLab admin RPC smoke client (TCP).

Run the simulator with TCP transport active, then:

    uv run --project py_example python Tools/runtime/admin_smoke_client.py --robot robot_rp0

The client connects to the admin TCP port, exercises set_pose (joint_qpos),
set_pose (translation), reset, and a deliberate dim_mismatch error.
"""

from __future__ import annotations

import argparse
import json
import time

from ursoccerlab.tcp import AdminClient


def rpc(client: AdminClient, command: str, args: dict, timeout_ms: int) -> dict:
    deadline = time.time() + timeout_ms / 1000.0
    while time.time() < deadline:
        try:
            return client._request(command, args)
        except TimeoutError:
            time.sleep(0.01)
    raise TimeoutError(f"admin request '{command}' timed out after {timeout_ms} ms")


def reply_result(reply: dict) -> dict:
    result = reply.get("result")
    return result if isinstance(result, dict) else reply


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--robot", default="robot_rp0")
    parser.add_argument("--port", type=int, default=11000)
    parser.add_argument("--timeout-ms", type=int, default=5000)
    args = parser.parse_args()

    client = AdminClient(args.host, args.port)

    summary: dict = {"robot": args.robot, "admin_port": args.port, "ops": []}
    try:
        initial_reply = rpc(client, "get_pose", {"actor_id": args.robot}, args.timeout_ms)
        summary["ops"].append({"op": "get_pose_initial", "reply": initial_reply})
        if not initial_reply.get("ok"):
            raise RuntimeError(f"initial get_pose failed: {initial_reply}")
        initial_pose = reply_result(initial_reply)
        initial_joint_qpos = initial_pose.get("joint_qpos", [])
        if not initial_joint_qpos:
            raise RuntimeError(f"initial get_pose missing joint_qpos: {initial_reply}")

        joint_dim = len(initial_joint_qpos)

        moved_joints = [0.0] * joint_dim
        moved_joints[0] = 0.1
        if abs(initial_joint_qpos[0] - 0.1) < 1e-4:
            moved_joints[0] = -0.1

        set_pose_reply = rpc(client, "set_pose", {
            "actor_id": args.robot,
            "joint_qpos": moved_joints,
        }, args.timeout_ms)
        summary["ops"].append({"op": "set_pose_joints", "reply": set_pose_reply})
        if not set_pose_reply.get("ok"):
            raise RuntimeError(f"set_pose (joint_qpos) failed: {set_pose_reply}")

        verify_reply = rpc(client, "get_pose", {"actor_id": args.robot}, args.timeout_ms)
        summary["ops"].append({"op": "get_pose_after_set", "reply": verify_reply})
        if not verify_reply.get("ok"):
            raise RuntimeError(f"post-set get_pose failed: {verify_reply}")
        verified = reply_result(verify_reply).get("joint_qpos", [])
        if len(verified) != joint_dim:
            raise RuntimeError(f"post-set joint_qpos length {len(verified)} != expected {joint_dim}")
        if abs(verified[0] - moved_joints[0]) > 0.01:
            raise RuntimeError(
                f"MuJoCo joint qpos did not reflect set_pose: "
                f"expected joint[0]={moved_joints[0]:.4f}, got {verified[0]:.4f}"
            )

        initial_t = initial_pose.get("translation_m", [0, 0, 0])
        translation_probe = rpc(client, "set_pose", {
            "actor_id": args.robot,
            "translation_m": [0.5, 0.0, max(initial_t[2] if initial_t else 0, 0.0)],
            "rotation_quat_xyzw": [0.0, 0.0, 0.0, 1.0],
        }, args.timeout_ms)
        summary["ops"].append({"op": "set_pose_translation_probe", "reply": translation_probe})
        if translation_probe.get("ok"):
            verify_t = rpc(client, "get_pose", {"actor_id": args.robot}, args.timeout_ms)
            verified_t = reply_result(verify_t).get("translation_m", [])
            if len(verified_t) != 3 or abs(verified_t[0] - 0.5) > 1e-3:
                raise RuntimeError(f"set_pose translation did not land in MuJoCo xpos: got {verified_t}")
            summary["ops"].append({"op": "get_pose_after_translate", "reply": verify_t})
        elif translation_probe.get("error") != "fixed_base":
            raise RuntimeError(f"unexpected set_pose error for translation: {translation_probe}")

        reset_reply = rpc(client, "reset", {"actor_id": args.robot}, args.timeout_ms)
        summary["ops"].append({"op": "reset", "reply": reset_reply})
        if not reset_reply.get("ok"):
            raise RuntimeError(f"reset failed: {reset_reply}")

        reset_verify = rpc(client, "get_pose", {"actor_id": args.robot}, args.timeout_ms)
        summary["ops"].append({"op": "get_pose_after_reset", "reply": reset_verify})
        reset_joints = reply_result(reset_verify).get("joint_qpos", [])
        if len(reset_joints) != joint_dim:
            raise RuntimeError(f"post-reset joint_qpos length mismatch")
        for idx, value in enumerate(reset_joints):
            if abs(value) > 0.05:
                raise RuntimeError(f"reset did not zero joint[{idx}]: got {value:.4f}")

        bad_reply = rpc(client, "set_pose", {
            "actor_id": args.robot,
            "joint_qpos": [99.0],
        }, args.timeout_ms)
        summary["ops"].append({"op": "set_pose_bad_dim", "reply": bad_reply})
        if bad_reply.get("ok") or bad_reply.get("error") != "dim_mismatch":
            raise RuntimeError(f"expected dim_mismatch error, got: {bad_reply}")

        summary["ok"] = True
    finally:
        client.close()
        print(json.dumps(summary, indent=2, sort_keys=True))

    return 0 if summary.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
