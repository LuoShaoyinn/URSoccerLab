#!/usr/bin/env python3
"""Two-robot face-to-face demo with head yaw/pitch sweep.

The pi_plus_walk MJCF has articulated head_yaw_joint and head_pitch_joint.
This demo sweeps those joints (NOT the base), so the robot body stays
upright while the camera looks around.

Both robots are pose-locked at a standing pose.  Robot rp0's head joints
sweep through yaw/pitch sinuosoids.  Robot rp1 observes.

The sim must be running with Config/URS_scene.json configured for
two robots.

    source /tmp/opencode/walk-venv/bin/activate
    python py_example/ue_head_demo.py \
        --host 127.0.0.1 --duration 10 \
        --video0 py_example/out/head_demo_rp0.mp4 \
        --video1 py_example/out/head_demo_rp1.mp4
"""
from __future__ import annotations

import argparse
import math
import sys
import threading
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from walk_pi_plus import PI_PLUS_DEFAULT_DOF_POS_MUJOCO  # noqa: E402
from urs_tcp import AdminClient, FrameConn, TYPE_CAMERA  # noqa: E402

# 20 leg/arm joints + 2 head joints (yaw, pitch) = 22 total
DEFAULT_JOINT_QPOS = PI_PLUS_DEFAULT_DOF_POS_MUJOCO.tolist() + [0.0, 0.0]

# Motor command for the 20 actuated joints (head joints have no actuators)
JOINT_NAMES = [
    "l_hip_pitch_joint", "l_hip_roll_joint", "l_thigh_joint", "l_calf_joint",
    "l_ankle_pitch_joint", "l_ankle_roll_joint",
    "l_shoulder_pitch_joint", "l_shoulder_roll_joint", "l_upper_arm_joint", "l_elbow_joint",
    "r_hip_pitch_joint", "r_hip_roll_joint", "r_thigh_joint", "r_calf_joint",
    "r_ankle_pitch_joint", "r_ankle_roll_joint",
    "r_shoulder_pitch_joint", "r_shoulder_roll_joint", "r_upper_arm_joint", "r_elbow_joint",
]
DEFAULT_CMD = {n: float(v) for n, v in zip(JOINT_NAMES, PI_PLUS_DEFAULT_DOF_POS_MUJOCO.tolist())}

RP0_POS = [-1.5, 0.0, 0.39]
RP1_POS = [1.5, 0.0, 0.39]
RP0_QUAT = [0.0, 0.0, 0.0, 1.0]
RP1_QUAT = [0.0, 0.0, 1.0, 0.0]


class ThreadedCameraCapture(threading.Thread):
    def __init__(self, host: str, port: int, fps: int = 15):
        super().__init__(daemon=True)
        self.conn = FrameConn(host, port)
        self.fps = fps
        self.frames: list[np.ndarray] = []
        self.stop_event = threading.Event()
        self.latest_z: float = 0.0
        self.latest_up: float = 0.0

    def run(self):
        from PIL import Image
        import io as _io
        import struct as _struct
        import json as _json
        interval = 1.0 / self.fps
        next_frame = time.monotonic()
        try:
            while not self.stop_event.is_set():
                self.conn._try_read()
                buf = self.conn._buf
                while len(buf) >= 5:
                    flen = _struct.unpack(">I", bytes(buf[:4]))[0]
                    if len(buf) < 4 + flen:
                        break
                    ftype = buf[4]
                    payload = bytes(buf[5:4+flen])
                    del buf[:4+flen]
                    if ftype == TYPE_CAMERA:
                        now = time.monotonic()
                        if now >= next_frame:
                            img = Image.open(_io.BytesIO(payload[6:]))
                            self.frames.append(np.array(img.convert("RGB")))
                            next_frame = now + interval
                    elif ftype == 0x00:
                        try:
                            st = _json.loads(payload.decode("utf-8"))
                            b = st.get("base", {})
                            self.latest_z = b.get("pos", [0,0,0])[2]
                            q = b.get("quat", [1,0,0,0])
                            self.latest_up = 1.0 - 2.0*(q[1]**2 + q[2]**2)
                        except Exception:
                            pass
        except Exception:
            pass

    def close(self):
        self.stop_event.set()
        self.join(timeout=3.0)
        self.conn.close()


def save_video(frames: list[np.ndarray], path: Path, fps: int):
    if not frames:
        print(f"  no frames for {path}")
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    import imageio.v2 as imageio
    writer = imageio.get_writer(str(path), fps=fps, codec="libx264", quality=7,
                                macro_block_size=1)
    for f in frames:
        writer.append_data(f)
    writer.close()
    print(f"  saved {path} ({len(frames)} frames)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--admin-port", type=int, default=11000)
    ap.add_argument("--robot0-port", type=int, default=10000)
    ap.add_argument("--robot1-port", type=int, default=10001)
    ap.add_argument("--duration", type=float, default=10.0)
    ap.add_argument("--pose-hz", type=float, default=20.0)
    ap.add_argument("--video-fps", type=int, default=15)
    ap.add_argument("--video0", type=Path, default=Path("py_example/out/head_demo_rp0.mp4"))
    ap.add_argument("--video1", type=Path, default=Path("py_example/out/head_demo_rp1.mp4"))
    args = ap.parse_args()

    admin = AdminClient(args.host, args.admin_port)
    cmd0 = FrameConn(args.host, args.robot0_port)
    cmd1 = FrameConn(args.host, args.robot1_port)

    print("[demo] locking both robots at standing pose ...", flush=True)
    cmd0.send_json(DEFAULT_CMD)
    cmd1.send_json(DEFAULT_CMD)
    # Lock body upright (identity rotation), all joints at default
    admin.lock_pose("robot_rp0", RP0_POS, RP0_QUAT, DEFAULT_JOINT_QPOS)
    admin.lock_pose("robot_rp1", RP1_POS, RP1_QUAT, DEFAULT_JOINT_QPOS)
    time.sleep(1.0)

    cam0 = ThreadedCameraCapture(args.host, args.robot0_port, args.video_fps)
    cam1 = ThreadedCameraCapture(args.host, args.robot1_port, args.video_fps)
    cam0.start()
    cam1.start()
    time.sleep(1.0)

    print(f"[demo] sweeping HEAD yaw/pitch for {args.duration:.0f}s ...", flush=True)
    interval = 1.0 / args.pose_hz
    deadline = time.monotonic() + args.duration
    next_pose = time.monotonic()
    step = 0

    while time.monotonic() < deadline:
        now = time.monotonic()
        if now >= next_pose:
            t = 1.0 - (deadline - now) / args.duration
            head_yaw = 0.8 * math.sin(2.0 * math.pi * t)
            head_pitch = 0.4 * math.sin(4.0 * math.pi * t)

            # Update rp0's lock: same body pose, different head joint angles
            jq = PI_PLUS_DEFAULT_DOF_POS_MUJOCO.tolist() + [head_yaw, head_pitch]
            cmd0._try_read(); cmd0._buf.clear()
            cmd1._try_read(); cmd1._buf.clear()
            cmd0.send_json(DEFAULT_CMD)
            cmd1.send_json(DEFAULT_CMD)
            admin.lock_pose("robot_rp0", RP0_POS, RP0_QUAT, jq)

            step += 1
            if step % 20 == 0:
                print(f"  t={t:.1f} head_yaw={math.degrees(head_yaw):+.0f}° "
                      f"head_pitch={math.degrees(head_pitch):+.0f}° "
                      f"rp0(z={cam0.latest_z:.2f} up={cam0.latest_up:.2f}) "
                      f"rp1(z={cam1.latest_z:.2f} up={cam1.latest_up:.2f}) "
                      f"frames: rp0={len(cam0.frames)} rp1={len(cam1.frames)}", flush=True)
            next_pose = now + interval
        else:
            time.sleep(min(0.005, max(0, next_pose - now)))

    print("[demo] stopping ...", flush=True)
    cam0.close()
    cam1.close()
    admin.unlock_pose("robot_rp0")
    admin.unlock_pose("robot_rp1")
    cmd0.close()
    cmd1.close()
    admin.close()

    print("[demo] saving videos ...", flush=True)
    save_video(cam0.frames, args.video0, args.video_fps)
    save_video(cam1.frames, args.video1, args.video_fps)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
