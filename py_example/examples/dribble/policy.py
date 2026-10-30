"""Pi Plus walking-policy helpers, local to the dribble example.

Copied from the standalone ``pi_walk`` example so this folder is fully
self-contained. The policy was trained for the older mos-brain Pi dynamics.
"""
from __future__ import annotations

import sys
import types
from pathlib import Path

import numpy as np
import torch

POLICY_PATH = Path(__file__).resolve().parents[2] / "models" / "policies" / "pi_plus_model_40000.pt"

# The policy was trained with these 20 joints. Head joints remain at their
# robot-MJCF defaults and are intentionally not commanded by this example.
JOINTS_MUJOCO = [
    "l_hip_pitch_joint", "l_hip_roll_joint", "l_thigh_joint", "l_calf_joint",
    "l_ankle_pitch_joint", "l_ankle_roll_joint", "l_shoulder_pitch_joint",
    "l_shoulder_roll_joint", "l_upper_arm_joint", "l_elbow_joint",
    "r_hip_pitch_joint", "r_hip_roll_joint", "r_thigh_joint", "r_calf_joint",
    "r_ankle_pitch_joint", "r_ankle_roll_joint", "r_shoulder_pitch_joint",
    "r_shoulder_roll_joint", "r_upper_arm_joint", "r_elbow_joint",
]
ISAAC_TO_MUJOCO = np.asarray(
    [0, 4, 8, 12, 16, 18, 1, 5, 9, 13, 2, 6, 10, 14, 17, 19, 3, 7, 11, 15],
    dtype=np.int32,
)
MUJOCO_TO_ISAAC = np.asarray(
    [0, 6, 10, 16, 1, 7, 11, 17, 2, 8, 12, 18, 3, 9, 13, 19, 4, 14, 5, 15],
    dtype=np.int32,
)
DEFAULT_DOF = np.asarray(
    [-0.25, 0.0, 0.0, 0.65, -0.4, 0.0, 0.0, 0.2, 0.0, -1.2,
     -0.25, 0.0, 0.0, 0.65, -0.4, 0.0, 0.0, -0.2, 0.0, -1.2],
    dtype=np.float32,
)
OBS_STEP_DIM = 69
OBS_HISTORY = 5
ACTION_SCALE = 0.25


class Actor(torch.nn.Module):
    def __init__(self, dimensions: list[int]):
        super().__init__()
        layers: list[torch.nn.Module] = []
        for index in range(len(dimensions) - 1):
            layers.append(torch.nn.Linear(dimensions[index], dimensions[index + 1]))
            if index + 2 < len(dimensions):
                layers.append(torch.nn.ELU())
        self.actor = torch.nn.Sequential(*layers)

    def forward(self, observation: torch.Tensor) -> torch.Tensor:
        return self.actor(observation)


def load_policy(path: Path) -> torch.nn.Module:
    if not path.is_file():
        raise FileNotFoundError(f"Policy not found: {path}")

    # The mos-brain checkpoint carries an rsl_rl Normalizer object, but this
    # actor-only client only needs its tensor state dictionary.
    rsl_rl = sys.modules.setdefault("rsl_rl", types.ModuleType("rsl_rl"))
    utils_pkg = sys.modules.setdefault("rsl_rl.utils", types.ModuleType("rsl_rl.utils"))
    utils_mod = sys.modules.setdefault("rsl_rl.utils.utils", types.ModuleType("rsl_rl.utils.utils"))
    utils_mod.Normalizer = type("Normalizer", (), {})
    rsl_rl.utils = utils_pkg
    utils_pkg.utils = utils_mod

    checkpoint = torch.load(path, map_location="cpu", weights_only=False)
    state = checkpoint.get("model_state_dict", checkpoint)
    actor_state = {key: value for key, value in state.items() if key.startswith("actor.")}
    weights = sorted(
        (key for key in actor_state if key.endswith(".weight")),
        key=lambda key: int(key.split(".")[1]),
    )
    dimensions = [int(actor_state[weights[0]].shape[1])]
    dimensions.extend(int(actor_state[key].shape[0]) for key in weights)
    actor = Actor(dimensions)
    actor.load_state_dict(actor_state, strict=True)
    actor.eval()
    if dimensions != [OBS_STEP_DIM * OBS_HISTORY, 512, 256, 128, 20]:
        raise RuntimeError(f"Unexpected Pi Plus policy dimensions: {dimensions}")
    return actor


def inverse_rotate(quat_xyzw: np.ndarray, vector: np.ndarray) -> np.ndarray:
    scalar = quat_xyzw[3]
    qvec = quat_xyzw[:3]
    return (vector * (2.0 * scalar * scalar - 1.0)
            - np.cross(qvec, vector) * scalar * 2.0
            + qvec * np.dot(qvec, vector) * 2.0)


def observation(state: dict, command: np.ndarray, last_action: np.ndarray) -> np.ndarray:
    base = state["base"]
    quat = np.asarray(base["quat"], dtype=np.float32)  # Runtime state is [w, x, y, z].
    velocity = np.asarray(base.get("vel", [0.0] * 6), dtype=np.float32)
    # MuJoCo free-joint angular qvel is already expressed in the body frame.
    angular_velocity = velocity[3:6]
    gravity = inverse_rotate(np.asarray([quat[1], quat[2], quat[3], quat[0]], dtype=np.float32),
                             np.asarray([0.0, 0.0, -1.0], dtype=np.float32))
    joints = state["joints"]
    qpos = np.asarray([joints[name]["qpos"] for name in JOINTS_MUJOCO], dtype=np.float32)
    qvel = np.asarray([joints[name]["qvel"] for name in JOINTS_MUJOCO], dtype=np.float32)
    result = np.zeros(OBS_STEP_DIM, dtype=np.float32)
    result[0:3] = angular_velocity
    result[3:6] = gravity
    result[6:9] = command
    result[9:29] = (qpos - DEFAULT_DOF)[MUJOCO_TO_ISAAC]
    result[29:49] = qvel[MUJOCO_TO_ISAAC]
    result[49:69] = last_action
    return np.nan_to_num(result, nan=0.0, posinf=0.0, neginf=0.0)
