from __future__ import annotations

import math
import unittest

import main


class MotionCommandTest(unittest.TestCase):
    def test_selects_head_actuators_by_name(self) -> None:
        names = [
            "robot_Left_Hip_Pitch",
            "robot_AAHead_yaw",
            "robot_Head_pitch",
        ]

        self.assertEqual(main.select_motion_indices(names, "head", 0), [1, 2])

    def test_falls_back_to_first_actuators_when_pattern_is_absent(self) -> None:
        names = [
            "pi_l_hip_pitch_joint",
            "pi_l_hip_roll_joint",
            "pi_l_shoulder_pitch_joint",
        ]

        self.assertEqual(main.select_motion_indices(names, "head", 2), [0, 1])

    def test_builds_opposed_sine_command_for_selected_actuators(self) -> None:
        command = main.build_motion_command(
            motor_count=4,
            motion_indices=[1, 3],
            amplitude=0.5,
            frequency_hz=1.0,
            elapsed_sec=0.25,
        )

        self.assertEqual(command[0], 0.0)
        self.assertTrue(math.isclose(command[1], 0.5, abs_tol=1.0e-6))
        self.assertEqual(command[2], 0.0)
        self.assertTrue(math.isclose(command[3], -0.5, abs_tol=1.0e-6))

    def test_no_motion_indices_keep_zero_command(self) -> None:
        self.assertEqual(
            main.build_motion_command(
                motor_count=3,
                motion_indices=[],
                amplitude=0.5,
                frequency_hz=1.0,
                elapsed_sec=0.25,
            ),
            [0.0, 0.0, 0.0],
        )


if __name__ == "__main__":
    unittest.main()
