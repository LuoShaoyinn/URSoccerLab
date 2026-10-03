#!/usr/bin/env python3
"""Record the source simulator inspector stream on TCP port 12000."""
import argparse
import math
import time
from pathlib import Path

import imageio.v2 as imageio
from PIL import Image

from ursoccerlab import InspectorClient
from ursoccerlab.media import camera_to_rgb


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=12000)
    parser.add_argument("--position", type=float, nargs=3, default=[-4, 0, 2])
    parser.add_argument("--quaternion", type=float, nargs=4, default=[0, 0, 0, 1],
                        help="xyzw, MuJoCo world, camera +X forward and +Z up")
    parser.add_argument("--duration", type=float, default=10)
    parser.add_argument("--fps", type=float, default=15, help="output playback FPS")
    parser.add_argument("--video", type=Path, default=Path("out/inspector.mp4"))
    args = parser.parse_args()
    if not all(math.isfinite(v) and v > 0 for v in (args.duration, args.fps)):
        parser.error("duration and FPS must be positive and finite")
    args.video.parent.mkdir(parents=True, exist_ok=True)
    count = 0
    writer = None
    try:
        with InspectorClient(args.host, args.port) as client:
            client.set_camera(args.position, args.quaternion)
            deadline = time.monotonic() + args.duration
            while time.monotonic() < deadline:
                for kind, data in client.recv():
                    if kind == "status":
                        print(data)
                        if data.get("ok") is False:
                            raise RuntimeError(f"inspector rejected camera: {data}")
                    else:
                        for camera in data:
                            rgb = camera_to_rgb(camera)
                            if writer is None:
                                writer = imageio.get_writer(str(args.video), fps=args.fps,
                                                            codec="libx264", macro_block_size=1)
                            writer.append_data(rgb)
                            count += 1
                            if count == 1:
                                Image.fromarray(rgb).save(args.video.with_suffix(".png"))
                if not client.alive:
                    raise ConnectionError("inspector disconnected")
                time.sleep(0.002)
            if count == 0:
                raise TimeoutError("no inspector images received")
    finally:
        if writer is not None:
            writer.close()
    print(f"Saved {count} frames to {args.video} (playback {args.fps:g} FPS)")


if __name__ == "__main__":
    main()
