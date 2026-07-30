import struct
import unittest
import zlib
from io import BytesIO

import numpy as np
from PIL import Image

from ursoccerlab.media import camera_to_rgb, depth_to_meters
from ursoccerlab.tcp import (
    CODEC_JPEG,
    CODEC_RAW,
    CODEC_ZLIB,
    IMAGE_MESSAGE_VERSION,
    PIXEL_BGRA8,
    PIXEL_DEPTH_U16_MM,
    parse_camera,
    parse_image_message,
)


def image_message(
    *,
    sequence: int,
    sim_time: float,
    entries: list[tuple[str, int, int, int, int, bytes, int]],
) -> bytes:
    payload = bytearray(
        struct.pack("<BBHId", IMAGE_MESSAGE_VERSION, len(entries), 0, sequence, sim_time)
    )
    for name, codec, pixel_format, width, height, data, raw_len in entries:
        encoded_name = name.encode("utf-8")
        payload += struct.pack("<B", len(encoded_name)) + encoded_name
        payload += struct.pack(
            "<BBBHHII",
            codec,
            pixel_format,
            0,
            width,
            height,
            raw_len,
            len(data),
        )
        payload += data
    return bytes(payload)


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

    def test_parse_v2_stereo_rgb(self) -> None:
        pixels = bytes((1, 2, 3, 255, 4, 5, 6, 255))
        payload = image_message(
            sequence=42,
            sim_time=3.25,
            entries=[
                ("left_eye", CODEC_RAW, PIXEL_BGRA8, 2, 1, pixels, len(pixels)),
                ("right_eye", CODEC_RAW, PIXEL_BGRA8, 2, 1, pixels, len(pixels)),
            ],
        )

        images = parse_image_message(payload)

        self.assertEqual([image["camera_name"] for image in images], ["left_eye", "right_eye"])
        self.assertEqual(images[0]["sequence"], 42)
        self.assertAlmostEqual(images[0]["sim_time"], 3.25)
        self.assertEqual(camera_to_rgb(images[1]).shape, (1, 2, 3))

    def test_parse_zlib_uint16_depth(self) -> None:
        millimeters = np.array([[0, 1250], [3100, 65535]], dtype="<u2")
        raw = millimeters.tobytes()
        payload = image_message(
            sequence=7,
            sim_time=4.5,
            entries=[
                (
                    "left_eye",
                    CODEC_ZLIB,
                    PIXEL_DEPTH_U16_MM,
                    2,
                    2,
                    zlib.compress(raw),
                    len(raw),
                )
            ],
        )

        depth = parse_image_message(payload)[0]
        decoded = depth_to_meters(depth)

        self.assertEqual(depth["pixel_format"], "depth_u16_mm")
        np.testing.assert_allclose(
            decoded,
            np.array([[0.0, 1.25], [3.1, 65.535]], dtype=np.float32),
            atol=1e-5,
        )

    def test_v2_rejects_truncated_payload(self) -> None:
        payload = image_message(
            sequence=1,
            sim_time=0.0,
            entries=[("left_eye", CODEC_RAW, PIXEL_BGRA8, 1, 1, b"1234", 4)],
        )
        with self.assertRaisesRegex(ValueError, "truncated payload"):
            parse_image_message(payload[:-1])
