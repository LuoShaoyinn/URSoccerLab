#!/usr/bin/env python3
"""Benchmark every robot camera stream while checking physics clock progress.

The harness launches the tracked soccer field, connects one TCP client to every
robot in a scene config, drains all state/RGB/depth messages, and compares
MuJoCo simulation time with wall time. This exercises the actual 3v3 transport
load instead of opening a single robot port.
"""

from __future__ import annotations

import argparse
import json
import signal
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PY_EXAMPLE = ROOT / "py_example"
sys.path.insert(0, str(PY_EXAMPLE / "src"))

from ursoccerlab.tcp import (  # noqa: E402
    FrameConn,
    TYPE_DEPTH,
    TYPE_JSON,
    TYPE_RGB,
    parse_image_message,
)
from ndisplay_config import write_ndisplay_config


DEFAULT_UE = Path.home() / "Unreal_Engine_5.7.4/Engine/Binaries/Linux/UnrealEditor"
PROJECT = ROOT / "URSoccerLab.uproject"
MAP_PATH = "/Game/Levels/URS_SoccerField"


@dataclass
class ModalityStats:
    messages: int = 0
    entries: int = 0
    payload_bytes: int = 0
    wire_payload_bytes: int = 0
    incomplete_messages: int = 0
    sequence_gaps: int = 0
    last_sequence: int | None = None
    codecs: dict[str, int] = field(default_factory=dict)
    resolutions: set[tuple[int, int]] = field(default_factory=set)

    def add(self, payload: bytes, expected_entries: int) -> None:
        images = parse_image_message(payload)
        self.messages += 1
        self.entries += len(images)
        self.wire_payload_bytes += len(payload)
        if len(images) != expected_entries or any(not image["data"] for image in images):
            self.incomplete_messages += 1
        if images:
            sequence = int(images[0]["sequence"])
            if self.last_sequence is not None and sequence > self.last_sequence + 1:
                self.sequence_gaps += sequence - self.last_sequence - 1
            self.last_sequence = sequence
        for image in images:
            encoded = image["data"]
            self.payload_bytes += len(encoded)
            codec = str(image["codec"])
            self.codecs[codec] = self.codecs.get(codec, 0) + 1
            self.resolutions.add((int(image["width"]), int(image["height"])))

    def render(self, duration: float) -> dict:
        return {
            "messages": self.messages,
            "message_rate_hz": self.messages / duration,
            "entries": self.entries,
            "entry_rate_hz": self.entries / duration,
            "payload_mib_per_sec": self.payload_bytes / duration / (1024 * 1024),
            "wire_payload_mib_per_sec": (
                self.wire_payload_bytes / duration / (1024 * 1024)
            ),
            "incomplete_messages": self.incomplete_messages,
            "sequence_gaps": self.sequence_gaps,
            "codecs": self.codecs,
            "resolutions": sorted(self.resolutions),
        }


@dataclass
class RobotStats:
    actor_id: str
    port: int
    state_messages: int = 0
    first_state_wall: float | None = None
    last_state_wall: float | None = None
    first_sim_time: float | None = None
    last_sim_time: float | None = None
    rgb: ModalityStats = field(default_factory=ModalityStats)
    depth: ModalityStats = field(default_factory=ModalityStats)

    def add_state(self, payload: bytes, now: float) -> None:
        state = json.loads(payload.decode("utf-8"))
        sim_time = float(state["sim_time"])
        self.state_messages += 1
        if self.first_state_wall is None:
            self.first_state_wall = now
            self.first_sim_time = sim_time
        self.last_state_wall = now
        self.last_sim_time = sim_time

    def render(self, duration: float) -> dict:
        state_wall_delta = 0.0
        sim_delta = 0.0
        if self.first_state_wall is not None and self.last_state_wall is not None:
            state_wall_delta = self.last_state_wall - self.first_state_wall
        if self.first_sim_time is not None and self.last_sim_time is not None:
            sim_delta = self.last_sim_time - self.first_sim_time
        return {
            "actor_id": self.actor_id,
            "port": self.port,
            "state_messages": self.state_messages,
            "state_rate_hz": self.state_messages / duration,
            "state_wall_delta_sec": state_wall_delta,
            "sim_delta_sec": sim_delta,
            "physics_realtime_ratio": (
                sim_delta / state_wall_delta if state_wall_delta > 0.0 else 0.0
            ),
            "rgb": self.rgb.render(duration),
            "depth": self.depth.render(duration),
        }


def terminate_process(proc: subprocess.Popen[str], timeout_sec: float = 10.0) -> None:
    if proc.poll() is not None:
        return
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=timeout_sec)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=timeout_sec)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ue", type=Path, default=DEFAULT_UE)
    parser.add_argument(
        "--scene-config",
        type=Path,
        default=ROOT / "Config/examples/six_robots_rgbd.json",
    )
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--base-port", type=int, default=10000)
    parser.add_argument("--warmup-sec", type=float, default=3.0)
    parser.add_argument("--duration-sec", type=float, default=15.0)
    parser.add_argument("--timeout-sec", type=float, default=60.0)
    parser.add_argument("--min-physics-realtime-ratio", type=float, default=0.90)
    parser.add_argument(
        "--scene-capture",
        action="store_true",
        help="Use the legacy independent SceneCapture backend instead of production nDisplay.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=ROOT / "Saved/Benchmarks/match_vision.json",
    )
    parser.add_argument(
        "--sim-extra-arg",
        action="append",
        default=[],
        help="Additional Unreal command-line argument; may be repeated.",
    )
    args = parser.parse_args()

    scene_path = args.scene_config.resolve()
    config = json.loads(scene_path.read_text(encoding="utf-8"))
    robots = config["robots"]
    mode = config.get("vision", {}).get("mode", "stereo_rgb")
    expected_rgb_entries = 2 if mode == "stereo_rgb" else 1
    expect_depth = mode == "rgbd"
    rgb_view_count = len(robots) * expected_rgb_entries

    log_path = ROOT / "Saved/Logs/URS_MatchVisionBenchmark.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    ready = threading.Event()
    render_args: list[str]
    if args.scene_capture:
        render_args = ["-ForceRes", "-ResX=64", "-ResY=64"]
    else:
        ndisplay_path = (
            ROOT / "Saved/Generated/NDisplay"
            / f"match_{rgb_view_count}_rgb.ndisplay"
        )
        atlas_width, atlas_height = write_ndisplay_config(
            rgb_view_count, ndisplay_path
        )
        render_args = [
            "-ForceRes",
            f"-ResX={atlas_width}",
            f"-ResY={atlas_height}",
            "-dc_cluster",
            "-dc_dev_mono",
            f"-dc_cfg={ndisplay_path}",
            "-dc_node=node_0",
            "-URSNDisplayCameras",
            f"-URSNDisplayCameraCount={rgb_view_count}",
            "-ExecCmds=MjCamera.AutoReadback 0,DisableAllScreenMessages",
        ]
        if mode == "rgbd":
            render_args.append(
                f"-URSNDisplayCameraName={config['vision'].get('left_camera', 'left_eye')}"
            )

    command = [
        str(args.ue),
        str(PROJECT),
        MAP_PATH,
        "-game",
        "-RenderOffscreen",
        *render_args,
        "-unattended",
        "-nop4",
        "-nosplash",
        "-NoSound",
        "-URSCameraStats",
        f"-URSSceneConfig={scene_path}",
        *args.sim_extra_arg,
    ]
    print("+", " ".join(command), flush=True)
    simulator = subprocess.Popen(
        command,
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )

    def drain_log() -> None:
        with log_path.open("w", encoding="utf-8") as log:
            if simulator.stdout is None:
                return
            for line in simulator.stdout:
                log.write(line)
                log.flush()
                if " listening on port 10000" in line:
                    ready.set()

    log_thread = threading.Thread(target=drain_log, name="urs-match-log", daemon=True)
    log_thread.start()
    connections: list[FrameConn] = []

    try:
        deadline = time.monotonic() + args.timeout_sec
        while not ready.is_set() and time.monotonic() < deadline:
            if simulator.poll() is not None:
                raise RuntimeError(
                    f"simulator exited early with {simulator.returncode}; see {log_path}"
                )
            time.sleep(0.1)
        if not ready.is_set():
            raise TimeoutError(f"robot listeners did not start; see {log_path}")

        for index, _robot in enumerate(robots):
            connections.append(
                FrameConn(args.host, args.base_port + index, timeout=args.timeout_sec)
            )

        stats = [
            RobotStats(str(robot["actor_id"]), args.base_port + index)
            for index, robot in enumerate(robots)
        ]
        measurement_start = time.monotonic() + args.warmup_sec
        measurement_end = measurement_start + args.duration_sec

        while time.monotonic() < measurement_end:
            for connection, robot_stats in zip(connections, stats):
                for frame_type, payload in connection.recv_frames():
                    now = time.monotonic()
                    if now < measurement_start:
                        continue
                    if frame_type == TYPE_JSON:
                        robot_stats.add_state(payload, now)
                    elif frame_type == TYPE_RGB:
                        robot_stats.rgb.add(payload, expected_rgb_entries)
                    elif frame_type == TYPE_DEPTH:
                        robot_stats.depth.add(payload, 1)
            time.sleep(0.0005)

        rendered_robots = [
            robot_stats.render(args.duration_sec) for robot_stats in stats
        ]
        ratios = [robot["physics_realtime_ratio"] for robot in rendered_robots]
        result = {
            "scene_config": str(scene_path),
            "mode": mode,
            "render_backend": "scene_capture" if args.scene_capture else "ndisplay",
            "robot_count": len(robots),
            "rgb_stream_count": len(robots) * expected_rgb_entries,
            "depth_stream_count": len(robots) if expect_depth else 0,
            "duration_sec": args.duration_sec,
            "physics_realtime_ratio_min": min(ratios, default=0.0),
            "physics_realtime_ratio_mean": (
                sum(ratios) / len(ratios) if ratios else 0.0
            ),
            "robots": rendered_robots,
        }
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        print(json.dumps(result, indent=2, sort_keys=True))

        failures: list[str] = []
        for robot in rendered_robots:
            actor_id = robot["actor_id"]
            if robot["state_messages"] < 2:
                failures.append(f"{actor_id}: fewer than two state messages")
            if robot["physics_realtime_ratio"] < args.min_physics_realtime_ratio:
                failures.append(
                    f"{actor_id}: physics realtime ratio "
                    f"{robot['physics_realtime_ratio']:.3f} below "
                    f"{args.min_physics_realtime_ratio:.3f}"
                )
            if robot["rgb"]["messages"] == 0:
                failures.append(f"{actor_id}: no RGB messages")
            if robot["rgb"]["incomplete_messages"] != 0:
                failures.append(f"{actor_id}: incomplete RGB message")
            if expect_depth and robot["depth"]["messages"] == 0:
                failures.append(f"{actor_id}: no depth messages")
            if expect_depth and robot["depth"]["incomplete_messages"] != 0:
                failures.append(f"{actor_id}: incomplete depth message")
            if not expect_depth and robot["depth"]["messages"] != 0:
                failures.append(f"{actor_id}: unexpected depth message")

        if failures:
            print("benchmark failed:", file=sys.stderr)
            for failure in failures:
                print(f"- {failure}", file=sys.stderr)
            return 1
        return 0
    finally:
        for connection in connections:
            connection.close()
        terminate_process(simulator)
        log_thread.join(timeout=2.0)


if __name__ == "__main__":
    raise SystemExit(main())
