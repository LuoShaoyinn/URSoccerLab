#!/usr/bin/env python3
"""Capture one or more standing robots, optionally sweeping their heads.

Works with any robot type (pi_plus, mos9, ...) — head actuators are discovered
dynamically from the state's ``actuators`` dict by substring-matching
``head_yaw`` / ``head_pitch``.  Motor command suffix (``_servo`` or ``_motor``)
does not matter.

For torque-controlled robots (motor actuators), pass ``--stabilize`` to enable
a PD controller that holds all non-head joints at a standing pose while the
head sweeps.  Use ``--lock`` to additionally pin the floating base via the
admin API.

Connect to as many robots as the scene provides::

    # single robot (mos9_solo)
    uv run python examples/move_head.py --port 10000 --duration 10 \
        --stabilize --lock --actor robot_mos9

    # two robots (pi_plus, position-servo — no stabilization needed)
    uv run python examples/move_head.py --port 10000 10001 --duration 10
"""
from __future__ import annotations

import argparse
import json
import math
import time
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


class RobotConnection:
    """Per-robot TCP connection: motor commands out, state + camera in."""

    def __init__(self, host: str, port: int):
        self.conn = FrameConn(host, port)
        self.actuator_names: list[str] = []
        self.joint_names: list[str] = []
        self.joint_qpos: dict[str, float] = {}
        self.joint_qvel: dict[str, float] = {}
        self.frames: list[np.ndarray] = []
        self.latest_z: float = 0.0
        self.latest_up: float = 0.0

    def send_command(self, cmd: dict[str, float]):
        self.conn.send_json(cmd)

    def pump(self):
        """Read available frames.  Learns actuator names from first state."""
        for ftype, payload in self.conn.receive_available():
            if ftype == TYPE_JSON:
                st = json.loads(payload.decode("utf-8"))
                if not self.actuator_names:
                    self.actuator_names = list(st.get("actuators", {}).keys())
                if not self.joint_names:
                    self.joint_names = list(st.get("joints", {}).keys())
                joints = st.get("joints", {})
                self.joint_qpos = {k: v.get("qpos", 0.0) for k, v in joints.items()}
                self.joint_qvel = {k: v.get("qvel", 0.0) for k, v in joints.items()}
                b = st.get("base", {})
                self.latest_z = b.get("pos", [0, 0, 0])[2]
                q = b.get("quat", [1, 0, 0, 0])
                self.latest_up = 1.0 - 2.0 * (q[1] ** 2 + q[2] ** 2)
            elif ftype == TYPE_RGB:
                cams = (
                    parse_image_message(payload)
                    if payload and payload[0] == IMAGE_MESSAGE_VERSION
                    else parse_camera(payload)
                )
                if not cams:
                    continue
                cam0 = cams[0]
                if not cam0["data"]:
                    continue
                self.frames.append(camera_to_rgb(cam0))

    def close(self):
        self.conn.close()


def _joint_to_actuator(joint_name: str, actuator_names: list[str]) -> str | None:
    """Find the actuator that drives *joint_name*."""
    for a in actuator_names:
        if joint_name in a:
            return a
    return None


def main(default_mode: str = "sweep") -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, nargs="+", default=[10000],
                    help="one or more robot TCP ports")
    ap.add_argument("--duration", type=float, default=10.0)
    ap.add_argument("--mode", choices=("sweep", "static"), default=default_mode,
                    help="sweep head joints, or capture with no motor commands")
    ap.add_argument("--cmd-hz", type=float, default=60.0)
    ap.add_argument("--video-fps", type=int, default=15)
    ap.add_argument("--video", type=Path, default=Path("out/head_demo"),
                    help="output video prefix (one file per port appended with _N.mp4)")
    # PD stabilization for torque-controlled robots
    ap.add_argument("--stabilize", action="store_true",
                    help="PD-control non-head joints to maintain standing pose")
    ap.add_argument("--kp", type=float, default=40.0, help="PD proportional gain")
    ap.add_argument("--kd", type=float, default=2.0, help="PD derivative gain")
    # Admin base locking
    ap.add_argument("--lock", action="store_true",
                    help="lock floating base via admin API")
    ap.add_argument("--admin-port", type=int, default=11000)
    ap.add_argument("--actor", nargs="+", default=[],
                    help="actor_id per --port for admin lock_pose")
    ap.add_argument("--base-height", type=float, default=0.45,
                    help="base z-height for lock_pose")
    args = ap.parse_args()

    # Lock base IMMEDIATELY (before robot falls) if requested
    admin: AdminClient | None = None
    if args.lock:
        admin = AdminClient(args.host, args.admin_port)
        for i in range(len(args.port)):
            actor_id = args.actor[i] if i < len(args.actor) else f"robot_rp{i}"
            resp = admin.lock_pose(
                actor_id,
                translation_m=[0.0, 0.0, args.base_height],
                rotation_quat_xyzw=[0.0, 0.0, 0.0, 1.0],
            )
            print(f"[demo] lock_pose({actor_id}) at z={args.base_height}: "
                  f"{resp.get('ok', resp)}", flush=True)

    robots = [RobotConnection(args.host, p) for p in args.port]
    n = len(robots)

    # Wait for first state to learn actuator names
    print(f"[demo] waiting for state on {n} robot(s) ...", flush=True)
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        for r in robots:
            r.pump()
        if all(r.actuator_names for r in robots):
            break
        time.sleep(0.01)

    if not all(r.actuator_names for r in robots):
        missing = [args.port[i] for i, r in enumerate(robots) if not r.actuator_names]
        print(f"[demo] ERROR: no state on port(s) {missing}", flush=True)
        return 1

    for i, r in enumerate(robots):
        print(f"[demo] robot {i} (port {args.port[i]}): "
              f"{len(r.actuator_names)} actuators, {len(r.joint_names)} joints",
              flush=True)

    # Discover head actuators per robot
    head_names: list[tuple[str | None, str | None]] = []
    for r in robots:
        hy = next((a for a in r.actuator_names if "head_yaw" in a), None)
        hp = next((a for a in r.actuator_names if "head_pitch" in a), None)
        head_names.append((hy, hp))

    for i, (hy, hp) in enumerate(head_names):
        if args.mode == "sweep" and (not hy or not hp):
            print(f"[demo] WARNING: robot {i} has no head actuators; skipping sweep",
                  flush=True)

    # Build PD joint→actuator mapping for stabilization
    pd_maps: list[dict[str, str]] = []
    if args.stabilize:
        for i, r in enumerate(robots):
            hy, hp = head_names[i]
            head_acts = {a for a in (hy, hp) if a}
            mapping = {}
            for jn in r.joint_names:
                act = _joint_to_actuator(jn, r.actuator_names)
                if act and act not in head_acts:
                    mapping[jn] = act
            pd_maps.append(mapping)
            print(f"[demo] robot {i}: PD stabilizing {len(mapping)} body joints "
                  f"(kp={args.kp}, kd={args.kd})", flush=True)

    print(f"[demo] {args.mode} capture for {args.duration:.0f}s ...", flush=True)
    interval = 1.0 / args.cmd_hz
    t0 = time.monotonic()
    next_cmd = t0
    step = 0

    while time.monotonic() - t0 < args.duration:
        now = time.monotonic()
        if now >= next_cmd:
            for r in robots:
                r.pump()

            t = (now - t0) / args.duration
            head_yaw = 0.0
            head_pitch = 0.0
            if args.mode == "sweep":
                head_yaw = 0.8 * math.sin(2.0 * math.pi * t)
                head_pitch = 0.4 * math.sin(4.0 * math.pi * t)

            for i, r in enumerate(robots):
                cmd: dict[str, float] = {}

                # PD stabilization for body joints
                if args.stabilize and i < len(pd_maps):
                    for jn, act in pd_maps[i].items():
                        qpos = r.joint_qpos.get(jn, 0.0)
                        qvel = r.joint_qvel.get(jn, 0.0)
                        cmd[act] = args.kp * (0.0 - qpos) - args.kd * qvel

                # Head sweep
                hy, hp = head_names[i]
                if hy and hp:
                    sign = 1.0 if i == 0 else -1.0
                    if args.stabilize:
                        # PD toward sweep target
                        jy = hy.replace("_motor", "").replace("_servo", "")
                        jp = hp.replace("_motor", "").replace("_servo", "")
                        cmd[hy] = args.kp * (sign * head_yaw - r.joint_qpos.get(jy, 0.0)) \
                                  - args.kd * r.joint_qvel.get(jy, 0.0)
                        cmd[hp] = args.kp * (head_pitch - r.joint_qpos.get(jp, 0.0)) \
                                  - args.kd * r.joint_qvel.get(jp, 0.0)
                    else:
                        cmd[hy] = sign * head_yaw
                        cmd[hp] = head_pitch

                if cmd:
                    r.send_command(cmd)

            step += 1
            if step % 30 == 0:
                parts = [f"r{i}(z={r.latest_z:.2f} up={r.latest_up:.2f} "
                         f"frames={len(r.frames)})"
                         for i, r in enumerate(robots)]
                print(f"  t={t:.1f} yaw={math.degrees(head_yaw):+.0f}° "
                      f"pitch={math.degrees(head_pitch):+.0f}° "
                      f"{' '.join(parts)}", flush=True)
            next_cmd = now + interval
        else:
            for r in robots:
                r.pump()
            time.sleep(0.001)

    print("[demo] done, saving videos ...", flush=True)
    if admin:
        for i in range(n):
            actor_id = args.actor[i] if i < len(args.actor) else f"robot_rp{i}"
            admin.unlock_pose(actor_id)
        admin.close()
    for r in robots:
        r.close()
    for i, r in enumerate(robots):
        out_path = args.video if n == 1 else args.video.with_name(
            f"{args.video.stem}_{i}{args.video.suffix}")
        write_video(r.frames, out_path, args.video_fps)
        print(f"  robot {i}: {out_path} ({len(r.frames)} frames)", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
