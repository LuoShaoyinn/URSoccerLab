#!/usr/bin/env python3
"""Measure URSoccerLab camera transport without decoding image payloads."""

from __future__ import annotations

import argparse
import json
import statistics
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PY_EXAMPLE = ROOT / "py_example"
sys.path.insert(0, str(PY_EXAMPLE))

from common.tcp import FrameConn, TYPE_CAMERA, parse_camera  # noqa: E402


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(int(round((len(ordered) - 1) * fraction)), len(ordered) - 1)
    return ordered[index]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=10000)
    parser.add_argument("--duration", type=float, default=15.0)
    parser.add_argument("--warmup", type=float, default=2.0)
    parser.add_argument("--expected-cameras", type=int, default=2)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    conn = FrameConn(args.host, args.port, timeout=10.0)
    started = time.monotonic()
    measuring_from = started + args.warmup
    deadline = measuring_from + args.duration

    message_arrivals: list[float] = []
    sim_times: list[float] = []
    camera_payload_bytes = 0
    wire_payload_bytes = 0
    camera_entries = 0
    complete_messages = 0
    incomplete_messages = 0
    codec_counts: dict[str, int] = {}
    resolutions: set[tuple[int, int]] = set()

    try:
        while time.monotonic() < deadline:
            for frame_type, payload in conn.recv_frames():
                now = time.monotonic()
                if frame_type != TYPE_CAMERA or now < measuring_from:
                    continue

                cameras = parse_camera(payload)
                nonempty = [camera for camera in cameras if camera["data"]]
                if len(cameras) == args.expected_cameras and len(nonempty) == len(cameras):
                    complete_messages += 1
                else:
                    incomplete_messages += 1

                message_arrivals.append(now)
                if cameras:
                    sim_times.append(float(cameras[0]["sim_time"]))
                wire_payload_bytes += len(payload)
                for camera in nonempty:
                    encoded = camera["data"]
                    camera_payload_bytes += len(encoded)
                    camera_entries += 1
                    codec = str(camera["codec"])
                    codec_counts[codec] = codec_counts.get(codec, 0) + 1
                    resolutions.add((int(camera["width"]), int(camera["height"])))

            time.sleep(0.0005)
    finally:
        conn.close()

    measured_seconds = max(time.monotonic() - measuring_from, 1e-9)
    wall_intervals_ms = [
        (current - previous) * 1000.0
        for previous, current in zip(message_arrivals, message_arrivals[1:])
    ]
    sim_intervals_ms = [
        (current - previous) * 1000.0
        for previous, current in zip(sim_times, sim_times[1:])
    ]
    duplicate_sim_timestamps = sum(
        current <= previous
        for previous, current in zip(sim_times, sim_times[1:])
    )

    result = {
        "duration_sec": measured_seconds,
        "camera_messages": len(message_arrivals),
        "complete_messages": complete_messages,
        "incomplete_messages": incomplete_messages,
        "message_rate_hz": len(message_arrivals) / measured_seconds,
        "camera_entries": camera_entries,
        "camera_entry_rate_hz": camera_entries / measured_seconds,
        "camera_payload_mib_per_sec": camera_payload_bytes / measured_seconds / (1024 * 1024),
        "wire_payload_mib_per_sec": wire_payload_bytes / measured_seconds / (1024 * 1024),
        "mean_camera_payload_bytes": (
            camera_payload_bytes / camera_entries if camera_entries else 0.0
        ),
        "codec_counts": codec_counts,
        "resolutions": sorted(resolutions),
        "wall_interval_ms": {
            "mean": statistics.fmean(wall_intervals_ms) if wall_intervals_ms else 0.0,
            "p50": percentile(wall_intervals_ms, 0.50),
            "p95": percentile(wall_intervals_ms, 0.95),
            "p99": percentile(wall_intervals_ms, 0.99),
            "max": max(wall_intervals_ms, default=0.0),
        },
        "sim_interval_ms": {
            "mean": statistics.fmean(sim_intervals_ms) if sim_intervals_ms else 0.0,
            "p50": percentile(sim_intervals_ms, 0.50),
            "p95": percentile(sim_intervals_ms, 0.95),
            "p99": percentile(sim_intervals_ms, 0.99),
            "max": max(sim_intervals_ms, default=0.0),
        },
        "duplicate_or_reversed_sim_timestamps": duplicate_sim_timestamps,
    }

    rendered = json.dumps(result, indent=2, sort_keys=True)
    print(rendered)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered + "\n", encoding="utf-8")

    return 0 if complete_messages > 0 and incomplete_messages == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
