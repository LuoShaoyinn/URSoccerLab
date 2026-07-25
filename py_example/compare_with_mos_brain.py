#!/usr/bin/env python3
"""Compare the pure-Python walk example against the mos-brain origin pipeline.

Drives the *actual* ``MultiRobotMujocoSim`` from ``refs/mos-brain`` (the same
code the mos-brain launcher uses) for a single Pi Plus robot walking forward,
then compares its base trajectory against the ``walk_pi_plus.py`` trajectory.

Both runs use the same velocity command and duration. Because mos-brain spawns
its robot on the soccer pitch (different floor friction / scene from the
standalone pi_plus.xml), the absolute positions differ but the *macro* walking
behaviour (forward progress, stability, gait) should match closely.

Run from the repository root:

    source /tmp/opencode/walk-venv/bin/activate
    python py_example/compare_with_mos_brain.py --duration 5.0 --vx 0.5
"""

from __future__ import annotations

import argparse
import os
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np

os.environ.setdefault("MUJOCO_GL", "egl")

import mujoco
import torch

REPO_ROOT = Path(__file__).resolve().parents[1]
MOS_BRAIN = REPO_ROOT / "refs" / "mos-brain" / "simulation" / "mujoco"
sys.path.insert(0, str(MOS_BRAIN))


def _load_mos_brain_sim():
    """Import mos-brain's MultiRobotMujocoSim + config builder."""
    from app.multi_robot_sim import MultiRobotMujocoSim  # noqa: F401
    from app.runtime_config import RuntimeArgs, build_robot_runtime_config
    return MultiRobotMujocoSim, RuntimeArgs, build_robot_runtime_config


@dataclass
class Trajectory:
    t: np.ndarray
    x: np.ndarray
    y: np.ndarray
    z: np.ndarray
    upright: np.ndarray
    qpos: np.ndarray


def run_mos_brain_reference(duration: float, vx: float, vy: float, vtheta: float) -> Trajectory:
    MultiRobotMujocoSim, RuntimeArgs, build_robot_runtime_config = _load_mos_brain_sim()

    robot_cfg = build_robot_runtime_config(MOS_BRAIN, robot_type="pi_plus", policy_override=None, robot_xml_override=None)
    args = RuntimeArgs(
        robot_type=robot_cfg.robot_type,
        robot_cfg=robot_cfg,
        policy=robot_cfg.policy,
        robot_xml=robot_cfg.robot_xml,
        soccer_world_xml=MOS_BRAIN / "assets" / "environments" / "soccer" / "world.xml",
        match_config=MOS_BRAIN / "assets" / "config" / "match_config.json",
        webview=False,
        zmq=True,
        webview_port=0,
        web_fps=20,
        web_width=480,
        web_height=360,
        render_collision_meshes=False,
        allow_keyboard_control=False,
        port=0,
        team_size=1,
        max_red_robots=1,
        max_blue_robots=0,
        use_referee=False,
        policy_device="cpu",
        real_time=False,
    )

    sim = MultiRobotMujocoSim(args)
    sim_dt = float(sim.sim_dt)
    n_steps = int(round(duration / sim_dt))
    every = max(1, round(1.0 / (sim_dt * 30.0)))

    rid = 0
    spec = sim.robot_specs[rid]
    # Place the reference robot at the origin facing +X (identity yaw) so its
    # world-frame trajectory is directly comparable to walk_pi_plus.py, which
    # loads the raw pi_plus.xml at the same pose. This overrides the field
    # spawn (yaw/position) coming from the match config.
    sim.data.qpos[spec.base_qpos_adr + 0] = 0.0
    sim.data.qpos[spec.base_qpos_adr + 1] = 0.0
    sim.data.qpos[spec.base_qpos_adr + 3 : spec.base_qpos_adr + 7] = [1.0, 0.0, 0.0, 0.0]
    sim.data.qvel[spec.base_qvel_adr : spec.base_qvel_adr + 6] = 0.0
    mujoco.mj_forward(sim.model, sim.data)
    sim.set_command(float(vx), float(vy), float(vtheta), robot_id=rid, timestamp=1.0, source="compare")

    t_list, x_list, y_list, z_list, up_list, qp_list = [], [], [], [], [], []
    counter = 0
    for i in range(n_steps):
        counter = sim._step_once(counter)
        if i % every == 0:
            qp = sim.data.qpos.copy()
            t_list.append(i * sim_dt)
            x_list.append(float(qp[spec.base_qpos_adr]))
            y_list.append(float(qp[spec.base_qpos_adr + 1]))
            z_list.append(float(qp[spec.base_qpos_adr + 2]))
            qw, qx, qy, qz = qp[spec.base_qpos_adr + 3 : spec.base_qpos_adr + 7]
            up_list.append(float(1.0 - 2.0 * (qx * qx + qy * qy)))
            qp_list.append(qp)

    return Trajectory(
        t=np.asarray(t_list), x=np.asarray(x_list), y=np.asarray(y_list),
        z=np.asarray(z_list), upright=np.asarray(up_list), qpos=np.asarray(qp_list),
    )


def load_example_trajectory(path: Path) -> Trajectory:
    d = np.load(path)
    upright = d["upright"]
    return Trajectory(
        t=d["t"], x=d["x"] - float(d["x"][0]), y=d["y"] - float(d["y"][0]),
        z=d["z"], upright=upright, qpos=d["qpos"],
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--duration", type=float, default=5.0)
    parser.add_argument("--vx", type=float, default=0.5)
    parser.add_argument("--vy", type=float, default=0.0)
    parser.add_argument("--vtheta", type=float, default=0.0)
    parser.add_argument("--example-trajectory", type=Path, default=None,
                        help="npz from walk_pi_plus.py; if absent it is generated first")
    args = parser.parse_args()

    if args.example_trajectory is None:
        import subprocess
        traj_path = REPO_ROOT / "py_example" / "out" / "compare_example.npz"
        traj_path.parent.mkdir(parents=True, exist_ok=True)
        cmd = [
            sys.executable, str(REPO_ROOT / "py_example" / "walk_pi_plus.py"),
            "--duration", str(args.duration), "--vx", str(args.vx),
            "--vy", str(args.vy), "--vtheta", str(args.vtheta),
            "--no-video", "--trajectory", str(traj_path),
        ]
        print("[compare] generating example trajectory:", " ".join(cmd))
        subprocess.run(cmd, check=True)
        args.example_trajectory = traj_path

    print("[compare] running mos-brain MultiRobotMujocoSim reference ...")
    ref = run_mos_brain_reference(args.duration, args.vx, args.vy, args.vtheta)
    ex = load_example_trajectory(args.example_trajectory)

    def stats(traj: Trajectory, label: str) -> None:
        dx = float(traj.x[-1] - traj.x[0])
        dy = float(traj.y[-1] - traj.y[0])
        dur = float(traj.t[-1] - traj.t[0])
        print(f"[compare] {label}:")
        print(f"    forward  = {dx:+.3f} m   ({dx / dur:+.3f} m/s)")
        print(f"    lateral  = {dy:+.3f} m")
        print(f"    min up   = {float(traj.upright.min()):.3f}")
        print(f"    z range  = [{float(traj.z.min()):.3f}, {float(traj.z.max()):.3f}]")

    stats(ref, "mos-brain reference")
    stats(ex, "pure-python example")

    ref_dx = float(ref.x[-1] - ref.x[0])
    ex_dx = float(ex.x[-1] - ex.x[0])
    ref_speed = ref_dx / float(ref.t[-1])
    ex_speed = ex_dx / float(ex.t[-1])
    speed_rel_err = abs(ex_speed - ref_speed) / max(abs(ref_speed), 1e-6)
    both_upright = bool(ref.upright.min() > 0.5 and ex.upright.min() > 0.5)
    same_direction = (ref_dx > 0) == (ex_dx > 0) and abs(ref_dx) > 0.05

    print("[compare] ---- verdict ----")
    print(f"    reference speed = {ref_speed:+.3f} m/s, example speed = {ex_speed:+.3f} m/s")
    print(f"    relative speed error = {speed_rel_err * 100:.1f}%")
    print(f"    both walked forward  : {same_direction}")
    print(f"    both stayed upright  : {both_upright}")

    ok = same_direction and both_upright and speed_rel_err < 0.35
    print(f"    => {'MATCH (within tolerance)' if ok else 'MISMATCH'}")
    return 0 if ok else 2


if __name__ == "__main__":
    raise SystemExit(main())
