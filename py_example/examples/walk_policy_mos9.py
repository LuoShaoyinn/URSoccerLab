#!/usr/bin/env python3
"""Run a MOS9 walking policy (ONNX) against the URSoccerLab sim.

The policy was trained with AMP on IsaacLab (walk_v11_terrain).
Observation: 63-dim (3 ang_vel + 3 gravity + 3 cmd + 18 joint_pos_rel + 18 joint_vel_rel + 18 prev_action)
Action: 18-dim position offsets from default pose, scaled per-joint.

Usage:
    uv run python examples/walk_policy_mos9.py --port 10000 --vx 0.4 --duration 15
"""
from __future__ import annotations

import argparse
import math
import time
import json
from pathlib import Path

import numpy as np

from ursoccerlab.media import camera_to_rgb, write_video
from ursoccerlab.tcp import (
    IMAGE_MESSAGE_VERSION,
    FrameConn,
    TYPE_JSON,
    TYPE_RGB,
    parse_camera,
    parse_image_message,
    AdminClient,
)

# ── Policy joint order (from MOS9 AMP training) ────────────────────────────
MOS9_JOINT_NAMES = [
    "right_shoulder_pitch", "right_shoulder_roll", "right_elbow",
    "left_shoulder_pitch", "left_shoulder_roll", "left_elbow",
    "right_hip_pitch", "right_hip_roll", "right_hip_yaw",
    "right_knee", "right_ankle_pitch", "right_ankle_roll",
    "left_hip_pitch", "left_hip_roll", "left_hip_yaw",
    "left_knee", "left_ankle_pitch", "left_ankle_roll",
]

# Default pose (only shoulder_roll is non-zero)
DEFAULT_QPOS = np.zeros(18)
DEFAULT_QPOS[1] = -1.4   # right_shoulder_roll
DEFAULT_QPOS[4] = 1.4    # left_shoulder_roll

# Action scales: 0.25 * torque_limit / stiffness
STIFFNESS_4310 = 59.59586122651323
STIFFNESS_6408 = 98.30757637604704
TORQUE_LIMIT_4310 = 36.0
TORQUE_LIMIT_6408 = 60.0

ACTION_SCALES = np.array([
    0.25 * TORQUE_LIMIT_4310 / STIFFNESS_4310 if (
        "ankle_roll" in j or "shoulder_pitch" in j or "shoulder_roll" in j
    ) else 0.25 * TORQUE_LIMIT_6408 / STIFFNESS_6408
    for j in MOS9_JOINT_NAMES
], dtype=np.float64)

# Map policy joint name → URSoccerLab actuator name
def _joint_to_servo(joint_name: str) -> str:
    """right_shoulder_pitch → r_shoulder_pitch_joint_servo"""
    prefix_map = {"right": "r", "left": "l"}
    parts = joint_name.split("_")
    side = prefix_map.get(parts[0], parts[0])
    rest = "_".join(parts[1:])
    return f"{side}_{rest}_joint_servo"

SERVO_NAMES = [_joint_to_servo(j) for j in MOS9_JOINT_NAMES]

# Map policy joint name → URSoccerLab joint name (for state reading)
def _joint_name_urs(joint_name: str) -> str:
    prefix_map = {"right": "r", "left": "l"}
    parts = joint_name.split("_")
    side = prefix_map.get(parts[0], parts[0])
    rest = "_".join(parts[1:])
    return f"{side}_{rest}_joint"

JOINT_NAMES_URS = [_joint_name_urs(j) for j in MOS9_JOINT_NAMES]


class OnnxPolicy:
    def __init__(self, model_path: str):
        import onnxruntime as ort
        self.session = ort.InferenceSession(model_path, providers=["CPUExecutionProvider"])
        self.input_name = self.session.get_inputs()[0].name

    def __call__(self, obs: np.ndarray) -> np.ndarray:
        y = self.session.run(None, {self.input_name: obs.astype(np.float32)[None, :]})[0]
        return y[0]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=10000)
    ap.add_argument("--policy", type=Path,
                    default=Path("refs/MOS9-AMP/logs/rsl_rl/mos9_loco/walk_v11_terrain/exported/policy_5500.onnx"))
    ap.add_argument("--duration", type=float, default=15.0)
    ap.add_argument("--vx", type=float, default=0.4, help="forward velocity command (m/s)")
    ap.add_argument("--vy", type=float, default=0.0)
    ap.add_argument("--wz", type=float, default=0.0, help="yaw rate command (rad/s)")
    ap.add_argument("--policy-hz", type=float, default=50.0)
    ap.add_argument("--video-fps", type=int, default=30)
    ap.add_argument("--video", type=Path, default=Path("out/mos9_walk.mp4"))
    ap.add_argument("--admin-port", type=int, default=11000)
    ap.add_argument("--actor", default="robot_mos9")
    ap.add_argument("--base-height", type=float, default=0.56)
    args = ap.parse_args()

    if not args.policy.exists():
        print(f"ERROR: policy not found: {args.policy}")
        return 1

    policy = OnnxPolicy(str(args.policy))
    print(f"[walk] loaded policy: {args.policy}", flush=True)

    conn = FrameConn(args.host, args.port)

    # Wait for first state
    print("[walk] waiting for state...", flush=True)
    state = None
    deadline = time.monotonic() + 15.0
    while time.monotonic() < deadline:
        for ftype, payload in conn.receive_available():
            if ftype == TYPE_JSON:
                state = json.loads(payload.decode("utf-8"))
                break
        if state:
            break
        time.sleep(0.01)

    if not state:
        print("[walk] ERROR: no state received")
        return 1

    # Verify actuators exist
    actuators = list(state.get("actuators", {}).keys())
    print(f"[walk] {len(actuators)} actuators", flush=True)
    for sn in SERVO_NAMES:
        if sn not in actuators:
            print(f"[walk] ERROR: actuator '{sn}' not found in {actuators}")
            return 1

    # Send default pose targets first so actuators hold the pose
    default_cmd = {sn: float(DEFAULT_QPOS[i]) for i, sn in enumerate(SERVO_NAMES)}
    default_cmd["head_yaw_joint_servo"] = 0.0
    default_cmd["head_pitch_joint_servo"] = 0.0
    conn.send_json(default_cmd)
    time.sleep(0.05)  # brief pause for command to arrive

    # Set default standing pose via admin (shoulder_roll ±1.4, base at height)
    joint_names_urs_state = list(state.get("joints", {}).keys())
    joint_qpos = [0.0] * len(joint_names_urs_state)
    for i, jn in enumerate(joint_names_urs_state):
        if "shoulder_roll" in jn and jn.startswith("r"):
            joint_qpos[i] = -1.4
        elif "shoulder_roll" in jn and jn.startswith("l"):
            joint_qpos[i] = 1.4

    try:
        admin = AdminClient(args.host, args.admin_port)
        admin.set_pose(args.actor, translation_m=[-1.0, 0.0, args.base_height],
                       rotation_quat_xyzw=[0, 0, 0, 1], joint_qpos=joint_qpos)
        admin.close()
        print("[walk] set default pose (shoulder_roll ±1.4, z={:.2f})".format(args.base_height), flush=True)
    except Exception as e:
        print(f"[walk] WARNING: admin set_pose failed: {e}", flush=True)
    # Send default command again immediately (command timeout is 0.1s)
    conn.send_json(default_cmd)

    # Initialize
    prev_action = np.zeros(18, dtype=np.float32)
    frames: list[np.ndarray] = []
    interval = 1.0 / args.policy_hz
    t0 = time.monotonic()
    step = 0
    last_policy_sim_time = -1.0
    ctrl_dt = 1.0 / args.policy_hz
    last_cmd = {sn: float(DEFAULT_QPOS[i]) for i, sn in enumerate(SERVO_NAMES)}
    last_cmd["head_yaw_joint_servo"] = 0.0
    last_cmd["head_pitch_joint_servo"] = 0.0

    print(f"[walk] running for {args.duration:.0f}s (vx={args.vx} vy={args.vy} wz={args.wz})...", flush=True)

    while time.monotonic() - t0 < args.duration:
        # Read all available state
        latest_state = None
        for ftype, payload in conn.receive_available():
            if ftype == TYPE_JSON:
                latest_state = json.loads(payload.decode("utf-8"))
            elif ftype == TYPE_RGB:
                cams = (parse_image_message(payload) if payload and payload[0] == IMAGE_MESSAGE_VERSION
                        else parse_camera(payload))
                if cams and cams[0]["data"]:
                    frames.append(camera_to_rgb(cams[0]))

        if latest_state is None:
            time.sleep(0.001)
            continue
        state = latest_state

        # Resend last command every state update to prevent 100ms timeout
        conn.send_json(last_cmd)

        # Gate policy on sim_time
        current_sim_time = latest_state.get("sim_time", 0.0)
        if current_sim_time - last_policy_sim_time >= ctrl_dt:

            # Build observation
            base = latest_state.get("base", {})
            joints = latest_state.get("joints", {})

            # Rotation matrix body→world from quaternion [w, x, y, z]
            quat = base.get("quat", [1, 0, 0, 0])
            w, x, y, z = quat
            R = np.array([
                [1 - 2*(y*y+z*z), 2*(x*y-z*w),   2*(x*z+y*w)],
                [2*(x*y+z*w),     1-2*(x*x+z*z), 2*(y*z-x*w)],
                [2*(x*z-y*w),     2*(y*z+x*w),   1-2*(x*x+y*y)],
            ])

            # Base angular velocity: qvel[3:6] is WORLD frame, policy needs BODY frame
            base_vel = base.get("vel", [0, 0, 0, 0, 0, 0])
            ang_vel_world = np.array(base_vel[3:6] if len(base_vel) >= 6 else [0, 0, 0], dtype=np.float64)
            ang_vel = R.T @ ang_vel_world  # world → body

            # Projected gravity: rotate [0,0,-1] from world to body frame
            grav_body = R.T @ np.array([0, 0, -1.0])

            # Joint positions and velocities in policy order
            qpos = np.array([joints.get(jn, {}).get("qpos", 0.0) for jn in JOINT_NAMES_URS])
            qvel = np.array([joints.get(jn, {}).get("qvel", 0.0) for jn in JOINT_NAMES_URS])

            obs = np.concatenate([
                ang_vel * 0.2,
                grav_body,
                np.array([args.vx, args.vy, args.wz]),
                qpos - DEFAULT_QPOS,
                qvel * 0.05,
                prev_action,
            ]).astype(np.float32)

            # Policy inference
            action = policy(obs).astype(np.float64)
            target = DEFAULT_QPOS + action * ACTION_SCALES
            prev_action = action.astype(np.float32)

            # Send position targets to all 20 actuators
            last_cmd = {sn: float(target[i]) for i, sn in enumerate(SERVO_NAMES)}
            last_cmd["head_yaw_joint_servo"] = 0.0
            last_cmd["head_pitch_joint_servo"] = 0.0
            conn.send_json(last_cmd)

            step += 1
            last_policy_sim_time = current_sim_time
            if step % 50 == 0:
                pos = base.get("pos", [0, 0, 0])
                print(f"  step={step} sim_t={current_sim_time:.1f} pos=({pos[0]:+.2f},{pos[1]:+.2f},{pos[2]:.2f}) "
                      f"frames={len(frames)}", flush=True)
        else:
            time.sleep(0.001)

    print("[walk] done, saving video...", flush=True)
    conn.close()
    write_video(frames, args.video, args.video_fps)
    print(f"[walk] saved {args.video} ({len(frames)} frames)", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
