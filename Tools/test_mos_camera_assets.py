#!/usr/bin/env python3
"""Validate project-owned MOS camera test assets."""

from __future__ import annotations

import unittest
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PI_URDF = ROOT / "Assets/MosBrainCameraTest/pi_plus/pi_plus_head_camera.urdf"
PI_MJCF = ROOT / "Assets/MosBrainCameraTest/pi_plus/pi_plus_urlab_origin_camera.xml"


class PiHeadCameraAssetTest(unittest.TestCase):
    def test_urdf_head_joints_are_unlocked_and_camera_is_head_mounted(self) -> None:
        root = ET.parse(PI_URDF).getroot()

        joints = {joint.get("name"): joint for joint in root.findall("joint")}
        self.assertEqual(joints["head_yaw_joint"].get("type"), "revolute")
        self.assertEqual(joints["head_pitch_joint"].get("type"), "revolute")

        camera_joint = joints["urlab_origin_camera_joint"]
        self.assertEqual(camera_joint.get("type"), "fixed")
        self.assertEqual(camera_joint.find("parent").get("link"), "head_pitch_link")
        self.assertEqual(camera_joint.find("child").get("link"), "urlab_origin_camera_link")
        self.assertIsNotNone(root.find("./link[@name='urlab_origin_camera_link']"))

    def test_mjcf_head_joints_are_actuated_and_camera_is_head_mounted(self) -> None:
        root = ET.parse(PI_MJCF).getroot()

        head_yaw = root.find(".//body[@name='head_yaw_link']")
        self.assertIsNotNone(head_yaw)
        head_pitch = head_yaw.find("./body[@name='head_pitch_link']")
        self.assertIsNotNone(head_pitch)
        self.assertIsNotNone(head_pitch.find("./camera[@name='urlab_origin_camera']"))
        self.assertIsNotNone(head_yaw.find("./joint[@name='head_yaw_joint']"))
        self.assertIsNotNone(head_pitch.find("./joint[@name='head_pitch_joint']"))

        actuated_joints = {
            motor.get("joint")
            for motor in root.findall("./actuator/motor")
        }
        self.assertIn("head_yaw_joint", actuated_joints)
        self.assertIn("head_pitch_joint", actuated_joints)


if __name__ == "__main__":
    unittest.main()
