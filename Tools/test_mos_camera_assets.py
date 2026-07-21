#!/usr/bin/env python3
"""Validate project-owned Pi Plus MJCF camera test assets."""

from __future__ import annotations

import unittest
import json
import struct
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PI_MJCF = ROOT / "Assets/MosBrainCameraTest/pi_plus/pi_plus_stereo_camera.xml"
FIELD_GLB = ROOT / "Assets/Scenes/SoccerField/source/field.glb"


def read_glb_json(path: Path) -> dict:
    data = path.read_bytes()
    magic, _version, _length = struct.unpack_from("<III", data, 0)
    if magic != 0x46546C67:
        raise ValueError(f"{path} is not a GLB file")
    chunk_length, chunk_type = struct.unpack_from("<II", data, 12)
    if chunk_type != 0x4E4F534A:
        raise ValueError(f"{path} first GLB chunk is not JSON")
    return json.loads(data[20 : 20 + chunk_length].decode("utf-8"))


class PiStereoCameraAssetTest(unittest.TestCase):
    def test_mjcf_head_joints_are_actuated_and_cameras_are_head_mounted(self) -> None:
        root = ET.parse(PI_MJCF).getroot()

        head_yaw = root.find(".//body[@name='head_yaw_link']")
        self.assertIsNotNone(head_yaw)
        head_pitch = head_yaw.find("./body[@name='head_pitch_link']")
        self.assertIsNotNone(head_pitch)

        expected_cameras = {
            "left_eye": "0.1 0.03 0.05",
            "right_eye": "0.1 -0.03 0.05",
        }
        for camera_name, pos in expected_cameras.items():
            with self.subTest(camera_name=camera_name):
                camera = head_pitch.find(f"./camera[@name='{camera_name}']")
                self.assertIsNotNone(camera)
                self.assertEqual(camera.get("pos"), pos)
                self.assertEqual(camera.get("xyaxes"), "0 -1 0 0 0 1")
                self.assertIsNone(camera.get("euler"))
                self.assertEqual(camera.get("fovy"), "60")
                self.assertEqual(camera.get("resolution"), "640 480")

        self.assertIsNotNone(head_yaw.find("./joint[@name='head_yaw_joint']"))
        self.assertIsNotNone(head_pitch.find("./joint[@name='head_pitch_joint']"))
        self.assertIsNone(root.find(".//freejoint"))
        self.assertIsNone(root.find(".//joint[@name='head_to_right_eye']"))
        self.assertIsNone(root.find(".//body[@name='left_eye_camera_link']"))
        self.assertIsNone(root.find(".//body[@name='right_eye_camera_link']"))

        actuated_joints = {
            actuator.get("joint")
            for actuator in root.findall("./actuator/*")
        }
        self.assertIn("head_yaw_joint", actuated_joints)
        self.assertIn("head_pitch_joint", actuated_joints)
        self.assertNotIn("head_to_right_eye", actuated_joints)

        joints = {
            joint.get("name")
            for joint in root.findall(".//joint")
            if joint.get("name")
        }
        missing_actuator_joints = sorted(
            joint_name for joint_name in actuated_joints if joint_name not in joints
        )
        self.assertEqual(missing_actuator_joints, [])


class SoccerFieldAssetTest(unittest.TestCase):
    def test_field_glb_is_blender_y_up_and_bake_script_converts_to_ue(self) -> None:
        glb = read_glb_json(FIELD_GLB)
        nodes = {node["name"]: node for node in glb["nodes"]}

        self.assertEqual(nodes["Plane"].get("scale"), [5.300000190734863, 1, 3.9000000953674316])
        self.assertEqual(nodes["goal_10"].get("translation"), [4.5, 0.5, 0.9399999976158142])
        self.assertEqual(nodes["goal_11"].get("translation"), [4.5, 0.5, -0.940000057220459])
        self.assertEqual(nodes["goal_0"].get("translation"), [4.5, 0.9599999785423279, 0])
        self.assertEqual(nodes["goal_1"].get("translation"), [-4.5, 0.9599999785423279, 0])

        bake_script = (ROOT / "Tools/ue_bake_soccer_field_scene.py").read_text(encoding="utf-8")
        self.assertIn("return unreal.Vector(x * 100.0, z_width * 100.0, y_up * 100.0)", bake_script)
        self.assertIn("return unreal.Vector(x, z_width, y_up)", bake_script)


if __name__ == "__main__":
    unittest.main()
