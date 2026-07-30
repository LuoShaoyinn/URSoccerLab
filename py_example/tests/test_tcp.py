import struct
import unittest

from ursoccerlab.media import camera_to_rgb
from ursoccerlab.tcp import CODEC_RAW, parse_camera


class CameraProtocolTest(unittest.TestCase):
    def test_parse_raw_camera_payload(self) -> None:
        pixels = bytes((1, 2, 3, 255, 4, 5, 6, 255))
        payload = (
            bytes((CODEC_RAW, 1))
            + struct.pack("<d", 1.25)
            + struct.pack("<HHI", 2, 1, len(pixels))
            + pixels
        )

        cameras = parse_camera(payload)

        self.assertEqual(len(cameras), 1)
        self.assertAlmostEqual(cameras[0]["sim_time"], 1.25)
        self.assertEqual(cameras[0]["width"], 2)
        self.assertEqual(cameras[0]["height"], 1)
        self.assertEqual(camera_to_rgb(cameras[0]).shape, (1, 2, 3))
