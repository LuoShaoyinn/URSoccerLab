#!/usr/bin/env python3
"""Drive the Pi Plus walking policy inside the UE simulator via TCP.

Uses the new modular TCP protocol (one port per robot).  Connects to
port 10000+i, receives state JSON + camera JPEG frames, sends joint
position targets as JSON.

Run from the repository root:

    source /tmp/opencode/walk-venv/bin/activate
    python py_example/ue_walk_tcp.py \
        --host 127.0.0.1 --robot-port 10000 --admin-port 11000 \
        --duration 8.0 --vx 0.5 \
        --video py_example/out/ue_walk_tcp.mp4
"""
from __future__ import annotations

import argparse
import sys
import threading
import time
from pathlib import Path

import numpy as np
import torch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from walk_pi_plus import (  # noqa: E402
    PI_PLUS_DEFAULT_DOF_POS_MUJOCO,
    PI_PLUS_ISAAC_TO_MUJOCO_IDX,
    PI_PLUS_MUJOCO_TO_ISAAC_IDX,
    PI_PLUS_JOINTS_MUJOCO_ORDER,
    ACTION_CLIP,
    ACTION_SCALE,
    CMD_CLIP,
    OBS_CLIP,
    OBS_HISTORY_LENGTH,
    OBS_STEP_DIM,
    PI_PLUS_POLICY,
    load_policy,
    quat_apply_inverse,
)
from urs_tcp import RobotClient, AdminClient, FrameConn, TYPE_CAMERA, TYPE_JSON  # noqa: E402


def quat_to_rot_wb(quat_wxyz: np.ndarray) -> np.ndarray:
    w, x, y, z = quat_wxyz
    return np.array([
        [1 - 2*(y*y+z*z), 2*(x*y-w*z),   2*(x*z+w*y)],
        [2*(x*y+w*z),    1-2*(x*x+z*z), 2*(y*z-w*x)],
        [2*(x*z-w*y),    2*(y*z+w*x),   1-2*(x*x+y*y)],
    ], dtype=np.float32)


class CameraThread(threading.Thread):
    def __init__(self, host: str, port: int, fps: int = 15):
        super().__init__(daemon=True)
        self.conn = FrameConn(host, port)
        self.fps = fps
        self.frames: list[np.ndarray] = []
        self.stop_event = threading.Event()

    def run(self):
        from PIL import Image
        import io as _io
        import struct as _struct
        interval = 1.0 / self.fps
        next_frame = time.monotonic()
        try:
            while not self.stop_event.is_set():
                self.conn._try_read()
                buf = self.conn._buf
                if len(buf) < 5:
                    time.sleep(0.001)
                    continue
                while len(buf) >= 5:
                    flen = _struct.unpack(">I", bytes(buf[:4]))[0]
                    if len(buf) < 4 + flen:
                        break
                    ftype = buf[4]
                    if ftype == TYPE_CAMERA:
                        payload = bytes(buf[5:4+flen])
                        now = time.monotonic()
                        if now >= next_frame:
                            from urs_tcp import parse_camera
                            cams = parse_camera(payload)
                            if cams and cams[0]["data"]:
                                img = Image.open(_io.BytesIO(cams[0]["data"]))
                                self.frames.append(np.array(img.convert("RGB")))
                                next_frame = now + interval
                    del buf[:4+flen]
        except Exception:
            pass

    def close(self):
        self.stop_event.set()
        self.join(timeout=3.0)
        self.conn.close()


class TCPWalkingClient:
    def __init__(self, host: str, robot_port: int, admin_port: int,
                 policy_path: Path, device: str = "cpu"):
        self.admin = AdminClient(host, admin_port)
        self.policy, obs_dim, act_dim = load_policy(policy_path, torch.device(device))
        assert obs_dim == OBS_STEP_DIM * OBS_HISTORY_LENGTH
        assert act_dim == 20

        self.default_dof = PI_PLUS_DEFAULT_DOF_POS_MUJOCO.copy()
        self.last_action = np.zeros(20, dtype=np.float32)
        self.obs_history = np.zeros(obs_dim, dtype=np.float32)
        self.cmd = np.zeros(3, dtype=np.float32)
        self.latest_state: dict | None = None
        self.conn: FrameConn | None = None
        self.cam: CameraThread | None = None

    def connect(self, host: str, robot_port: int):
        self.conn = FrameConn(host, robot_port)

    def start_camera(self, host: str, robot_port: int, fps: int = 15):
        self.cam = CameraThread(host, robot_port, fps)
        self.cam.start()

    @property
    def camera_frames(self) -> list[np.ndarray]:
        return self.cam.frames if self.cam else []

    def set_command(self, vx: float, vy: float, vtheta: float):
        vx_lim, vy_lim, w_lim = CMD_CLIP
        self.cmd[:] = [np.clip(vx, -vx_lim, vx_lim),
                       np.clip(vy, -vy_lim, vy_lim),
                       np.clip(vtheta, -w_lim, w_lim)]

    def _pump(self):
        import json as _json
        import struct as _struct
        conn = self.conn
        if not conn:
            return
        conn._try_read()
        buf = conn._buf
        processed = 0
        while len(buf) >= 5 and processed < 4:
            flen = _struct.unpack(">I", bytes(buf[:4]))[0]
            if len(buf) < 4 + flen:
                break
            ftype = buf[4]
            payload = bytes(buf[5:4 + flen])
            del buf[:4 + flen]
            processed += 1
            if ftype == TYPE_JSON:
                self.latest_state = _json.loads(payload.decode("utf-8"))

    def _build_obs(self, state: dict) -> np.ndarray:
        b = state["base"]
        pos = b["pos"]
        quat_wxyz = np.asarray(b["quat"], dtype=np.float32)  # [w,x,y,z]
        vel = b.get("vel", [0]*6)
        ang_world = np.asarray(vel[3:6], dtype=np.float32)

        rot_wb = quat_to_rot_wb(quat_wxyz)
        base_ang = (rot_wb.T @ ang_world).astype(np.float32)

        quat_xyzw = np.array([quat_wxyz[1], quat_wxyz[2], quat_wxyz[3], quat_wxyz[0]],
                             dtype=np.float32)
        gravity = quat_apply_inverse(quat_xyzw, np.array([0, 0, -1], dtype=np.float32))

        joints = state["joints"]
        dof_pos = np.asarray([joints[n]["qpos"] for n in PI_PLUS_JOINTS_MUJOCO_ORDER],
                             dtype=np.float32)
        dof_vel = np.asarray([joints[n]["qvel"] for n in PI_PLUS_JOINTS_MUJOCO_ORDER],
                             dtype=np.float32)
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

    def _compute_targets(self, state: dict) -> np.ndarray:
        obs_step = self._build_obs(state)
        self.obs_history = np.roll(self.obs_history, -OBS_STEP_DIM)
        self.obs_history[-OBS_STEP_DIM:] = obs_step
        obs_clipped = np.clip(self.obs_history, -OBS_CLIP, OBS_CLIP)

        with torch.inference_mode():
            t = torch.from_numpy(obs_clipped).unsqueeze(0)
            action = self.policy(t).detach().numpy().squeeze().astype(np.float32)
        action = np.nan_to_num(action, nan=0.0, posinf=0.0, neginf=0.0)
        action = np.clip(action, ACTION_CLIP[0], ACTION_CLIP[1])
        self.last_action[:] = action

        target = action * ACTION_SCALE
        target_dof = target[PI_PLUS_ISAAC_TO_MUJOCO_IDX] + self.default_dof
        return target_dof.astype(np.float32)

    def _send_command(self, targets: np.ndarray):
        cmd = {n: float(v) for n, v in zip(PI_PLUS_JOINTS_MUJOCO_ORDER, targets)}
        if self.conn:
            self.conn.send_json(cmd)

    def run(self, duration: float, policy_hz: float, actor_id: str = "robot_rp0") -> dict:
        # Wait for first state while sending warm-up commands
        self.latest_state = None
        self.conn._buf.clear()
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            self._pump()
            self._send_command(self.default_dof)
            if self.latest_state:
                break
            time.sleep(0.005)
        if not self.latest_state:
            raise RuntimeError("no state received — is the simulator running?")

        start_x = self.latest_state["base"]["pos"][0]
        start_y = self.latest_state["base"]["pos"][1]
        print(f"[walk-tcp] first state: base=({start_x:.2f}, {start_y:.2f})")

        # Warm-up: hold default pose
        warmup_end = time.monotonic() + 1.5
        while time.monotonic() < warmup_end:
            self._pump()
            self._send_command(self.default_dof)
            time.sleep(0.02)
        print("[walk-tcp] warm-up done, starting policy")

        interval = 1.0 / policy_hz
        deadline = time.monotonic() + duration
        next_policy = time.monotonic()
        steps = 0
        traj_t, traj_x, traj_y, traj_z, traj_up = [], [], [], [], []

        while time.monotonic() < deadline:
            now = time.monotonic()
            self._pump()
            if now >= next_policy and self.latest_state:
                targets = self._compute_targets(self.latest_state)
                self._send_command(targets)
                steps += 1

                b = self.latest_state["base"]
                pos = b["pos"]
                quat = b["quat"]  # [w,x,y,z]
                upright = 1.0 - 2.0*(quat[1]**2 + quat[2]**2)
                traj_t.append(now - (deadline - duration))
                traj_x.append(pos[0] - start_x)
                traj_y.append(pos[1] - start_y)
                traj_z.append(pos[2])
                traj_up.append(upright)
                next_policy = now + interval
            else:
                time.sleep(min(0.001, max(0, next_policy - now)))

        return {
            "steps": steps,
            "t": np.asarray(traj_t), "x": np.asarray(traj_x),
            "y": np.asarray(traj_y), "z": np.asarray(traj_z),
            "upright": np.asarray(traj_up),
        }

    def close(self):
        if self.cam:
            self.cam.close()
        if self.conn:
            self.conn.close()
        self.admin.close()


def save_video(frames: list[np.ndarray], path: Path, fps: int):
    if not frames:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    import imageio.v2 as imageio
    writer = imageio.get_writer(str(path), fps=fps, codec="libx264", quality=7,
                                macro_block_size=1)
    for f in frames:
        writer.append_data(f)
    writer.close()
    print(f"[walk-tcp] video saved: {path} ({len(frames)} frames)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--robot-port", type=int, default=10000)
    ap.add_argument("--admin-port", type=int, default=11000)
    ap.add_argument("--robot-id", default="robot_rp0")
    ap.add_argument("--policy", type=Path, default=PI_PLUS_POLICY)
    ap.add_argument("--duration", type=float, default=8.0)
    ap.add_argument("--vx", type=float, default=0.5)
    ap.add_argument("--policy-hz", type=float, default=50.0)
    ap.add_argument("--video-fps", type=int, default=15)
    ap.add_argument("--video", type=Path, default=None)
    ap.add_argument("--device", default="cpu", choices=["cpu", "cuda"])
    args = ap.parse_args()

    client = TCPWalkingClient(args.host, args.robot_port, args.admin_port,
                              args.policy, args.device)
    client.connect(args.host, args.robot_port)
    if args.video:
        client.start_camera(args.host, args.robot_port, args.video_fps)
    client.set_command(args.vx, 0.0, 0.0)
    print(f"[walk-tcp] walking {args.vx:.2f} m/s for {args.duration:.1f}s @ {args.policy_hz:.0f}Hz")
    try:
        result = client.run(args.duration, args.policy_hz, args.robot_id)
    finally:
        client.close()

    print("[walk-tcp] ---- summary ----")
    if len(result["x"]) > 0:
        dx = float(result["x"][-1])
        dur = float(result["t"][-1]) if len(result["t"]) > 0 else args.duration
        speed = dx / dur if dur > 0 else 0
        print(f"  forward: {dx:+.3f} m ({speed:+.3f} m/s)")
        print(f"  min upright: {float(result['upright'].min()):.3f}")
        print(f"  steps: {result['steps']}")

    if args.video:
        save_video(client.camera_frames, args.video, args.video_fps)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
