"""Factory gain presets for URSoccerLab robots.

Each preset provides the ``kp`` and ``kv`` dictionaries that reproduce
the original MuJoCo ``<position>`` actuator behaviour when passed to
``RobotClient.set_controller_params(**PRESET, actuator_mode="position")``.

Example::

    from ursoccerlab import RobotClient
    from ursoccerlab.gains import PI_PLUS

    client = RobotClient("127.0.0.1", 10000)
    client.set_controller_params(**PI_PLUS, actuator_mode="position")
"""
from __future__ import annotations

# --------------------------------------------------------------------------
# pi_plus — 22 actuators (original MJCF kp/kv)
# --------------------------------------------------------------------------
_PI_PLUS_KP = {
    "l_hip_pitch_joint_servo": 80, "r_hip_pitch_joint_servo": 80,
    "l_hip_roll_joint_servo": 60, "r_hip_roll_joint_servo": 60,
    "l_thigh_joint_servo": 30, "r_thigh_joint_servo": 30,
    "l_calf_joint_servo": 80, "r_calf_joint_servo": 80,
    "l_ankle_pitch_joint_servo": 60, "r_ankle_pitch_joint_servo": 60,
    "l_ankle_roll_joint_servo": 30, "r_ankle_roll_joint_servo": 30,
    "l_shoulder_pitch_joint_servo": 80, "r_shoulder_pitch_joint_servo": 80,
    "l_shoulder_roll_joint_servo": 60, "r_shoulder_roll_joint_servo": 60,
    "l_upper_arm_joint_servo": 30, "r_upper_arm_joint_servo": 30,
    "l_elbow_joint_servo": 80, "r_elbow_joint_servo": 80,
    "head_yaw_joint_servo": 50, "head_pitch_joint_servo": 50,
}
_PI_PLUS_KV = {
    "l_hip_pitch_joint_servo": 1.1, "r_hip_pitch_joint_servo": 1.1,
    "l_hip_roll_joint_servo": 1.2, "r_hip_roll_joint_servo": 1.2,
    "l_thigh_joint_servo": 0.6, "r_thigh_joint_servo": 0.6,
    "l_calf_joint_servo": 1.1, "r_calf_joint_servo": 1.1,
    "l_ankle_pitch_joint_servo": 1.2, "r_ankle_pitch_joint_servo": 1.2,
    "l_ankle_roll_joint_servo": 0.6, "r_ankle_roll_joint_servo": 0.6,
    "l_shoulder_pitch_joint_servo": 1.1, "r_shoulder_pitch_joint_servo": 1.1,
    "l_shoulder_roll_joint_servo": 1.2, "r_shoulder_roll_joint_servo": 1.2,
    "l_upper_arm_joint_servo": 0.6, "r_upper_arm_joint_servo": 0.6,
    "l_elbow_joint_servo": 1.1, "r_elbow_joint_servo": 1.1,
    "head_yaw_joint_servo": 0.0, "head_pitch_joint_servo": 0.0,
}

PI_PLUS: dict = {"kp": _PI_PLUS_KP, "kv": _PI_PLUS_KV}

# --------------------------------------------------------------------------
# mos9 — 20 actuators (original MJCF kp/kv)
# --------------------------------------------------------------------------
_STIFF_6408_KP = 98.3076
_STIFF_6408_KV = 3.9115
_STIFF_4310_KP = 59.5959
_STIFF_4310_KV = 2.3712

_MOS9_KP = {
    "r_shoulder_pitch_joint_servo": _STIFF_4310_KP, "l_shoulder_pitch_joint_servo": _STIFF_4310_KP,
    "r_shoulder_roll_joint_servo": _STIFF_4310_KP, "l_shoulder_roll_joint_servo": _STIFF_4310_KP,
    "r_elbow_joint_servo": _STIFF_6408_KP, "l_elbow_joint_servo": _STIFF_6408_KP,
    "r_hip_pitch_joint_servo": _STIFF_6408_KP, "l_hip_pitch_joint_servo": _STIFF_6408_KP,
    "r_hip_roll_joint_servo": _STIFF_6408_KP, "l_hip_roll_joint_servo": _STIFF_6408_KP,
    "r_hip_yaw_joint_servo": _STIFF_6408_KP, "l_hip_yaw_joint_servo": _STIFF_6408_KP,
    "r_knee_joint_servo": _STIFF_6408_KP, "l_knee_joint_servo": _STIFF_6408_KP,
    "r_ankle_pitch_joint_servo": _STIFF_6408_KP, "l_ankle_pitch_joint_servo": _STIFF_6408_KP,
    "r_ankle_roll_joint_servo": _STIFF_4310_KP, "l_ankle_roll_joint_servo": _STIFF_4310_KP,
    "head_yaw_joint_servo": 50, "head_pitch_joint_servo": 50,
}
_MOS9_KV = {
    "r_shoulder_pitch_joint_servo": _STIFF_4310_KV, "l_shoulder_pitch_joint_servo": _STIFF_4310_KV,
    "r_shoulder_roll_joint_servo": _STIFF_4310_KV, "l_shoulder_roll_joint_servo": _STIFF_4310_KV,
    "r_elbow_joint_servo": _STIFF_6408_KV, "l_elbow_joint_servo": _STIFF_6408_KV,
    "r_hip_pitch_joint_servo": _STIFF_6408_KV, "l_hip_pitch_joint_servo": _STIFF_6408_KV,
    "r_hip_roll_joint_servo": _STIFF_6408_KV, "l_hip_roll_joint_servo": _STIFF_6408_KV,
    "r_hip_yaw_joint_servo": _STIFF_6408_KV, "l_hip_yaw_joint_servo": _STIFF_6408_KV,
    "r_knee_joint_servo": _STIFF_6408_KV, "l_knee_joint_servo": _STIFF_6408_KV,
    "r_ankle_pitch_joint_servo": _STIFF_6408_KV, "l_ankle_pitch_joint_servo": _STIFF_6408_KV,
    "r_ankle_roll_joint_servo": _STIFF_4310_KV, "l_ankle_roll_joint_servo": _STIFF_4310_KV,
    "head_yaw_joint_servo": 0.0, "head_pitch_joint_servo": 0.0,
}

MOS9: dict = {"kp": _MOS9_KP, "kv": _MOS9_KV}


def detect_gains(actuator_names: list[str]) -> dict:
    """Return the matching gain preset for the robot behind *actuator_names*.

    Falls back to an empty dict (torque mode, no PD) if the type is unknown.
    """
    names = set(actuator_names)
    if "l_thigh_joint_servo" in names:
        return PI_PLUS
    if "r_knee_joint_servo" in names or "l_knee_joint_servo" in names:
        return MOS9
    return {}
