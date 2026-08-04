#!/usr/bin/env python3
"""Run a MOS9 walking policy (ONNX) while recording both robots' cameras.

The simulator must be running with a scene that has at least one MOS9 robot.
For the observer variant, use ``Config/examples/mos9_face_to_face.json``::

    cd py_example
    uv run python examples/mos9_walk.py --robot-port 10000 --vx 0.4 --duration 15 \
        --video out/walker.mp4 --observer-port 10001 --observer-video out/observer.mp4

For solo walking, use ``Config/examples/mos9_solo.json``::

    uv run python examples/mos9_walk.py --robot-port 10000 --vx 0.4 --duration 15 \
        --video out/walker.mp4
"""
from __future__ import annotations

import argparse
import json
import math
import time
from pathlib import Path

import numpy as np

from ursoccerlab.media import camera_to_rgb, write_video
from ursoccerlab.tcp import RobotClient, AdminClient

REPO_ROOT = Path(__file__).resolve().parents[2]
POLICY_PATH = REPO_ROOT / "refs/MOS9-AMP/logs/rsl_rl/mos9_loco/walk_v11_terrain/exported/policy_5500.onnx"

# ── Policy joint order (from AMP training, walk_v11_terrain) ────────────────
MOS9_JOINT_NAMES = [
    "right_shoulder_pitch", "right_shoulder_roll", "right_elbow",
    "left_shoulder_pitch", "left_shoulder_roll", "left_elbow",
    "right_hip_pitch", "right_hip_roll", "right_hip_yaw",
    "right_knee", "right_ankle_pitch", "right_ankle_roll",
    "left_hip_pitch", "left_hip_roll", "left_hip_yaw",
    "left_knee", "left_ankle_pitch", "left_ankle_roll",
]

DEFAULT_QPOS = np.zeros(18)
DEFAULT_QPOS[1] = -1.4   # right_shoulder_roll
DEFAULT_QPOS[4] = 1.4    # left_shoulder_roll

STIFFNESS_4310 = 59.59586122651323
STIFFNESS_6408 = 98.30757637604704
ACTION_SCALES = np.array([
    0.25 * (36.0 if ("ankle_roll" in j or "shoulder_pitch" in j or "shoulder_roll" in j) else 60.0)
    / (STIFFNESS_4310 if ("ankle_roll" in j or "shoulder_pitch" in j or "shoulder_roll" in j) else STIFFNESS_6408)
    for j in MOS9_JOINT_NAMES
], dtype=np.float64)

JOINT_RANGES = np.array([
    [-2.0, 2.0], [-1.45, 1.5708], [-2.5, 2.5],
    [-2.0, 2.0], [-1.5708, 1.45], [-2.5, 2.5],
    [-1.8, 1.8], [-1.5708, 0.25], [-1.5708, 1.5708],
    [-1.0, 2.3], [-0.9, 2.1], [-0.7, 0.7],
    [-1.8, 1.8], [-0.25, 1.5708], [-1.5708, 1.5708],
    [-2.3, 1.0], [-2.1, 0.9], [-0.7, 0.7],
], dtype=np.float64)


def _joint_to_servo(joint_name: str) -> str:
    parts = joint_name.split("_")
    side = {"right": "r", "left": "l"}.get(parts[0], parts[0])
    return f"{side}_{'_'.join(parts[1:])}_joint_servo"


def _joint_to_urs(joint_name: str) -> str:
    parts = joint_name.split("_")
    side = {"right": "r", "left": "l"}.get(parts[0], parts[0])
    return f"{side}_{'_'.join(parts[1:])}_joint"


SERVO_NAMES = [_joint_to_servo(j) for j in MOS9_JOINT_NAMES]
JOINT_NAMES_URS = [_joint_to_urs(j) for j in MOS9_JOINT_NAMES]


class OnnxPolicy:
    def __init__(self, model_path: str):
        import onnxruntime as ort
        self.session = ort.InferenceSession(model_path, providers=["CPUExecutionProvider"])
        self.input_name = self.session.get_inputs()[0].name

    def __call__(self, obs: np.ndarray) -> np.ndarray:
        return self.session.run(None, {self.input_name: obs.astype(np.float32)[None, :]})[0][0]


def quat_to_rotmat(wxyz: np.ndarray) -> np.ndarray:
    """Body→world rotation matrix from [w, x, y, z] quaternion."""
    w, x, y, z = wxyz
    return np.array([
        [1 - 2*(y*y+z*z), 2*(x*y-z*w),   2*(x*z+y*w)],
        [2*(x*y+z*w),     1-2*(x*x+z*z), 2*(y*z-x*w)],
        [2*(x*z-y*w),     2*(y*z+x*w),   1-2*(x*x+y*y)],
    ], dtype=np.float64)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--robot-port", type=int, default=10000)
    parser.add_argument("--observer-port", type=int, default=0)
    parser.add_argument("--policy", type=Path, default=POLICY_PATH)
    parser.add_argument("--duration", type=float, default=15.0)
    parser.add_argument("--vx", type=float, default=0.4)
    parser.add_argument("--vy", type=float, default=0.0)
    parser.add_argument("--wz", type=float, default=0.0)
    parser.add_argument("--policy-hz", type=float, default=50.0)
    parser.add_argument("--video-fps", type=int, default=30)
    parser.add_argument("--video", type=Path, default=Path("out/mos9_walker.mp4"))
    parser.add_argument("--observer-video", type=Path, default=Path("out/mos9_observer.mp4"))
    parser.add_argument("--admin-port", type=int, default=11000)
    parser.add_argument("--actor", default="robot_rp0")
    parser.add_argument("--base-height", type=float, default=0.56)
    args = parser.parse_args()

    if not args.policy.exists():
        print(f"ERROR: policy not found: {args.policy}")
        return 1

    policy = OnnxPolicy(str(args.policy))
    client = RobotClient(args.host, args.robot_port)
    observer = RobotClient(args.host, args.observer_port) if args.observer_port else None
    cmd = np.asarray([args.vx, args.vy, args.wz], dtype=np.float32)

    # ── Wait for first state ────────────────────────────────────────────────
    latest_state: dict | None = None
    actuator_names: list[str] | None = None
    frames: list[np.ndarray] = []
    observer_frames: list[np.ndarray] = []
    last_frame_time = float("-inf")
    observer_last_frame_time = float("-inf")

    def pump() -> None:
        nonlocal latest_state, actuator_names, last_frame_time
        for kind, payload in client.recv():
            if kind == "state":
                latest_state = payload
                if actuator_names is None:
                    available = set(payload.get("actuators", {}))
                    missing = [s for s in SERVO_NAMES if s not in available]
                    if missing:
                        print(f"ERROR: missing actuators: {missing}")
                        return
                    actuator_names = SERVO_NAMES + ["head_yaw_joint_servo", "head_pitch_joint_servo"]
            elif kind in ("rgb", "camera"):
                camera = payload[0]
                if camera.get("sim_time", 0) - last_frame_time < 1.0 / args.video_fps:
                    continue
                if camera.get("data"):
                    frames.append(camera_to_rgb(camera))
                    last_frame_time = camera.get("sim_time", 0)

    def pump_observer() -> None:
        nonlocal observer_last_frame_time
        if observer is None:
            return
        for kind, payload in observer.recv():
            if kind not in ("rgb", "camera"):
                continue
            camera = payload[0]
            if camera.get("sim_time", 0) - observer_last_frame_time < 1.0 / args.video_fps:
                continue
            if camera.get("data"):
                observer_frames.append(camera_to_rgb(camera))
                observer_last_frame_time = camera.get("sim_time", 0)

    def send_targets(targets_18: np.ndarray) -> None:
        if actuator_names is None:
            return
        command_dict = {SERVO_NAMES[i]: float(targets_18[i]) for i in range(18)}
        command_dict["head_yaw_joint_servo"] = 0.0
        command_dict["head_pitch_joint_servo"] = 0.0
        client.send_command(command_dict)

    # ── Connect and resolve actuators ───────────────────────────────────────
    deadline = time.monotonic() + 15.0
    while time.monotonic() < deadline and (latest_state is None or actuator_names is None):
        pump()
        pump_observer()
        send_targets(DEFAULT_QPOS)
        time.sleep(0.002)
    if latest_state is None or actuator_names is None:
        print("ERROR: no robot state received")
        return 1
    print(f"[walk] resolved {len(actuator_names)} actuators", flush=True)

    # ── Set default standing pose via admin ─────────────────────────────────
    joint_names_state = list(latest_state.get("joints", {}).keys())
    joint_qpos = [0.0] * len(joint_names_state)
    for i, jn in enumerate(joint_names_state):
        if "shoulder_roll" in jn and jn.startswith("r"):
            joint_qpos[i] = -1.4
        elif "shoulder_roll" in jn and jn.startswith("l"):
            joint_qpos[i] = 1.4
    try:
        admin = AdminClient(args.host, args.admin_port)
        admin.set_pose(args.actor, translation_m=[0.0, 0.0, args.base_height],
                       rotation_quat_xyzw=[0, 0, 0, 1], joint_qpos=joint_qpos)
        admin.close()
    except Exception as e:
        print(f"[walk] WARNING: admin set_pose failed: {e}", flush=True)
    send_targets(DEFAULT_QPOS)
    print("[walk] holding standing pose for 1.0 s", flush=True)
    hold_until = time.monotonic() + 1.0
    while time.monotonic() < hold_until:
        pump()
        pump_observer()
        send_targets(DEFAULT_QPOS)
        time.sleep(0.01)

    # ── Walk ────────────────────────────────────────────────────────────────
    prev_action = np.zeros(18, dtype=np.float32)
    last_policy_sim_time = -1.0
    ctrl_dt = 1.0 / args.policy_hz
    start_x = latest_state["base"]["pos"][0]
    start_sim_time = latest_state.get("sim_time", 0.0)

    print(f"[walk] walking vx={args.vx:.2f} m/s for {args.duration:.1f} s", flush=True)
    end = time.monotonic() + args.duration
    while time.monotonic() < end:
        pump()
        pump_observer()
        send_targets(DEFAULT_QPOS + prev_action * ACTION_SCALES)  # resend to prevent timeout

        if latest_state is None:
            time.sleep(0.001)
            continue

        sim_time = latest_state.get("sim_time", 0.0)
        if sim_time - last_policy_sim_time < ctrl_dt:
            time.sleep(0.001)
            continue
        last_policy_sim_time = sim_time

        # ── Build 63-dim observation ────────────────────────────────────────
        base = latest_state["base"]
        joints = latest_state["joints"]
        quat = np.asarray(base.get("quat", [1, 0, 0, 0]), dtype=np.float64)
        R = quat_to_rotmat(quat)  # body→world
        R_inv = R.T               # world→body

        base_vel = base.get("vel", [0] * 6)
        ang_vel_world = np.asarray(base_vel[3:6] if len(base_vel) >= 6 else [0, 0, 0], dtype=np.float64)
        ang_vel = R_inv @ ang_vel_world           # body frame
        grav_body = R_inv @ np.array([0, 0, -1.0])  # projected gravity

        qpos = np.asarray([joints.get(jn, {}).get("qpos", 0.0) for jn in JOINT_NAMES_URS])
        qvel = np.asarray([joints.get(jn, {}).get("qvel", 0.0) for jn in JOINT_NAMES_URS])

        obs = np.concatenate([
            ang_vel * 0.2,
            grav_body,
            cmd,
            qpos - DEFAULT_QPOS,
            qvel * 0.05,
            prev_action,
        ]).astype(np.float32)

        # ── Policy inference ────────────────────────────────────────────────
        action = policy(obs).astype(np.float64)
        target = np.clip(DEFAULT_QPOS + action * ACTION_SCALES,
                         JOINT_RANGES[:, 0], JOINT_RANGES[:, 1])
        prev_action = action.astype(np.float32)
        send_targets(target)

    # ── Summary ─────────────────────────────────────────────────────────────
    pump()
    pump_observer()
    end_x = latest_state["base"]["pos"][0] if latest_state else start_x
    elapsed_sim = (latest_state.get("sim_time", start_sim_time) - start_sim_time) if latest_state else 0
    print(f"[walk] forward displacement: {end_x - start_x:+.3f} m in {elapsed_sim:.1f} s "
          f"sim time ({(end_x - start_x) / max(elapsed_sim, 0.1):+.2f} m/s)", flush=True)

    client.close()
    if observer:
        observer.close()
    write_video(frames, args.video, args.video_fps)
    print(f"[walk] saved {args.video} ({len(frames)} frames)", flush=True)
    if observer_frames:
        write_video(observer_frames, args.observer_video, args.video_fps)
        print(f"[walk] saved {args.observer_video} ({len(observer_frames)} frames)", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
