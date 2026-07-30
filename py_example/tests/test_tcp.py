import struct
import unittest
from io import BytesIO

import numpy as np
from PIL import Image

from ursoccerlab.media import camera_to_rgb
from ursoccerlab.tcp import CODEC_JPEG, CODEC_RAW, parse_camera


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

    def test_parse_and_decode_jpeg_camera_payload(self) -> None:
        source = np.array([[[220, 30, 10], [20, 200, 40]]], dtype=np.uint8)
        encoded = BytesIO()
        Image.fromarray(source).save(encoded, format="JPEG", quality=90)
        image_data = encoded.getvalue()
        payload = (
            bytes((CODEC_JPEG, 1))
            + struct.pack("<d", 2.5)
            + struct.pack("<HHI", 2, 1, len(image_data))
            + image_data
        )

        cameras = parse_camera(payload)

        self.assertEqual(cameras[0]["codec"], "jpeg")
        self.assertEqual(camera_to_rgb(cameras[0]).shape, (1, 2, 3))
