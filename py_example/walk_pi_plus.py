#!/usr/bin/env python3
"""Pure-Python bipedal walking example for the Pi Plus humanoid.

This is a standalone example that demonstrates how to drive the mos-brain
locomotion policy (``pi_plus_model_40000.pt``) against the Pi Plus MuJoCo
model. The simulator itself ships no gait/locomotion logic; everything needed
to make the robot walk is implemented here in plain Python so it can be used as
a reference for writing your own decider/clients.

The observation assembly, action processing and PD control faithfully mirror
the pi_plus pipeline inside ``refs/mos-brain/.../app/multi_robot_sim.py`` so
that this example reproduces the mos-brain walking behaviour.

Run from the repository root:

    source /tmp/opencode/walk-venv/bin/activate
    python py_example/walk_pi_plus.py \
        --duration 6.0 --vx 0.5 \
        --video out/walk.mp4 --trajectory out/walk.npz

A negative ``--vx`` walks backwards. Leave ``--vx/vy/vtheta`` at 0 to test
balancing in place.
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from pathlib import Path

import numpy as np

os.environ.setdefault("MUJOCO_GL", "egl")

import mujoco
import torch

REPO_ROOT = Path(__file__).resolve().parents[1]
MOS_BRAIN = REPO_ROOT / "refs" / "mos-brain" / "simulation" / "mujoco"
PI_PLUS_XML = MOS_BRAIN / "assets" / "robots" / "pi_plus" / "pi_plus.xml"
PI_PLUS_POLICY = MOS_BRAIN / "assets" / "policies" / "pi_plus_model_40000.pt"

# --- mos-brain pi_plus constants (copied from app/multi_robot_sim.py) ---------
# 20 actuated joints in the order they are declared in pi_plus.xml.
PI_PLUS_JOINTS_MUJOCO_ORDER = [
    "l_hip_pitch_joint", "l_hip_roll_joint", "l_thigh_joint", "l_calf_joint",
    "l_ankle_pitch_joint", "l_ankle_roll_joint",
    "l_shoulder_pitch_joint", "l_shoulder_roll_joint", "l_upper_arm_joint", "l_elbow_joint",
    "r_hip_pitch_joint", "r_hip_roll_joint", "r_thigh_joint", "r_calf_joint",
    "r_ankle_pitch_joint", "r_ankle_roll_joint",
    "r_shoulder_pitch_joint", "r_shoulder_roll_joint", "r_upper_arm_joint", "r_elbow_joint",
]

# Policy (Isaac) joint order. Observations are fed to the network in this order.
PI_PLUS_JOINTS_POLICY_ORDER = [
    "l_hip_pitch_joint", "l_shoulder_pitch_joint", "r_hip_pitch_joint", "r_shoulder_pitch_joint",
    "l_hip_roll_joint", "l_shoulder_roll_joint", "r_hip_roll_joint", "r_shoulder_roll_joint",
    "l_thigh_joint", "l_upper_arm_joint", "r_thigh_joint", "r_upper_arm_joint",
    "l_calf_joint", "l_elbow_joint", "r_calf_joint", "r_elbow_joint",
    "l_ankle_pitch_joint", "r_ankle_pitch_joint", "l_ankle_roll_joint", "r_ankle_roll_joint",
]

# isaac[j] -> mujoco index, i.e. target_dof_pos_mujoco = scaled[ISAAC_TO_MUJOCO]
PI_PLUS_ISAAC_TO_MUJOCO_IDX = np.asarray(
    [0, 4, 8, 12, 16, 18, 1, 5, 9, 13, 2, 6, 10, 14, 17, 19, 3, 7, 11, 15], dtype=np.int32,
)
# mujoco[j] -> isaac index, i.e. obs_joint = dof[mUJOCO_TO_ISAAC]
PI_PLUS_MUJOCO_TO_ISAAC_IDX = np.asarray(
    [0, 6, 10, 16, 1, 7, 11, 17, 2, 8, 12, 18, 3, 9, 13, 19, 4, 14, 5, 15], dtype=np.int32,
)

PI_PLUS_DEFAULT_DOF_POS_MUJOCO = np.asarray(
    [-0.25, 0.0, 0.0, 0.65, -0.4, 0.0, 0.0, 0.2, 0.0, -1.2,
     -0.25, 0.0, 0.0, 0.65, -0.4, 0.0, 0.0, -0.2, 0.0, -1.2],
    dtype=np.float32,
)

# PD gains applied element-wise to mujoco-order arrays (matches mos-brain).
PI_PLUS_KP = np.asarray(
    [80.0, 80.0, 80.0, 80.0, 60.0, 60.0, 30.0, 30.0, 30.0, 30.0,
     80.0, 80.0, 80.0, 80.0, 60.0, 60.0, 30.0, 30.0, 30.0, 30.0],
    dtype=np.float32,
)
PI_PLUS_KD = np.asarray(
    [1.1, 1.1, 1.1, 1.1, 1.2, 1.2, 0.6, 0.6, 0.6, 0.6,
     1.1, 1.1, 1.1, 1.1, 1.2, 1.2, 0.6, 0.6, 0.6, 0.6],
    dtype=np.float32,
)

ACTION_SCALE = 0.25
ACTION_CLIP = (-100.0, 100.0)
EFFORT_LIMIT = 20.0
SIM_DT = 0.002
CONTROL_DECIMATION = 10
OBS_HISTORY_LENGTH = 5
OBS_CLIP = 100.0
OBS_STEP_DIM = 69  # 3 ang + 3 grav + 3 cmd + 20*3
CMD_CLIP = (1.5, 1.0, 3.0)


def quat_apply_inverse(q_xyzw: np.ndarray, v: np.ndarray) -> np.ndarray:
    """Rotate ``v`` by the inverse of quaternion ``q`` (xyzw). Mirrors mos-brain."""
    q_w = q_xyzw[-1]
    q_vec = q_xyzw[:3]
    a = v * (2.0 * q_w * q_w - 1.0)
    b = np.cross(q_vec, v) * q_w * 2.0
    c = q_vec * np.dot(q_vec, v) * 2.0
    return a - b + c


class MLPActor(torch.nn.Module):
    """Rebuild the actor MLP from a checkpoint's actor.* weights."""

    def __init__(self, layer_dims: list[int]):
        super().__init__()
        layers: list[torch.nn.Module] = []
        for i in range(len(layer_dims) - 1):
            layers.append(torch.nn.Linear(layer_dims[i], layer_dims[i + 1]))
            if i < len(layer_dims) - 2:
                layers.append(torch.nn.ELU())
        self.actor = torch.nn.Sequential(*layers)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return self.actor(x)


def _ensure_rsl_rl_stub() -> None:
    """pi_plus checkpoints pickle an rsl_rl Normalizer; stub the module if absent."""
    if "rsl_rl.utils.utils" in sys.modules:
        return
    try:
        import rsl_rl.utils.utils  # noqa: F401
        return
    except ModuleNotFoundError:
        pass
    import types

    rsl_rl = sys.modules.setdefault("rsl_rl", types.ModuleType("rsl_rl"))
    utils_pkg = sys.modules.setdefault("rsl_rl.utils", types.ModuleType("rsl_rl.utils"))
    utils_mod = types.ModuleType("rsl_rl.utils.utils")

    class Normalizer:
        pass

    utils_mod.Normalizer = Normalizer
    sys.modules["rsl_rl.utils.utils"] = utils_mod
    rsl_rl.utils = utils_pkg
    utils_pkg.utils = utils_mod


def load_policy(path: Path, device: torch.device) -> tuple[torch.nn.Module, int, int]:
    _ensure_rsl_rl_stub()
    ckpt = torch.load(path, map_location=device, weights_only=False)
    state = ckpt.get("model_state_dict", ckpt) if isinstance(ckpt, dict) else ckpt
    actor_state = {k: v for k, v in state.items() if k.startswith("actor.")}
    weight_keys = sorted(
        (k for k in actor_state if k.endswith(".weight")),
        key=lambda s: int(s.split(".")[1]),
    )
    dims: list[int] = []
    for i, wk in enumerate(weight_keys):
        w = actor_state[wk]
        if i == 0:
            dims.append(int(w.shape[1]))
        dims.append(int(w.shape[0]))
    policy = MLPActor(dims).to(device)
    policy.load_state_dict(actor_state, strict=True)
    policy.eval()
    return policy, dims[0], dims[-1]


class WalkingPiPlus:
    """Drives the Pi Plus model with the mos-brain locomotion policy."""

    def __init__(self, xml_path: Path, policy_path: Path, device: str = "cpu"):
        self.model = mujoco.MjModel.from_xml_path(str(xml_path))
        self.data = mujoco.MjData(self.model)
        self.model.opt.timestep = SIM_DT

        self.qpos_idx = self._resolve_joint_qpos(PI_PLUS_JOINTS_MUJOCO_ORDER)
        self.qvel_idx = self._resolve_joint_qvel(PI_PLUS_JOINTS_MUJOCO_ORDER)

        self.base_qpos_adr = 0
        self.base_qvel_adr = 0

        self.policy, self.obs_dim, self.act_dim = load_policy(policy_path, torch.device(device))
        expected_obs = OBS_STEP_DIM * OBS_HISTORY_LENGTH
        if self.obs_dim != expected_obs:
            raise RuntimeError(f"policy expects obs dim {self.obs_dim}, example builds {expected_obs}")
        if self.act_dim != len(PI_PLUS_JOINTS_POLICY_ORDER):
            raise RuntimeError(f"policy action dim {self.act_dim} != 20")

        self.default_dof_pos = PI_PLUS_DEFAULT_DOF_POS_MUJOCO
        self.last_action = np.zeros(20, dtype=np.float32)
        self.target_dof_pos = self.default_dof_pos.copy()
        self.obs_history = np.zeros(expected_obs, dtype=np.float32)
        self.cmd = np.zeros(3, dtype=np.float32)
        self._has_ang_sensor = self._sensor_id("angular-velocity") is not None
        self._has_ori_sensor = self._sensor_id("orientation") is not None

    def _sensor_id(self, name: str) -> int | None:
        sid = mujoco.mj_name2id(self.model, mujoco.mjtObj.mjOBJ_SENSOR, name)
        return int(sid) if sid >= 0 else None

    def _resolve_joint_qpos(self, names: list[str]) -> np.ndarray:
        idx = []
        for n in names:
            jid = mujoco.mj_name2id(self.model, mujoco.mjtObj.mjOBJ_JOINT, n)
            if jid < 0:
                raise RuntimeError(f"joint not in model: {n}")
            idx.append(int(self.model.jnt_qposadr[jid]))
        return np.asarray(idx, dtype=np.int32)

    def _resolve_joint_qvel(self, names: list[str]) -> np.ndarray:
        idx = []
        for n in names:
            jid = mujoco.mj_name2id(self.model, mujoco.mjtObj.mjOBJ_JOINT, n)
            if jid < 0:
                raise RuntimeError(f"joint not in model: {n}")
            idx.append(int(self.model.jnt_dofadr[jid]))
        return np.asarray(idx, dtype=np.int32)

    def reset(self, spawn_xy: tuple[float, float] | None = None, yaw: float = 0.0) -> None:
        mujoco.mj_resetData(self.model, self.data)
        self.data.qpos[7:27] = self.default_dof_pos
        if spawn_xy is not None:
            self.data.qpos[0] = float(spawn_xy[0])
            self.data.qpos[1] = float(spawn_xy[1])
        half = 0.5 * float(yaw)
        self.data.qpos[3:7] = [np.cos(half), 0.0, 0.0, np.sin(half)]
        self.last_action[:] = 0.0
        self.target_dof_pos[:] = self.default_dof_pos
        self.obs_history[:] = 0.0
        mujoco.mj_forward(self.model, self.data)

    def set_command(self, vx: float, vy: float, vtheta: float) -> None:
        vx_lim, vy_lim, w_lim = CMD_CLIP
        self.cmd[:] = [
            float(np.clip(vx, -vx_lim, vx_lim)),
            float(np.clip(vy, -vy_lim, vy_lim)),
            float(np.clip(vtheta, -w_lim, w_lim)),
        ]

    def _build_obs_step(self) -> np.ndarray:
        qpos = self.data.qpos
        qvel = self.data.qvel

        if self._has_ang_sensor:
            base_ang = self.data.sensor("angular-velocity").data.astype(np.float32)
        else:
            base_ang = qvel[self.base_qvel_adr + 3 : self.base_qvel_adr + 6].astype(np.float32)

        if self._has_ori_sensor:
            ori_wxyz = self.data.sensor("orientation").data.astype(np.float32)
            quat_xyzw = np.asarray([ori_wxyz[1], ori_wxyz[2], ori_wxyz[3], ori_wxyz[0]], dtype=np.float32)
        else:
            quat_wxyz = qpos[self.base_qpos_adr + 3 : self.base_qpos_adr + 7]
            quat_xyzw = np.asarray([quat_wxyz[1], quat_wxyz[2], quat_wxyz[3], quat_wxyz[0]], dtype=np.float32)
        gravity = quat_apply_inverse(quat_xyzw, np.array([0.0, 0.0, -1.0], dtype=np.float32)).astype(np.float32)

        dof_pos = qpos[self.qpos_idx].astype(np.float32)
        dof_vel = qvel[self.qvel_idx].astype(np.float32)
        joint_pos = (dof_pos - self.default_dof_pos)[PI_PLUS_MUJOCO_TO_ISAAC_IDX]
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

    def policy_step(self) -> None:
        obs_step = self._build_obs_step()
        # Roll history buffer left by OBS_STEP_DIM and append the newest step.
        self.obs_history = np.roll(self.obs_history, shift=-OBS_STEP_DIM)
        self.obs_history[-OBS_STEP_DIM:] = obs_step
        obs_clipped = np.clip(self.obs_history, -OBS_CLIP, OBS_CLIP)

        with torch.inference_mode():
            t = torch.from_numpy(obs_clipped).unsqueeze(0)
            action = self.policy(t).detach().numpy().squeeze().astype(np.float32)
        action = np.nan_to_num(action, nan=0.0, posinf=0.0, neginf=0.0)
        action = np.clip(action, ACTION_CLIP[0], ACTION_CLIP[1])
        self.last_action[:] = action
        scaled = action * ACTION_SCALE
        self.target_dof_pos[:] = scaled[PI_PLUS_ISAAC_TO_MUJOCO_IDX] + self.default_dof_pos

    def apply_pd(self) -> None:
        q = self.data.qpos[self.qpos_idx]
        qd = self.data.qvel[self.qvel_idx]
        q = np.nan_to_num(q, nan=0.0, posinf=0.0, neginf=0.0)
        qd = np.nan_to_num(qd, nan=0.0, posinf=0.0, neginf=0.0)
        target = np.nan_to_num(self.target_dof_pos, nan=0.0, posinf=0.0, neginf=0.0)
        tau = PI_PLUS_KP * (target - q) + PI_PLUS_KD * (0.0 - qd)
        tau = np.clip(tau, -EFFORT_LIMIT, EFFORT_LIMIT)
        self.data.ctrl[:] = tau

    def step(self) -> None:
        self.apply_pd()
        mujoco.mj_step(self.model, self.data)

    @property
    def base_pos(self) -> np.ndarray:
        return self.data.qpos[0:3].copy()

    @property
    def base_quat_wxyz(self) -> np.ndarray:
        return self.data.qpos[3:7].copy()

    @property
    def upright(self) -> float:
        """z-component of the world-frame up expressed in the body frame."""
        q = self.base_quat_wxyz
        w, x, y, z = q
        return float(1.0 - 2.0 * (x * x + y * y))


def run_walk(
    walker: WalkingPiPlus,
    duration_sec: float,
    fps: int,
    record_video: bool,
    video_path: Path | None,
) -> dict:
    n_steps = int(round(duration_sec / SIM_DT))
    decim = CONTROL_DECIMATION
    frames: list[np.ndarray] = []
    renderer = None
    camera = None
    every_render = max(1, round(1.0 / (SIM_DT * fps)))

    if record_video:
        renderer = mujoco.Renderer(walker.model, 480, 320)
        camera = mujoco.MjvCamera()
        mujoco.mjv_defaultCamera(camera)
        camera.type = mujoco.mjtCamera.mjCAMERA_TRACKING
        body_id = mujoco.mj_name2id(walker.model, mujoco.mjtObj.mjOBJ_BODY, "base_link")
        camera.trackbodyid = body_id if body_id >= 0 else 0
        camera.distance = 2.6
        camera.elevation = -0.35

    traj_t: list[float] = []
    traj_x: list[float] = []
    traj_y: list[float] = []
    traj_z: list[float] = []
    traj_upright: list[float] = []
    traj_qpos: list[np.ndarray] = []

    start = time.monotonic()
    for i in range(n_steps):
        if i % decim == 0:
            walker.policy_step()
        walker.step()

        if renderer is not None and i % every_render == 0:
            renderer.update_scene(walker.data, camera=camera)
            frames.append(renderer.render())

        if i % every_render == 0:
            pos = walker.base_pos
            traj_t.append(i * SIM_DT)
            traj_x.append(float(pos[0]))
            traj_y.append(float(pos[1]))
            traj_z.append(float(pos[2]))
            traj_upright.append(walker.upright)
            traj_qpos.append(walker.data.qpos.copy())

    elapsed = time.monotonic() - start
    if renderer is not None:
        renderer.close()

    if video_path is not None and frames:
        import imageio.v2 as imageio

        video_path.parent.mkdir(parents=True, exist_ok=True)
        writer = imageio.get_writer(str(video_path), fps=fps, codec="libx264", quality=7)
        for f in frames:
            writer.append_data(f)
        writer.close()

    displacement = float(traj_x[-1] - traj_x[0]) if traj_x else 0.0
    lateral = float(traj_y[-1] - traj_y[0]) if traj_y else 0.0
    min_upright = min(traj_upright) if traj_upright else 0.0
    summary = {
        "duration_sec": duration_sec,
        "sim_steps": n_steps,
        "wall_time_sec": elapsed,
        "final_x": traj_x[-1] if traj_x else 0.0,
        "final_y": traj_y[-1] if traj_y else 0.0,
        "forward_displacement_m": displacement,
        "lateral_drift_m": lateral,
        "min_upright": min_upright,
        "avg_forward_speed_mps": displacement / duration_sec if duration_sec > 0 else 0.0,
        "stayed_upright": min_upright > 0.5,
        "frames": len(frames),
        "t": np.asarray(traj_t),
        "x": np.asarray(traj_x),
        "y": np.asarray(traj_y),
        "z": np.asarray(traj_z),
        "upright": np.asarray(traj_upright),
        "qpos": np.asarray(traj_qpos) if traj_qpos else np.empty((0, 0)),
    }
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--xml", type=Path, default=PI_PLUS_XML)
    parser.add_argument("--policy", type=Path, default=PI_PLUS_POLICY)
    parser.add_argument("--duration", type=float, default=6.0, help="simulation seconds")
    parser.add_argument("--vx", type=float, default=0.5, help="forward velocity command (m/s)")
    parser.add_argument("--vy", type=float, default=0.0)
    parser.add_argument("--vtheta", type=float, default=0.0)
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--video", type=Path, default=None, help="output mp4 path")
    parser.add_argument("--trajectory", type=Path, default=None, help="output npz path")
    parser.add_argument("--no-video", action="store_true")
    parser.add_argument("--device", default="cpu", choices=["cpu", "cuda"])
    args = parser.parse_args()

    if args.device == "cuda" and not torch.cuda.is_available():
        print("[walk] CUDA unavailable, falling back to CPU", file=sys.stderr)
        args.device = "cpu"

    walker = WalkingPiPlus(args.xml, args.policy, device=args.device)
    walker.reset()
    walker.set_command(args.vx, args.vy, args.vtheta)
    print(f"[walk] cmd vx={args.vx} vy={args.vy} vtheta={args.vtheta} for {args.duration}s")

    record_video = (not args.no_video) and (args.video is not None)
    summary = run_walk(walker, args.duration, args.fps, record_video, args.video)

    print("[walk] ---- summary ----")
    print(f"  forward displacement : {summary['forward_displacement_m']:+.3f} m")
    print(f"  lateral drift        : {summary['lateral_drift_m']:+.3f} m")
    print(f"  avg forward speed    : {summary['avg_forward_speed_mps']:+.3f} m/s")
    print(f"  min upright (z-axis) : {summary['min_upright']:.3f}  (>0.5 = stayed up)")
    print(f"  stayed upright       : {summary['stayed_upright']}")
    print(f"  wall time            : {summary['wall_time_sec']:.2f}s  ({summary['sim_steps']} steps)")

    if args.trajectory is not None:
        args.trajectory.parent.mkdir(parents=True, exist_ok=True)
        np.savez(
            args.trajectory,
            t=summary["t"], x=summary["x"], y=summary["y"], z=summary["z"],
            upright=summary["upright"], qpos=summary["qpos"],
            vx=args.vx, vy=args.vy, vtheta=args.vtheta,
        )
        print(f"[walk] trajectory saved to {args.trajectory}")

    if record_video:
        print(f"[walk] video saved to {args.video} ({summary['frames']} frames)")

    return 0 if summary["stayed_upright"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
