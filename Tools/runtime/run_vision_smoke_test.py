#!/usr/bin/env python3
"""Run the URSoccerLab end-to-end vision smoke test.

The test validates the tracked baked assets, starts the field map offscreen,
runs the Python zero-command client, and requires that a camera PNG is produced.
"""

from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import sys
import threading
import time
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_UE = Path(
    os.environ.get(
        "URS_UE",
        str(Path.home() / "Unreal_Engine_5.7.4/Engine/Binaries/Linux/UnrealEditor"),
    )
)
PROJECT = ROOT / "URSoccerLab.uproject"
MAP_PATH = "/Game/Levels/URS_SoccerField"


def paeth_predictor(left: int, above: int, upper_left: int) -> int:
    estimate = left + above - upper_left
    left_dist = abs(estimate - left)
    above_dist = abs(estimate - above)
    upper_left_dist = abs(estimate - upper_left)
    if left_dist <= above_dist and left_dist <= upper_left_dist:
        return left
    if above_dist <= upper_left_dist:
        return above
    return upper_left


def png_rgb_stats(path: Path) -> dict[str, float | int]:
    data = path.read_bytes()
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise RuntimeError(f"{path} is not a PNG")

    offset = 8
    width = height = bit_depth = color_type = None
    compressed = bytearray()
    while offset < len(data):
        if offset + 8 > len(data):
            raise RuntimeError(f"{path} has a truncated PNG chunk header")
        length = int.from_bytes(data[offset : offset + 4], "big")
        chunk_type = data[offset + 4 : offset + 8]
        chunk_data = data[offset + 8 : offset + 8 + length]
        offset += 12 + length
        if chunk_type == b"IHDR":
            width = int.from_bytes(chunk_data[0:4], "big")
            height = int.from_bytes(chunk_data[4:8], "big")
            bit_depth = chunk_data[8]
            color_type = chunk_data[9]
            interlace = chunk_data[12]
            if bit_depth != 8 or color_type not in (2, 6) or interlace != 0:
                raise RuntimeError(
                    f"unsupported PNG format: bit_depth={bit_depth}, "
                    f"color_type={color_type}, interlace={interlace}"
                )
        elif chunk_type == b"IDAT":
            compressed.extend(chunk_data)
        elif chunk_type == b"IEND":
            break

    if width is None or height is None or color_type is None:
        raise RuntimeError(f"{path} is missing PNG metadata")

    channels = 4 if color_type == 6 else 3
    row_bytes = width * channels
    raw = zlib.decompress(bytes(compressed))
    expected = (row_bytes + 1) * height
    if len(raw) != expected:
        raise RuntimeError(f"{path} decoded to {len(raw)} bytes, expected {expected}")

    prev = bytearray(row_bytes)
    pixels = 0
    total = 0
    max_channel = 0
    non_black = 0
    unique_sample: set[tuple[int, int, int]] = set()
    sample_stride = max((width * height) // 4096, 1)

    for y in range(height):
        start = y * (row_bytes + 1)
        filter_type = raw[start]
        encoded = raw[start + 1 : start + 1 + row_bytes]
        row = bytearray(row_bytes)
        for i, value in enumerate(encoded):
            left = row[i - channels] if i >= channels else 0
            above = prev[i]
            upper_left = prev[i - channels] if i >= channels else 0
            if filter_type == 0:
                decoded = value
            elif filter_type == 1:
                decoded = value + left
            elif filter_type == 2:
                decoded = value + above
            elif filter_type == 3:
                decoded = value + ((left + above) // 2)
            elif filter_type == 4:
                decoded = value + paeth_predictor(left, above, upper_left)
            else:
                raise RuntimeError(f"{path} has unsupported PNG filter {filter_type}")
            row[i] = decoded & 0xFF

        for x in range(width):
            idx = x * channels
            r, g, b = row[idx], row[idx + 1], row[idx + 2]
            total += r + g + b
            max_channel = max(max_channel, r, g, b)
            if r > 4 or g > 4 or b > 4:
                non_black += 1
            if pixels % sample_stride == 0:
                unique_sample.add((r, g, b))
            pixels += 1
        prev = row

    return {
        "width": width,
        "height": height,
        "mean_rgb": total / (pixels * 3),
        "max_channel": max_channel,
        "non_black_ratio": non_black / pixels,
        "unique_sample": len(unique_sample),
    }


def run_checked(cmd: list[str], cwd: Path, log_path: Path | None = None) -> None:
    print("+", " ".join(cmd), flush=True)
    if log_path:
        log_path.parent.mkdir(parents=True, exist_ok=True)
        with log_path.open("w", encoding="utf-8") as log:
            proc = subprocess.run(cmd, cwd=cwd, text=True, stdout=log, stderr=subprocess.STDOUT)
    else:
        proc = subprocess.run(cmd, cwd=cwd)
    if proc.returncode != 0:
        suffix = f" See {log_path}" if log_path else ""
        raise RuntimeError(f"command failed with exit code {proc.returncode}.{suffix}")


def start_simulator(
    ue: Path, extra_args: list[str], *, force_memory_ddc: bool = False
) -> subprocess.Popen[str]:
    cmd = [
        str(ue),
        str(PROJECT),
        MAP_PATH,
        "-game",
        "-RenderOffscreen",
        "-unattended",
        "-nop4",
        "-nosplash",
        "-NoSound",
        *(["-DDC-ForceMemoryCache"] if force_memory_ddc else []),
        *extra_args,
    ]
    print("+", " ".join(cmd), flush=True)
    return subprocess.Popen(
        cmd,
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )


def terminate_process(proc: subprocess.Popen[str], timeout_sec: float = 10.0) -> None:
    if proc.poll() is not None:
        return
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=timeout_sec)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=timeout_sec)


def drain_process_log(
    proc: subprocess.Popen[str],
    log_path: Path,
    ready_markers: tuple[str, ...],
) -> tuple[threading.Event, threading.Thread]:
    ready = threading.Event()

    def run() -> None:
        with log_path.open("w", encoding="utf-8") as log:
            if not proc.stdout:
                return
            for line in proc.stdout:
                log.write(line)
                log.flush()
                if any(marker in line for marker in ready_markers):
                    ready.set()

    thread = threading.Thread(target=run, name="urs-ue-log-drain", daemon=True)
    thread.start()
    return ready, thread


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ue", type=Path, default=DEFAULT_UE)
    parser.add_argument("--robot", default="robot_rp0")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--timeout-ms", type=int, default=30000)
    parser.add_argument("--out", type=Path, default=ROOT / "py_example" / "out" / "vision_smoke")
    parser.add_argument("--scene-config", type=Path, default=ROOT / "Config" / "URS_scene.json")
    parser.add_argument("--sim-extra-arg", action="append", default=[])
    parser.add_argument("--render-warmup-sec", type=float, default=2.0)
    parser.add_argument("--camera-frame-count", type=int, default=20)
    parser.add_argument(
        "--camera-compress",
        choices=("jpeg", "raw"),
        default="jpeg",
        help="Camera wire encoding selected by the Unreal transport.",
    )
    parser.add_argument(
        "--jpeg-quality",
        type=int,
        default=85,
        help="JPEG quality from 1 to 100; ignored for raw transport.",
    )
    parser.add_argument(
        "--force-memory-ddc",
        action="store_true",
        help="Use a non-persistent in-memory Unreal DDC (slower on repeated runs).",
    )
    args = parser.parse_args()

    if not args.ue.exists():
        raise FileNotFoundError(args.ue)
    args.out = args.out.resolve()

    if not args.scene_config.exists():
        raise FileNotFoundError(args.scene_config)
    scene_config = json.loads(args.scene_config.read_text(encoding="utf-8"))
    expect_depth = scene_config.get("vision", {}).get("mode") == "rgbd"
    if not 1 <= args.jpeg_quality <= 100:
        parser.error("--jpeg-quality must be between 1 and 100")
    run_checked([sys.executable, str(ROOT / "Tools" / "editor" / "validate_baked_assets.py")], ROOT)
    run_checked(["uv", "sync"], ROOT / "py_example")

    args.out.mkdir(parents=True, exist_ok=True)
    sim_extra_args = list(args.sim_extra_arg)
    sim_extra_args.append(f"-URSSceneConfig={args.scene_config.resolve()}")
    sim_extra_args.append(f"-URSCameraCompress={args.camera_compress}")
    if args.camera_compress == "jpeg":
        sim_extra_args.append(f"-URSJpegQuality={args.jpeg_quality}")
    sim = start_simulator(
        args.ue, sim_extra_args, force_memory_ddc=args.force_memory_ddc
    )
    sim_log_path = ROOT / "Saved" / "Logs" / "URS_VisionSmokeRuntime.log"
    sim_log_path.parent.mkdir(parents=True, exist_ok=True)
    sim_ready, log_thread = drain_process_log(
        sim,
        sim_log_path,
        (
            " listening on port 10000",
        ),
    )

    try:
        deadline = time.monotonic() + max(args.timeout_ms / 1000.0, 5.0)
        while time.monotonic() < deadline and not sim_ready.is_set():
            if sim.poll() is not None:
                raise RuntimeError(f"simulator exited early with code {sim.returncode}. See {sim_log_path}")
            time.sleep(0.1)

        if sim.poll() is not None:
            raise RuntimeError(f"simulator exited early with code {sim.returncode}. See {sim_log_path}")
        if not sim_ready.is_set():
            raise RuntimeError(f"simulator did not start the URSoccerLab TCP transport. See {sim_log_path}")

        if args.render_warmup_sec > 0:
            time.sleep(args.render_warmup_sec)

        client_cmd = [
            "uv",
            "run",
            "python",
            "examples/vision_smoke.py",
            "--host",
            args.host,
            "--robot",
            args.robot,
            "--port",
            str(10000),
            "--timeout-ms",
            str(args.timeout_ms),
            "--out",
            str(args.out),
            "--camera-frame-count",
            str(args.camera_frame_count),
            "--expected-codec",
            args.camera_compress,
        ]
        if expect_depth:
            client_cmd.append("--expect-depth")

        result = subprocess.run(
            client_cmd,
            cwd=ROOT / "py_example",
            text=True,
            capture_output=True,
        )
        print(result.stdout, end="")
        if result.stderr:
            print(result.stderr, end="", file=sys.stderr)
        if result.returncode != 0:
            raise RuntimeError(f"py_example failed with exit code {result.returncode}. See {sim_log_path}")
    finally:
        terminate_process(sim)
        log_thread.join(timeout=2.0)

    camera_path = args.out / "camera.png"
    if not camera_path.exists() or camera_path.stat().st_size <= 0:
        raise RuntimeError(f"camera.png was not produced. See {sim_log_path}")

    stats = png_rgb_stats(camera_path)
    if (
        stats["mean_rgb"] < 2.0
        or stats["max_channel"] < 16
        or stats["non_black_ratio"] < 0.01
        or stats["unique_sample"] < 4
    ):
        raise RuntimeError(f"camera.png appears blank or nearly black: {stats}. See {sim_log_path}")

    print(f"vision smoke passed: {camera_path}")
    transport_path = args.out / "transport.json"
    transport = json.loads(transport_path.read_text()) if transport_path.exists() else {}
    print(
        json.dumps(
            {
                "camera_compress": args.camera_compress,
                "jpeg_quality": args.jpeg_quality if args.camera_compress == "jpeg" else None,
                "camera_stats": stats,
                "transport": transport,
            },
            indent=2,
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
