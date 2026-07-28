#!/usr/bin/env python3
"""Two-robot face-to-face demo with head yaw/pitch sweep.

Uses the fixed-base pi_plus_stereo_camera_ue model (22 actuators including
head_yaw_joint_servo and head_pitch_joint_servo).  Pure motor-command
control via the per-robot TCP port — no admin API, no lock_pose, no
physics override.  Just like driving a real robot.

The sim must be running with a two-robot scene config, for example
``Config/URS_two_robot_scene.json``, passed with ``-URSSceneConfig``:

    UnrealEditor URSoccerLab.uproject /Game/Levels/URS_SoccerField -game \
        -RenderOffscreen -URSSceneConfig=/path/to/Config/URS_two_robot_scene.json

    cd py_example
    uv run python ue_head_demo.py \
        --host 127.0.0.1 --duration 10 \
        --video0 out/head_demo_rp0.mp4 \
        --video1 out/head_demo_rp1.mp4
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

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from urs_tcp import FrameConn, TYPE_CAMERA, TYPE_JSON  # noqa: E402


class RobotConnection:
    """Per-robot TCP connection: motor commands out, state + camera in."""

    def __init__(self, host: str, port: int):
        self.conn = FrameConn(host, port)
        self.actuator_names: list[str] = []
        self.frames: list[np.ndarray] = []
        self.latest_z: float = 0.0
        self.latest_up: float = 0.0
        self._stop = threading.Event()

    def send_command(self, cmd: dict[str, float]):
        self.conn.send_json(cmd)

    def pump(self):
        """Read available frames.  Learns actuator names from first state."""
        self.conn._try_read()
        buf = self.conn._buf
        while len(buf) >= 5:
            flen = struct.unpack(">I", bytes(buf[:4]))[0]
            if len(buf) < 4 + flen:
                break
            ftype = buf[4]
            payload = bytes(buf[5:4 + flen])
            del buf[:4 + flen]
            if ftype == TYPE_JSON:
                st = json.loads(payload.decode("utf-8"))
                if not self.actuator_names:
                    self.actuator_names = list(st.get("actuators", {}).keys())
                b = st.get("base", {})
                self.latest_z = b.get("pos", [0, 0, 0])[2]
                q = b.get("quat", [1, 0, 0, 0])
                self.latest_up = 1.0 - 2.0 * (q[1] ** 2 + q[2] ** 2)
            elif ftype == TYPE_CAMERA:
                from urs_tcp import parse_camera
                cams = parse_camera(payload)
                if not cams:
                    continue
                # Left eye is camera index 0
                cam0 = cams[0]
                if not cam0["data"]:
                    continue
                from PIL import Image
                import io as _io
                img = Image.open(_io.BytesIO(cam0["data"]))
                self.frames.append(np.array(img.convert("RGB")))

    def close(self):
        self._stop.set()
        self.conn.close()


def default_command(actuator_names: list[str]) -> dict[str, float]:
    """All actuators at 0 — neutral pose for fixed-base model."""
    return {n: 0.0 for n in actuator_names}


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
    ap.add_argument("--robot0-port", type=int, default=10000)
    ap.add_argument("--robot1-port", type=int, default=10001)
    ap.add_argument("--duration", type=float, default=10.0)
    ap.add_argument("--cmd-hz", type=float, default=60.0)
    ap.add_argument("--video-fps", type=int, default=15)
    ap.add_argument("--video0", type=Path, default=Path("py_example/out/head_demo_rp0.mp4"))
    ap.add_argument("--video1", type=Path, default=Path("py_example/out/head_demo_rp1.mp4"))
    args = ap.parse_args()

    rp0 = RobotConnection(args.host, args.robot0_port)
    rp1 = RobotConnection(args.host, args.robot1_port)

    # Wait for first state to learn actuator names
    print("[demo] waiting for state ...", flush=True)
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        rp0.pump()
        rp1.pump()
        if rp0.actuator_names and rp1.actuator_names:
            break
        time.sleep(0.01)

    if not rp0.actuator_names:
        print("[demo] ERROR: no state received", flush=True)
        return 1

    print(f"[demo] {len(rp0.actuator_names)} actuators: {rp0.actuator_names[-4:]}", flush=True)

    # Identify head actuators
    head_yaw_name = next((n for n in rp0.actuator_names if "head_yaw" in n), None)
    head_pitch_name = next((n for n in rp0.actuator_names if "head_pitch" in n), None)
    if not head_yaw_name or not head_pitch_name:
        print(f"[demo] ERROR: head actuators not found in {rp0.actuator_names}", flush=True)
        return 1
    print(f"[demo] head: {head_yaw_name}, {head_pitch_name}", flush=True)

    # Build default commands
    cmd0_default = default_command(rp0.actuator_names)
    cmd1_default = default_command(rp1.actuator_names)

    # Warm up: hold neutral pose for 1s
    print("[demo] warm-up ...", flush=True)
    warmup_end = time.monotonic() + 1.0
    while time.monotonic() < warmup_end:
        rp0.pump(); rp1.pump()
        rp0.send_command(cmd0_default)
        rp1.send_command(cmd1_default)
        time.sleep(0.02)

    # Sweep head yaw/pitch on rp0; rp1 holds still
    print(f"[demo] sweeping head for {args.duration:.0f}s ...", flush=True)
    interval = 1.0 / args.cmd_hz
    t0 = time.monotonic()
    next_cmd = t0
    step = 0

    while time.monotonic() - t0 < args.duration:
        now = time.monotonic()
        if now >= next_cmd:
            rp0.pump(); rp1.pump()

            t = (now - t0) / args.duration
            head_yaw = 0.8 * math.sin(2.0 * math.pi * t)
            head_pitch = 0.4 * math.sin(4.0 * math.pi * t)

            cmd = dict(cmd0_default)
            cmd[head_yaw_name] = head_yaw
            cmd[head_pitch_name] = head_pitch
            rp0.send_command(cmd)
            rp1.send_command(cmd1_default)

            step += 1
            if step % 30 == 0:
                print(f"  t={t:.1f} yaw={math.degrees(head_yaw):+.0f}° "
                      f"pitch={math.degrees(head_pitch):+.0f}° "
                      f"rp0(z={rp0.latest_z:.2f} up={rp0.latest_up:.2f}) "
                      f"rp1(z={rp1.latest_z:.2f} up={rp1.latest_up:.2f}) "
                      f"frames: {len(rp0.frames)} {len(rp1.frames)}", flush=True)
            next_cmd = now + interval
        else:
            # Keep pumping for camera frames between commands
            rp0.pump(); rp1.pump()
            time.sleep(0.001)

    print("[demo] done, saving videos ...", flush=True)
    rp0.close(); rp1.close()
    save_video(rp0.frames, args.video0, args.video_fps)
    save_video(rp1.frames, args.video1, args.video_fps)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
