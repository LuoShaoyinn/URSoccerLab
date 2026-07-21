#!/usr/bin/env python3
"""Validate project-owned MOS camera test assets."""

from __future__ import annotations

import unittest
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PI_URDF = ROOT / "Assets/MosBrainCameraTest/pi_plus/pi_plus.urdf"
PI_MJCF = ROOT / "Assets/MosBrainCameraTest/pi_plus/pi_plus_stereo_camera.xml"


class PiStereoCameraAssetTest(unittest.TestCase):
    def test_urdf_head_joints_are_unlocked_and_cameras_are_head_mounted(self) -> None:
        root = ET.parse(PI_URDF).getroot()

        joints = {joint.get("name"): joint for joint in root.findall("joint")}
        self.assertEqual(joints["head_yaw_joint"].get("type"), "revolute")
        self.assertEqual(joints["head_pitch_joint"].get("type"), "revolute")

        expected_cameras = {
            "left_eye_camera_joint": ("left_eye_camera_link", "0.16 0.03 0.05"),
            "right_eye_camera_joint": ("right_eye_camera_link", "0.16 -0.03 0.05"),
        }
        for joint_name, (link_name, xyz) in expected_cameras.items():
            with self.subTest(joint_name=joint_name):
                camera_joint = joints[joint_name]
                self.assertEqual(camera_joint.get("type"), "fixed")
                self.assertEqual(camera_joint.find("origin").get("xyz"), xyz)
                self.assertEqual(camera_joint.find("parent").get("link"), "head_pitch_link")
                self.assertEqual(camera_joint.find("child").get("link"), link_name)
                self.assertIsNotNone(root.find(f"./link[@name='{link_name}']"))

    def test_mjcf_head_joints_are_actuated_and_cameras_are_head_mounted(self) -> None:
        root = ET.parse(PI_MJCF).getroot()

        head_yaw = root.find(".//body[@name='head_yaw_link']")
        self.assertIsNotNone(head_yaw)
        head_pitch = head_yaw.find("./body[@name='head_pitch_link']")
        self.assertIsNotNone(head_pitch)

        expected_cameras = {
            "left_eye_camera_link": ("left_eye_camera", "0.16 0.03 0.05"),
            "right_eye_camera_link": ("right_eye_camera", "0.16 -0.03 0.05"),
        }
        for body_name, (camera_name, pos) in expected_cameras.items():
            with self.subTest(camera_name=camera_name):
                camera_body = head_pitch.find(f"./body[@name='{body_name}']")
                self.assertIsNotNone(camera_body)
                self.assertEqual(camera_body.get("pos"), pos)
                camera = camera_body.find(f"./camera[@name='{camera_name}']")
                self.assertIsNotNone(camera)
                self.assertEqual(camera.get("pos"), "0 0 0")
                self.assertIsNone(camera.get("xyaxes"))

        self.assertIsNotNone(head_yaw.find("./joint[@name='head_yaw_joint']"))
        self.assertIsNotNone(head_pitch.find("./joint[@name='head_pitch_joint']"))
        self.assertIsNone(root.find(".//joint[@name='floating_base_joint']"))
        self.assertIsNone(root.find(".//joint[@name='head_to_right_eye']"))

        actuated_joints = {
            motor.get("joint")
            for motor in root.findall("./actuator/motor")
        }
        self.assertIn("head_yaw_joint", actuated_joints)
        self.assertIn("head_pitch_joint", actuated_joints)
        self.assertNotIn("head_to_right_eye", actuated_joints)


if __name__ == "__main__":
    unittest.main()
