#!/usr/bin/env python3
"""Run the mos-brain Pi Plus policy and record its left-eye camera.

The simulator must be running with ``Config/URS_scene.json``.  This is an
external client: it reads TCP state, sends named motor position targets, and
does not use the admin pose API.

The policy was trained for the older mos-brain Pi dynamics. It exercises the
runtime protocol and capture path, but it is not a validated gait for the
current MJCF until the models are calibrated or the policy is retrained.

Run with the policy environment (which provides PyTorch):

    source py_example/.venv-walk/bin/activate
    uv run --no-project --active python py_example/demos/walk_policy.py \
        --vx 0.35 --duration 8 --video py_example/out/walk_rp0.mp4
"""
from __future__ import annotations

import argparse
import io
import sys
import time
from pathlib import Path

import numpy as np
import torch
from PIL import Image

EXAMPLE_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = EXAMPLE_ROOT.parent
sys.path.insert(0, str(EXAMPLE_ROOT))
from common.tcp import RobotClient  # noqa: E402

POLICY_PATH = REPO_ROOT / "refs/mos-brain/simulation/mujoco/assets/policies/pi_plus_model_40000.pt"

# The policy was trained with these 20 joints.  Head joints remain at their
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
# The walking scene configuration initializes Pi at DEFAULT_DOF, matching this
# pre-trained policy's position-control reference.
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
    import types

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


def world_to_body_rotation(quat_wxyz: np.ndarray) -> np.ndarray:
    w, x, y, z = quat_wxyz
    return np.asarray([
        [1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
        [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
        [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)],
    ], dtype=np.float32).T


def observation(state: dict, command: np.ndarray, last_action: np.ndarray) -> np.ndarray:
    base = state["base"]
    quat = np.asarray(base["quat"], dtype=np.float32)  # Runtime state is [w, x, y, z].
    velocity = np.asarray(base.get("vel", [0.0] * 6), dtype=np.float32)
    angular_velocity = world_to_body_rotation(quat) @ velocity[3:6]
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


def save_video(frames: list[np.ndarray], path: Path, fps: int) -> None:
    if not frames:
        raise RuntimeError("No camera frames received")
    path.parent.mkdir(parents=True, exist_ok=True)
    import imageio.v2 as imageio

    with imageio.get_writer(str(path), fps=fps, codec="libx264", quality=7,
                            macro_block_size=1) as writer:
        for frame in frames:
            writer.append_data(frame)
    print(f"[walk] wrote {path} ({len(frames)} frames at {fps} FPS)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--robot-port", type=int, default=10000)
    parser.add_argument("--policy", type=Path, default=POLICY_PATH)
    parser.add_argument("--duration", type=float, default=8.0)
    parser.add_argument("--vx", type=float, default=0.35)
    parser.add_argument("--policy-hz", type=float, default=50.0)
    parser.add_argument("--video-fps", type=int, default=15)
    parser.add_argument("--video", type=Path, default=Path("py_example/out/walk_rp0.mp4"))
    args = parser.parse_args()

    policy = load_policy(args.policy)
    client = RobotClient(args.host, args.robot_port)
    command = np.asarray([np.clip(args.vx, -1.5, 1.5), 0.0, 0.0], dtype=np.float32)
    history = np.zeros(OBS_STEP_DIM * OBS_HISTORY, dtype=np.float32)
    last_action = np.zeros(20, dtype=np.float32)
    targets = DEFAULT_DOF.copy()
    latest_state: dict | None = None
    frames: list[np.ndarray] = []
    last_frame_time = float("-inf")
    actuator_names: list[str] | None = None

    def send_targets() -> None:
        if actuator_names is None:
            return
        client.send_command({name: float(value) for name, value in zip(actuator_names, targets)})

    def pump() -> None:
        nonlocal actuator_names, latest_state, last_frame_time
        for kind, payload in client.recv():
            if kind == "state":
                latest_state = payload
                if actuator_names is None:
                    available = set(payload.get("actuators", {}))
                    names = [f"{joint}_servo" for joint in JOINTS_MUJOCO]
                    missing = [name for name in names if name not in available]
                    if missing:
                        raise RuntimeError(f"Robot is missing required position actuators: {missing}")
                    actuator_names = names
            elif kind == "camera":
                camera = payload[0]  # left eye
                if camera["sim_time"] - last_frame_time < 1.0 / args.video_fps:
                    continue
                if not camera["data"]:
                    continue
                if camera["codec"] == "jpeg":
                    image = Image.open(io.BytesIO(camera["data"])).convert("RGB")
                elif camera["codec"] == "raw":
                    image = Image.frombytes("RGBA", (camera["width"], camera["height"]), camera["data"])
                    image = image.convert("RGB")
                else:
                    raise RuntimeError(f"Unsupported camera codec: {camera['codec']}")
                frames.append(np.asarray(image))
                last_frame_time = camera["sim_time"]

    try:
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline and (latest_state is None or actuator_names is None):
            pump()
            send_targets()
            time.sleep(0.002)
        if latest_state is None or actuator_names is None:
            raise RuntimeError("No robot state received. Is the simulator running on TCP port 10000?")
        print(f"[walk] resolved {len(actuator_names)} Pi Plus position actuators")

        print("[walk] holding the standing pose for 1.5 s")
        warmup_until = time.monotonic() + 1.5
        while time.monotonic() < warmup_until:
            send_targets()
            pump()
            time.sleep(0.01)

        print(f"[walk] walking at vx={command[0]:.2f} m/s for {args.duration:.1f} s")
        start_x = latest_state["base"]["pos"][0]
        end = time.monotonic() + args.duration
        next_policy = time.monotonic()
        while time.monotonic() < end:
            pump()
            now = time.monotonic()
            if latest_state is not None and now >= next_policy:
                step = observation(latest_state, command, last_action)
                history = np.roll(history, -OBS_STEP_DIM)
                history[-OBS_STEP_DIM:] = step
                with torch.inference_mode():
                    action = policy(torch.from_numpy(np.clip(history, -100.0, 100.0)).unsqueeze(0))
                last_action = np.clip(action.numpy().squeeze().astype(np.float32), -100.0, 100.0)
                targets = last_action[ISAAC_TO_MUJOCO] * ACTION_SCALE + DEFAULT_DOF
                next_policy = now + 1.0 / args.policy_hz
            send_targets()
            time.sleep(0.001)

        pump()
        end_x = latest_state["base"]["pos"][0] if latest_state else start_x
        print(f"[walk] forward displacement: {end_x - start_x:+.3f} m")
        save_video(frames, args.video, args.video_fps)
        return 0
    finally:
        client.close()


if __name__ == "__main__":
    raise SystemExit(main())
