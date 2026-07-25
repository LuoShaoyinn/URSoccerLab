#!/usr/bin/env python3
"""Two-robot face-to-face demo with head yaw/pitch sweep.

Spawns two pi_plus robots facing each other.  Robot rp0 sweeps its
base orientation (yaw + pitch).  Both robots' onboard cameras are
captured to separate video files using background threads.

The sim must be running with Config/URS_scene.json configured for
two robots (robot_rp0 at [-1.5,0,0.39] facing +X, robot_rp1 at
[1.5,0,0.39] facing -X).

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

DEFAULT_JOINT_QPOS = PI_PLUS_DEFAULT_DOF_POS_MUJOCO.tolist()

# Motor command dict (actuator name → target angle) sent via robot TCP port
# to keep PD controllers fed with the SAME targets as the set_pose joint_qpos.
DEFAULT_CMD = {n: float(v) for n, v in zip([
    "l_hip_pitch_joint", "l_hip_roll_joint", "l_thigh_joint", "l_calf_joint",
    "l_ankle_pitch_joint", "l_ankle_roll_joint",
    "l_shoulder_pitch_joint", "l_shoulder_roll_joint", "l_upper_arm_joint", "l_elbow_joint",
    "r_hip_pitch_joint", "r_hip_roll_joint", "r_thigh_joint", "r_calf_joint",
    "r_ankle_pitch_joint", "r_ankle_roll_joint",
    "r_shoulder_pitch_joint", "r_shoulder_roll_joint", "r_upper_arm_joint", "r_elbow_joint",
], DEFAULT_JOINT_QPOS)}

RP0_POS = [-1.5, 0.0, 0.39]
RP1_POS = [1.5, 0.0, 0.39]
RP0_QUAT = [0.0, 0.0, 0.0, 1.0]
RP1_QUAT = [0.0, 0.0, 1.0, 0.0]


def yaw_pitch_quat_xyzw(yaw: float, pitch: float, base_xyzw=None) -> list[float]:
    """Compose base quaternion with yaw (Z) + pitch (Y).  All tuples are xyzw."""
    base = tuple(base_xyzw) if base_xyzw else (0.0, 0.0, 0.0, 1.0)
    cy, sy = math.cos(yaw / 2), math.sin(yaw / 2)
    cp, sp = math.cos(pitch / 2), math.sin(pitch / 2)
    def qmul(a, b):
        aw, ax, ay, az = a[3], a[0], a[1], a[2]
        bw_, bx_, by_, bz_ = b[3], b[0], b[1], b[2]
        return [aw*bx_+ax*bw_+ay*bz_-az*by_,
                aw*by_-ax*bz_+ay*bw_+az*bx_,
                aw*bz_+ax*by_-ay*bx_+az*bw_,
                aw*bw_-ax*bx_-ay*by_-az*bz_]
    qz = (0.0, 0.0, sy, cy)   # yaw around Z in xyzw
    qy = (0.0, sp, 0.0, cp)   # pitch around Y in xyzw
    q = qmul(qmul(base, qz), qy)
    return [q[0], q[1], q[2], q[3]]


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
    ap.add_argument("--video-fps", type=int, default=20)
    ap.add_argument("--video0", type=Path, default=Path("py_example/out/head_demo_rp0.mp4"))
    ap.add_argument("--video1", type=Path, default=Path("py_example/out/head_demo_rp1.mp4"))
    args = ap.parse_args()

    admin = AdminClient(args.host, args.admin_port)
    cmd0 = FrameConn(args.host, args.robot0_port)
    cmd1 = FrameConn(args.host, args.robot1_port)
    print("[demo] locking poses + setting initial ...", flush=True)
    cmd0.send_json(DEFAULT_CMD)
    cmd1.send_json(DEFAULT_CMD)
    # Lock both robots at standing pose — prevents physics from launching them
    admin.lock_pose("robot_rp0", RP0_POS, RP0_QUAT, DEFAULT_JOINT_QPOS)
    admin.lock_pose("robot_rp1", RP1_POS, RP1_QUAT, DEFAULT_JOINT_QPOS)
    time.sleep(1.0)

    cam0 = ThreadedCameraCapture(args.host, args.robot0_port, args.video_fps)
    cam1 = ThreadedCameraCapture(args.host, args.robot1_port, args.video_fps)
    cam0.start()
    cam1.start()
    time.sleep(1.0)

    print(f"[demo] sweeping for {args.duration:.0f}s (pose-locked) ...", flush=True)
    interval = 1.0 / args.pose_hz
    deadline = time.monotonic() + args.duration
    next_pose = time.monotonic()
    step = 0

    while time.monotonic() < deadline:
        now = time.monotonic()
        if now >= next_pose:
            t = 1.0 - (deadline - now) / args.duration
            yaw = 0.8 * math.sin(2.0 * math.pi * t)
            pitch = 0.4 * math.sin(4.0 * math.pi * t)
            quat = yaw_pitch_quat_xyzw(yaw, pitch, RP0_QUAT)
            # Update rp0's lock with the new orientation; rp1 stays locked at fixed pose
            cmd0._try_read(); cmd0._buf.clear()
            cmd1._try_read(); cmd1._buf.clear()
            cmd0.send_json(DEFAULT_CMD)
            cmd1.send_json(DEFAULT_CMD)
            admin.lock_pose("robot_rp0", RP0_POS, quat, DEFAULT_JOINT_QPOS)
            step += 1
            if step % 20 == 0:
                print(f"  t={t:.1f} yaw={math.degrees(yaw):+.0f}° "
                      f"pitch={math.degrees(pitch):+.0f}° "
                      f"rp0(z={cam0.latest_z:.2f} up={cam0.latest_up:.2f}) "
                      f"rp1(z={cam1.latest_z:.2f} up={cam1.latest_up:.2f}) "
                      f"frames: rp0={len(cam0.frames)} rp1={len(cam1.frames)}", flush=True)
            next_pose = now + interval
        else:
            time.sleep(min(0.005, max(0, next_pose - now)))

    print("[demo] stopping cameras ...", flush=True)
    cam0.close()
    cam1.close()
    admin.unlock_pose("robot_rp0")
    admin.unlock_pose("robot_rp1")
    cmd0.close()
    cmd1.close()
    admin.close()

    print(f"[demo] saving videos ...", flush=True)
    save_video(cam0.frames, args.video0, args.video_fps)
    save_video(cam1.frames, args.video1, args.video_fps)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
