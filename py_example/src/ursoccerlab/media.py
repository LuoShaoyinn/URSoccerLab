"""Camera decoding and video output shared by the examples."""

from __future__ import annotations

import io
import zlib
from pathlib import Path

import imageio.v2 as imageio
import numpy as np
from PIL import Image


def camera_to_rgb(camera: dict) -> np.ndarray:
    """Decode one camera dictionary returned by ``RobotClient``."""
    data = camera["data"]
    if not data:
        raise ValueError("camera frame has no pixel data")
    if camera["codec"] == "jpeg":
        return np.asarray(Image.open(io.BytesIO(data)).convert("RGB"))
    if camera["codec"] == "raw":
        image = Image.frombytes(
            "RGBA",
            (int(camera["width"]), int(camera["height"])),
            data,
        )
        return np.asarray(image.convert("RGB"))
    raise ValueError(f"unsupported camera codec: {camera['codec']}")


def depth_to_meters(depth: dict) -> np.ndarray:
    """Decode one depth dictionary into a float32 ``(height, width)`` array."""
    data = depth["data"]
    expected_len = int(depth["uncompressed_len"])
    if depth["codec"] == "zlib":
        data = zlib.decompress(data)
    elif depth["codec"] != "raw":
        raise ValueError(f"unsupported depth codec: {depth['codec']}")
    if len(data) != expected_len:
        raise ValueError(
            f"depth payload decoded to {len(data)} bytes; expected {expected_len}"
        )

    shape = (int(depth["height"]), int(depth["width"]))
    if depth["pixel_format"] == "depth_f32_m":
        result = np.frombuffer(data, dtype="<f4").reshape(shape)
        return result.copy()
    if depth["pixel_format"] == "depth_u16_mm":
        result = np.frombuffer(data, dtype="<u2").reshape(shape)
        return result.astype(np.float32) * 0.001
    raise ValueError(f"unsupported depth pixel format: {depth['pixel_format']}")


def write_video(frames: list[np.ndarray], path: Path, fps: int) -> None:
    """Write RGB frames to an H.264 MP4. Skips (with a warning) if empty."""
    if not frames:
        print(f"WARNING: no camera frames received for {path} — skipping")
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    with imageio.get_writer(
        str(path),
        fps=fps,
        codec="libx264",
        quality=7,
        macro_block_size=1,
    ) as writer:
        for frame in frames:
            writer.append_data(frame)
    print(f"saved {path} ({len(frames)} frames at {fps} FPS)")
