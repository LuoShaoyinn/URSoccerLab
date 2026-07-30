"""Camera decoding and video output shared by the examples."""

from __future__ import annotations

import io
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


def write_video(frames: list[np.ndarray], path: Path, fps: int) -> None:
    """Write RGB frames to an H.264 MP4."""
    if not frames:
        raise RuntimeError(f"no camera frames received for {path}")
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
