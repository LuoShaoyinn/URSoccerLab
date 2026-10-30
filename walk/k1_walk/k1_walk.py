#!/usr/bin/env python3
"""Run the MOS-Brain K1 walking checkpoint against URSoccerLab."""
import argparse
import re
import sys
import time
from pathlib import Path
import numpy as np
import torch
from ursoccerlab.tcp import AdminClient, RobotClient

K1_JOINTS_POLICY_ORDER = [
    "AAHead_yaw",
    "ALeft_Shoulder_Pitch",
    "ARight_Shoulder_Pitch",
    "Left_Hip_Pitch",
    "Right_Hip_Pitch",
    "Head_pitch",
    "Left_Shoulder_Roll",
    "Right_Shoulder_Roll",
    "Left_Hip_Roll",
    "Right_Hip_Roll",
    "Left_Elbow_Pitch",
    "Right_Elbow_Pitch",
    "Left_Hip_Yaw",
    "Right_Hip_Yaw",
    "Left_Elbow_Yaw",
    "Right_Elbow_Yaw",
    "Left_Knee_Pitch",
    "Right_Knee_Pitch",
    "Left_Ankle_Pitch",
    "Right_Ankle_Pitch",
    "Left_Ankle_Roll",
    "Right_Ankle_Roll",
]

K1_ACTION_SCALE = {
    ".*Head.*": 0.375,
    ".*Shoulder_Pitch": 0.875,
    ".*Shoulder_Roll": 0.875,
    ".*Elbow_Pitch": 0.875,
    ".*Elbow_Yaw": 0.875,
    ".*Hip_Pitch": 0.09375,
    ".*Hip_Roll": 0.109375,
    ".*Hip_Yaw": 0.0625,
    ".*Knee_Pitch": 0.125,
    ".*Ankle_Pitch": 1.0 / 6.0,
    ".*Ankle_Roll": 1.0 / 6.0,
}

K1_MOTOR_STIFFNESS = {
    ".*Head.*": 4.0,
    ".*Shoulder.*": 4.0,
    ".*Elbow.*": 4.0,
    ".*Hip_Pitch": 80.0,
    ".*Hip_Roll": 80.0,
    ".*Hip_Yaw": 80.0,
    ".*Knee_Pitch": 80.0,
    ".*Ankle_.*": 30.0,
}

K1_MOTOR_DAMPING = {
    ".*Head.*": 1.0,
    ".*Shoulder.*": 1.0,
    ".*Elbow.*": 1.0,
    ".*Hip_Pitch": 2.0,
    ".*Hip_Roll": 2.0,
    ".*Hip_Yaw": 2.0,
    ".*Knee_Pitch": 2.0,
    ".*Ankle_.*": 2.0,
}

K1_RESET_JOINT_POS = {
    "Left_Shoulder_Roll": -1.3,
    "Right_Shoulder_Roll": 1.3,
}

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
    if dimensions[0] != 78 or dimensions[-1] != 22:
        raise RuntimeError(f"Unexpected K1 policy dimensions: {dimensions}")
    return actor


def inverse_rotate(q, v):
    w = q[0]
    xyz = q[1:]
    return v * (2 * w * w - 1) - 2 * w * np.cross(xyz, v) + 2 * xyz * np.dot(xyz, v)


def values(mapping):
    return np.asarray([next(value for pattern, value in mapping.items() if re.fullmatch(pattern, name)) for name in K1_JOINTS_POLICY_ORDER], dtype=np.float32)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=10000)
    parser.add_argument("--admin-port", type=int, default=11000)
    parser.add_argument("--actor-id", default="robot_rp0")
    parser.add_argument("--policy", type=Path, default=Path(__file__).resolve().parents[2] / "models/policies/k1_model_46000.pt")
    parser.add_argument("--duration", type=float, default=15.0)
    parser.add_argument("--vx", type=float, default=0.3)
    parser.add_argument("--vy", type=float, default=0.0)
    parser.add_argument("--yaw-rate", type=float, default=0.0)
    args = parser.parse_args()
    policy = load_policy(args.policy)
    names = K1_JOINTS_POLICY_ORDER
    actuators = [name + "_servo" for name in names]
    default = np.asarray([K1_RESET_JOINT_POS.get(name, 0.0) for name in names], dtype=np.float32)
    scale = values(K1_ACTION_SCALE)
    client = RobotClient(args.host, args.port)
    admin = None
    locked = False
    client.set_controller_params(kp=dict(zip(actuators, map(float, values(K1_MOTOR_STIFFNESS)))), kv=dict(zip(actuators, map(float, values(K1_MOTOR_DAMPING)))), actuator_mode="position")
    action = np.zeros(22, dtype=np.float32)
    command = np.asarray([args.vx, args.vy, args.yaw_rate], dtype=np.float32)
    latest = None
    deadline = time.monotonic() + 5
    try:
        admin = AdminClient(args.host, args.admin_port)
        reply = admin.reset(args.actor_id)
        if not reply.get("ok"):
            raise RuntimeError(f"K1 reset failed: {reply}")
        reply = admin.lock_pose(args.actor_id)
        if not reply.get("ok"):
            raise RuntimeError(f"K1 pose lock failed: {reply}")
        locked = True
        print(f"Reset and locked {args.actor_id}; preparing controller", flush=True)
        while latest is None and time.monotonic() < deadline:
            for kind, payload in client.recv():
                if kind == "state": latest = payload
            time.sleep(0.002)
        if latest is None: raise RuntimeError("No robot state received")
        missing = set(actuators) - set(latest.get("actuators", {}))
        if missing: raise RuntimeError(f"Missing K1 actuators: {sorted(missing)}")
        # Prime the position controller before releasing the upright pose.
        client.send_command(dict(zip(actuators, map(float, default))))
        reply = admin.unlock_pose(args.actor_id)
        if not reply.get("ok"):
            raise RuntimeError(f"K1 pose unlock failed: {reply}")
        locked = False
        print(f"Walking at vx={args.vx:.2f} m/s for {args.duration:.1f} s", flush=True)
        end = time.monotonic() + args.duration
        next_step = time.monotonic()
        while time.monotonic() < end:
            for kind, payload in client.recv():
                if kind == "state": latest = payload
            now = time.monotonic()
            if now < next_step:
                time.sleep(min(0.002, next_step - now)); continue
            base = latest["base"]
            q = np.asarray(base["quat"], dtype=np.float32)
            vel = np.asarray(base["vel"], dtype=np.float32)
            joints = latest["joints"]
            pos = np.asarray([joints[n]["qpos"] for n in names], dtype=np.float32)
            jvel = np.asarray([joints[n]["qvel"] for n in names], dtype=np.float32)
            obs = np.concatenate((inverse_rotate(q, vel[:3]), inverse_rotate(q, vel[3:]) * 0.2, inverse_rotate(q, np.array([0, 0, -1], dtype=np.float32)), command, pos-default, jvel*0.05, action))
            obs = np.clip(np.nan_to_num(obs), -100, 100).astype(np.float32)
            with torch.inference_mode(): action = np.clip(policy(torch.from_numpy(obs).unsqueeze(0)).numpy().reshape(-1), -100, 100)
            client.send_command(dict(zip(actuators, map(float, default + scale * action))))
            next_step = now + 0.02
    finally:
        if admin is not None:
            try:
                if locked:
                    admin.unlock_pose(args.actor_id)
            finally:
                admin.close()
        client.close()

if __name__ == "__main__": main()
