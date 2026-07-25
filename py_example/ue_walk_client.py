#!/usr/bin/env python3
"""Drive the Pi Plus walking policy inside the UE simulator via ZMQ.

Connects to the URSoccerLab UE simulator (running the soccer-field map with
a floating-base ``pi_plus_walk`` robot), runs the mos-brain locomotion policy
loop, sends motor torques, and captures the robot's onboard camera view as a
video.

The simulator must already be running (e.g. launched by
``Tools/run_vision_smoke_test.py`` infrastructure or directly with
``UnrealEditor … URS_SoccerField -game``).  This client only handles the ZMQ
communication — no UE process management.

Run from the repository root:

    source /tmp/opencode/walk-venv/bin/activate
    python py_example/ue_walk_client.py \
        --host 127.0.0.1 --robot robot_rp0 \
        --duration 8.0 --vx 0.5 \
        --video py_example/out/ue_walk.mp4
"""

from __future__ import annotations

import argparse
import json
import math
import struct
import sys
import threading
import time
from pathlib import Path
from typing import Any

import numpy as np
import zmq
from PIL import Image

# Reuse the policy + constants from the standalone example.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from walk_pi_plus import (  # noqa: E402
    PI_PLUS_DEFAULT_DOF_POS_MUJOCO,
    PI_PLUS_ISAAC_TO_MUJOCO_IDX,
    PI_PLUS_KP,
    PI_PLUS_KD,
    PI_PLUS_MUJOCO_TO_ISAAC_IDX,
    ACTION_CLIP,
    ACTION_SCALE,
    CMD_CLIP,
    EFFORT_LIMIT,
    OBS_CLIP,
    OBS_HISTORY_LENGTH,
    OBS_STEP_DIM,
    PI_PLUS_POLICY,
    load_policy,
    quat_apply_inverse,
)

MAGIC = 0x4D535255
VERSION = 1


# ---------------------------------------------------------------------------
# ZMQ helpers
# ---------------------------------------------------------------------------

def client_endpoint(bind: str, host: str) -> str:
    return bind.replace("tcp://0.0.0.0:", f"tcp://{host}:").replace("tcp://*:", f"tcp://{host}:")


def encode_motor_command(seq: int, motors: list[float]) -> bytes:
    header = struct.pack("<IHHQdI", MAGIC, VERSION, 0, seq, time.time(), len(motors))
    body = struct.pack(f"<{len(motors)}f", *motors) if motors else b""
    return header + body


def recv_json(sock: zmq.Socket, timeout_ms: int) -> tuple[str, dict[str, Any]]:
    if sock.poll(timeout_ms) == 0:
        raise TimeoutError(f"timed out after {timeout_ms} ms")
    topic, payload = sock.recv_multipart()
    return topic.decode().strip(), json.loads(payload.decode())


def recv_latest_json(sock: zmq.Socket, timeout_ms: int) -> dict[str, Any] | None:
    """Wait for first message (up to timeout), then drain the rest non-blocking."""
    if sock.poll(timeout_ms) == 0:
        return None
    _topic, payload = sock.recv_multipart()
    latest = json.loads(payload.decode())
    while sock.poll(0) > 0:
        _topic, payload = sock.recv_multipart()
        latest = json.loads(payload.decode())
    return latest


def recv_raw(sock: zmq.Socket, timeout_ms: int) -> bytes | None:
    if sock.poll(timeout_ms) == 0:
        return None
    _topic, payload = sock.recv_multipart()
    return payload


# ---------------------------------------------------------------------------
# Quaternion helpers (match mos-brain multi_robot_sim)
# ---------------------------------------------------------------------------

def quat_to_rot_wb(quat_wxyz: np.ndarray) -> np.ndarray:
    """Rotation matrix world-from-body for a (w,x,y,z) quaternion."""
    w, x, y, z = quat_wxyz
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
        [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
        [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)],
    ], dtype=np.float32)


# ---------------------------------------------------------------------------
# Camera capture thread
# ---------------------------------------------------------------------------

class CameraCapture(threading.Thread):
    def __init__(self, ctx: zmq.Context, endpoint: str, host: str, topic: str,
                 width: int, height: int, fps: int):
        super().__init__(name="ue-camera", daemon=True)
        self.endpoint = client_endpoint(endpoint, host)
        self.topic = topic
        self.width = width
        self.height = height
        self.frame_interval = 1.0 / fps
        self.frames: list[np.ndarray] = []
        self.stop_event = threading.Event()

    def run(self) -> None:
        sub = zmq.Socket.__new__(zmq.Socket)  # placeholder for type checker
        sub = zmq.Context().socket(zmq.SUB)
        sub.setsockopt_string(zmq.SUBSCRIBE, self.topic)
        sub.connect(self.endpoint)
        next_frame = time.monotonic()
        try:
            while not self.stop_event.is_set():
                raw = recv_raw(sub, 200)
                if raw is None:
                    continue
                now = time.monotonic()
                if now >= next_frame and len(raw) == self.width * self.height * 4:
                    img = np.frombuffer(raw, dtype=np.uint8).reshape(self.height, self.width, 4)
                    self.frames.append(img[:, :, :3].copy())  # BGRA → BGR
                    next_frame = now + self.frame_interval
        finally:
            sub.close(linger=0)


# ---------------------------------------------------------------------------
# Walking client
# ---------------------------------------------------------------------------

class UEWalkingClient:
    def __init__(self, host: str, robot: str, policy_path: Path, device: str = "cpu"):
        self.host = host
        self.robot = robot
        self.ctx = zmq.Context()
        self.policy, obs_dim, act_dim = load_policy(policy_path, __import__("torch").device(device))
        assert obs_dim == OBS_STEP_DIM * OBS_HISTORY_LENGTH, f"policy obs {obs_dim} != {OBS_STEP_DIM * OBS_HISTORY_LENGTH}"
        assert act_dim == 20, f"policy act {act_dim} != 20"

        self.default_dof = PI_PLUS_DEFAULT_DOF_POS_MUJOCO.copy()
        self.last_action = np.zeros(20, dtype=np.float32)
        self.obs_history = np.zeros(obs_dim, dtype=np.float32)
        self.cmd = np.zeros(3, dtype=np.float32)
        self.seq = 1

        # Populated after meta handshake.
        self.meta: dict[str, Any] = {}
        self.push: zmq.Socket | None = None
        self.state_sub: zmq.Socket | None = None
        self.cam: CameraCapture | None = None
        self.n_actuators = 0

    def set_command(self, vx: float, vy: float, vtheta: float) -> None:
        vx_lim, vy_lim, w_lim = CMD_CLIP
        self.cmd[:] = [
            float(np.clip(vx, -vx_lim, vx_lim)),
            float(np.clip(vy, -vy_lim, vy_lim)),
            float(np.clip(vtheta, -w_lim, w_lim)),
        ]

    def wait_for_meta(self, timeout_ms: int) -> None:
        sub = self.ctx.socket(zmq.SUB)
        sub.setsockopt_string(zmq.SUBSCRIBE, f"meta/{self.robot}")
        sub.connect(f"tcp://{self.host}:10101")
        try:
            _, self.meta = recv_json(sub, timeout_ms)
        finally:
            sub.close(linger=0)

        self.n_actuators = len(self.meta.get("actuator_names", []))
        assert self.n_actuators == 20, f"expected 20 actuators, meta says {self.n_actuators}"
        print(f"[ue-walk] meta: {self.n_actuators} actuators, {len(self.meta.get('cameras', []))} camera(s)")

    def connect_sockets(self) -> None:
        cmd_endpoint = client_endpoint(self.meta["command_endpoint"], self.host)
        self.push = self.ctx.socket(zmq.PUSH)
        self.push.connect(cmd_endpoint)

        state_endpoint = client_endpoint(self.meta["state_endpoint"], self.host)
        self.state_sub = self.ctx.socket(zmq.SUB)
        self.state_sub.setsockopt_string(zmq.SUBSCRIBE, self.meta["state_topic"])
        self.state_sub.connect(state_endpoint)
        # Keep a small buffer so we always read the latest.
        self.state_sub.setsockopt(zmq.RCVHWM, 4)

    def start_camera(self, fps: int) -> bool:
        cams = self.meta.get("cameras", [])
        if not cams:
            print("[ue-walk] WARNING: no camera advertised in metadata")
            return False
        cam = cams[0]
        self.cam = CameraCapture(
            self.ctx, cam["endpoint"], self.host, cam["topic"],
            int(cam["width"]), int(cam["height"]), fps,
        )
        self.cam.start()
        return True

    def _latest_state(self, timeout_ms: int = 500) -> dict[str, Any] | None:
        return recv_latest_json(self.state_sub, timeout_ms)

    def _build_obs(self, state: dict[str, Any]) -> np.ndarray:
        qpos = np.asarray(state["qpos"], dtype=np.float64)
        qvel = np.asarray(state["qvel"], dtype=np.float64)

        # Free-joint layout: qpos[0:7] = [x,y,z, qw,qx,qy,qz]
        #                     qvel[0:6] = [vx,vy,vz, wx,wy,wz]
        quat_wxyz = qpos[3:7].astype(np.float32)
        rot_wb = quat_to_rot_wb(quat_wxyz)

        # Angular velocity: qvel stores world-frame; rotate to body frame.
        ang_world = qvel[3:6].astype(np.float32)
        base_ang = (rot_wb.T @ ang_world).astype(np.float32)

        # Projected gravity in body frame.
        quat_xyzw = np.array([quat_wxyz[1], quat_wxyz[2], quat_wxyz[3], quat_wxyz[0]], dtype=np.float32)
        gravity = quat_apply_inverse(quat_xyzw, np.array([0, 0, -1], dtype=np.float32))

        # Joint positions / velocities (mujoco order → policy order).
        dof_pos = qpos[7:27].astype(np.float32)
        dof_vel = qvel[6:26].astype(np.float32)
        joint_pos = (dof_pos - self.default_dof)[PI_PLUS_MUJOCO_TO_ISAAC_IDX]
        joint_vel = dof_vel[PI_PLUS_MUJOCO_TO_ISAAC_IDX]
        last_action = np.clip(self.last_action, ACTION_CLIP[0], ACTION_CLIP[1])

        obs = np.zeros(OBS_STEP_DIM, dtype=np.float32)
        obs[0:3] = base_ang
        obs[3:6] = gravity
        obs[6:9] = self.cmd
        obs[9:29] = joint_pos
        obs[29:49] = joint_vel
        obs[49:69] = last_action
        return np.nan_to_num(obs, nan=0.0, posinf=0.0, neginf=0.0)

    def _compute_targets(self, state: dict[str, Any]) -> np.ndarray:
        """Run policy, return joint position targets (mujoco order) to send.

        With ``<position>`` actuators the motor-command float IS the target
        angle; MuJoCo applies kp/kv PD at every physics step internally.
        """
        obs_step = self._build_obs(state)
        self.obs_history = np.roll(self.obs_history, -OBS_STEP_DIM)
        self.obs_history[-OBS_STEP_DIM:] = obs_step
        obs_clipped = np.clip(self.obs_history, -OBS_CLIP, OBS_CLIP)

        import torch
        with torch.inference_mode():
            t = torch.from_numpy(obs_clipped).unsqueeze(0)
            action = self.policy(t).detach().numpy().squeeze().astype(np.float32)
        action = np.nan_to_num(action, nan=0.0, posinf=0.0, neginf=0.0)
        action = np.clip(action, ACTION_CLIP[0], ACTION_CLIP[1])
        self.last_action[:] = action

        target = action * ACTION_SCALE
        target_dof = target[PI_PLUS_ISAAC_TO_MUJOCO_IDX] + self.default_dof
        return target_dof.astype(np.float32)

    def run(self, duration: float, policy_hz: float) -> dict[str, Any]:
        interval = 1.0 / policy_hz
        deadline = time.monotonic() + duration
        states_sent = 0
        traj_t, traj_x, traj_y, traj_z, traj_up = [], [], [], [], []

        # Wait for first state so we know the sim is alive.
        first = self._latest_state(5000)
        if first is None:
            raise RuntimeError("no state received within 5 s — is the simulator running?")
        start_x = float(first.get("qpos", [0])[0])
        start_y = float(first.get("qpos", [0] * 27)[1]) if len(first.get("qpos", [])) > 1 else 0.0
        print(f"[ue-walk] first state received, base pos = ({start_x:.2f}, {start_y:.2f})")

        # Warm-up: hold default pose for 1.5 s to let the robot settle.
        default_targets = self.default_dof.tolist()
        warmup_end = time.monotonic() + 1.5
        while time.monotonic() < warmup_end:
            self.push.send(encode_motor_command(self.seq, default_targets))
            self.seq += 1
            time.sleep(0.02)
        print("[ue-walk] warm-up done, starting policy")

        next_policy = time.monotonic()
        traj_start = time.monotonic()
        while time.monotonic() < deadline:
            now = time.monotonic()
            if now >= next_policy:
                state = self._latest_state(500)
                if state is not None:
                    targets = self._compute_targets(state)
                    self.push.send(encode_motor_command(self.seq, targets.tolist()))
                    self.seq += 1
                    states_sent += 1

                    qpos = state.get("qpos", [])
                    if len(qpos) >= 7:
                        qx, qy, qz, qw_ = qpos[4], qpos[5], qpos[6], qpos[3]
                        upright = float(1.0 - 2.0 * (qx * qx + qy * qy))
                        t_rel = now - traj_start
                        traj_t.append(t_rel)
                        traj_x.append(float(qpos[0]) - start_x)
                        traj_y.append(float(qpos[1]) - start_y)
                        traj_z.append(float(qpos[2]))
                        traj_up.append(upright)

                next_policy = now + interval
            else:
                time.sleep(min(0.001, next_policy - now))

        return {
            "states_sent": states_sent,
            "t": np.asarray(traj_t),
            "x": np.asarray(traj_x),
            "y": np.asarray(traj_y),
            "z": np.asarray(traj_z),
            "upright": np.asarray(traj_up),
        }

    def close(self) -> None:
        if self.cam:
            self.cam.stop_event.set()
            self.cam.join(timeout=2.0)
        if self.push:
            self.push.close(linger=0)
        if self.state_sub:
            self.state_sub.close(linger=0)
        self.ctx.term()


def save_video(frames: list[np.ndarray], path: Path, fps: int) -> None:
    if not frames:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    import imageio.v2 as imageio
    writer = imageio.get_writer(str(path), fps=fps, codec="libx264", quality=7,
                                macro_block_size=1)
    for f in frames:
        writer.append_data(f)
    writer.close()
    print(f"[ue-walk] video saved: {path} ({len(frames)} frames)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--robot", default="robot_rp0")
    parser.add_argument("--policy", type=Path, default=PI_PLUS_POLICY)
    parser.add_argument("--duration", type=float, default=8.0)
    parser.add_argument("--vx", type=float, default=0.5)
    parser.add_argument("--vy", type=float, default=0.0)
    parser.add_argument("--vtheta", type=float, default=0.0)
    parser.add_argument("--policy-hz", type=float, default=50.0)
    parser.add_argument("--video-fps", type=int, default=30)
    parser.add_argument("--video", type=Path, default=None)
    parser.add_argument("--trajectory", type=Path, default=None)
    parser.add_argument("--device", default="cpu", choices=["cpu", "cuda"])
    args = parser.parse_args()

    if args.device == "cuda":
        import torch
        if not torch.cuda.is_available():
            print("[ue-walk] CUDA unavailable, using CPU", file=sys.stderr)
            args.device = "cpu"

    client = UEWalkingClient(args.host, args.robot, args.policy, device=args.device)
    client.wait_for_meta(10_000)
    client.connect_sockets()
    has_cam = client.start_camera(args.video_fps)
    client.set_command(args.vx, args.vy, args.vtheta)

    print(f"[ue-walk] walking {args.vx:.2f} m/s for {args.duration:.1f} s @ {args.policy_hz:.0f} Hz")
    try:
        result = client.run(args.duration, args.policy_hz)
    finally:
        client.close()

    print("[ue-walk] ---- summary ----")
    if len(result["x"]) > 0:
        dx = float(result["x"][-1])
        dy = float(result["y"][-1])
        dur = float(result["t"][-1]) if len(result["t"]) > 0 else args.duration
        speed = dx / dur if dur > 0 else 0.0
        min_up = float(result["upright"].min()) if len(result["upright"]) > 0 else 0.0
        print(f"  forward displacement : {dx:+.3f} m   ({speed:+.3f} m/s)")
        print(f"  lateral drift        : {dy:+.3f} m")
        print(f"  min upright          : {min_up:.3f}")
        print(f"  policy steps sent    : {result['states_sent']}")

    if args.trajectory:
        args.trajectory.parent.mkdir(parents=True, exist_ok=True)
        np.savez(args.trajectory, **{k: v for k, v in result.items() if k != "states_sent"},
                 states_sent=result["states_sent"], vx=args.vx, vy=args.vy, vtheta=args.vtheta)
        print(f"[ue-walk] trajectory saved: {args.trajectory}")

    if args.video and client.cam:
        save_video(client.cam.frames, args.video, args.video_fps)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
