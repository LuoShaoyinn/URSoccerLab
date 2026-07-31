#!/usr/bin/env python3
"""Look at the ball with the left eye, then walk forward to dribble it.

Run Unreal with ``Config/examples/walker_and_observer.json``.  Vision uses the
temporary fixed 736x1280 YOLO26n TorchScript cache on ROCm when available and
falls back to the source ONNX checkpoint on CPU.  The 50 Hz gait loop remains
independent from camera inference.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import queue
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import onnxruntime as ort
import torch
from PIL import Image

EXAMPLES_DIR = Path(__file__).resolve().parents[1]
REPO_ROOT = EXAMPLES_DIR.parents[1]
sys.path.insert(0, str(EXAMPLES_DIR))

from walk_policy import (  # noqa: E402
    ACTION_SCALE,
    DEFAULT_DOF,
    ISAAC_TO_MUJOCO,
    JOINTS_MUJOCO,
    OBS_HISTORY,
    OBS_STEP_DIM,
    POLICY_PATH,
    load_policy,
    observation,
)
from yolo_left_eye import (  # noqa: E402
    DEFAULT_CLASSES,
    annotate,
    decode_detections,
    letterbox,
)
from ursoccerlab.media import camera_to_rgb, write_video  # noqa: E402
from ursoccerlab.tcp import AdminClient, RobotClient  # noqa: E402


DEFAULT_ONNX = REPO_ROOT / "refs/vision/models/yolo26/yolo26n_best.onnx"
DEFAULT_TORCHSCRIPT = (
    REPO_ROOT / "py_example/out/models/yolo26n_best_736x1280.torchscript.pt"
)


@dataclass
class VisionResult:
    frame: np.ndarray
    sim_time: float
    detections: list[dict[str, object]]
    inference_ms: float


def yaw_from_wxyz(quaternion: list[float]) -> float:
    """Return world-frame yaw from the simulator's ``[w, x, y, z]`` IMU pose."""
    w, x, y, z = (float(value) for value in quaternion)
    return math.atan2(2.0 * (w * z + x * y), 1.0 - 2.0 * (y * y + z * z))


class VisionWorker:
    """Latest-frame-only detector worker; camera traffic never blocks control."""

    def __init__(
        self,
        onnx_path: Path,
        torchscript_path: Path,
        backend: str,
        confidence: float,
        iou: float,
    ):
        use_rocm = backend != "cpu" and torch.cuda.is_available()
        if backend == "gpu" and not use_rocm:
            raise RuntimeError("--vision-backend gpu requested, but ROCm is unavailable")
        if use_rocm and not torchscript_path.is_file():
            if backend == "gpu":
                raise FileNotFoundError(f"TorchScript cache not found: {torchscript_path}")
            use_rocm = False

        self.backend = "rocm-torchscript" if use_rocm else "cpu-onnxruntime"
        self.device = torch.device("cuda" if use_rocm else "cpu")
        self.confidence = confidence
        self.iou = iou
        self.pending: queue.Queue[tuple[np.ndarray, float] | None] = queue.Queue(maxsize=1)
        self.results: queue.Queue[VisionResult] = queue.Queue()
        self.error: BaseException | None = None
        self.ready = threading.Event()

        if use_rocm:
            self.model = torch.jit.load(str(torchscript_path), map_location=self.device).eval()
            self.session = None
        else:
            options = ort.SessionOptions()
            options.intra_op_num_threads = min(16, max(1, (os.cpu_count() or 2) // 2))
            options.inter_op_num_threads = 1
            self.session = ort.InferenceSession(
                str(onnx_path),
                sess_options=options,
                providers=["CPUExecutionProvider"],
            )
            self.model = None

        self.thread = threading.Thread(target=self._run, name="ball-yolo", daemon=True)
        self.thread.start()

    def submit(self, frame: np.ndarray, sim_time: float) -> None:
        try:
            self.pending.put_nowait((frame, sim_time))
        except queue.Full:
            try:
                self.pending.get_nowait()
            except queue.Empty:
                pass
            self.pending.put_nowait((frame, sim_time))

    def poll(self) -> list[VisionResult]:
        found: list[VisionResult] = []
        while True:
            try:
                found.append(self.results.get_nowait())
            except queue.Empty:
                break
        if self.error is not None:
            raise RuntimeError("vision worker failed") from self.error
        return found

    def close(self) -> None:
        try:
            self.pending.put_nowait(None)
        except queue.Full:
            try:
                self.pending.get_nowait()
            except queue.Empty:
                pass
            self.pending.put_nowait(None)
        self.thread.join(timeout=5.0)

    def _run(self) -> None:
        try:
            if self.model is not None:
                warmup = torch.zeros((1, 3, 736, 1280), device=self.device)
                warmup_started = time.perf_counter()
                with torch.inference_mode():
                    for _ in range(3):
                        self.model(warmup)
                torch.cuda.synchronize()
                print(
                    f"[vision] ROCm warm-up completed in "
                    f"{time.perf_counter() - warmup_started:.2f}s"
                )
            self.ready.set()
            while True:
                item = self.pending.get()
                if item is None:
                    return
                frame, sim_time = item
                image = Image.fromarray(frame)
                tensor, gain, pad_x, pad_y = letterbox(image, 1280, 736)
                started = time.perf_counter()
                if self.model is not None:
                    device_tensor = torch.from_numpy(tensor).to(self.device)
                    with torch.inference_mode():
                        output = self.model(device_tensor)
                    if self.device.type == "cuda":
                        torch.cuda.synchronize()
                    output_array = output.detach().cpu().numpy()
                else:
                    assert self.session is not None
                    output_array = self.session.run(None, {"images": tensor})[0]
                elapsed_ms = (time.perf_counter() - started) * 1000.0
                detections = decode_detections(
                    output_array,
                    image.size,
                    (1280, 736),
                    gain,
                    pad_x,
                    pad_y,
                    DEFAULT_CLASSES,
                    self.confidence,
                    self.iou,
                )
                self.results.put(VisionResult(frame, sim_time, detections, elapsed_ms))
        except BaseException as exc:
            self.error = exc
            self.ready.set()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=10000)
    parser.add_argument("--observer-port", type=int, default=10001)
    parser.add_argument("--admin-port", type=int, default=11000)
    parser.add_argument("--ball-actor", default="ball")
    parser.add_argument(
        "--reset-at-start",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="reset robot_rp0 and --ball-actor through the admin endpoint before control",
    )
    parser.add_argument("--policy", type=Path, default=POLICY_PATH)
    parser.add_argument("--onnx", type=Path, default=DEFAULT_ONNX)
    parser.add_argument("--torchscript", type=Path, default=DEFAULT_TORCHSCRIPT)
    parser.add_argument("--vision-backend", choices=("auto", "gpu", "cpu"), default="auto")
    parser.add_argument("--confidence", type=float, default=0.4)
    parser.add_argument("--iou", type=float, default=0.45)
    parser.add_argument("--seek-timeout", type=float, default=8.0)
    parser.add_argument("--duration", type=float, default=1.0, help="dribbling duration")
    parser.add_argument("--vx", type=float, default=0.3)
    parser.add_argument(
        "--vy-kp",
        type=float,
        default=0.55,
        help="lateral velocity gain for horizontal ball displacement",
    )
    parser.add_argument("--max-vy", type=float, default=0.4, help="maximum |vy| in m/s")
    parser.add_argument("--yaw-target", type=float, default=0.0, help="world yaw in rad")
    parser.add_argument("--yaw-kp", type=float, default=1.2)
    parser.add_argument("--yaw-ki", type=float, default=0.08)
    parser.add_argument("--yaw-kd", type=float, default=0.15)
    parser.add_argument("--yaw-integral-limit", type=float, default=0.5)
    parser.add_argument(
        "--max-yaw-rate", type=float, default=0.5, help="maximum body |yaw rate|"
    )
    parser.add_argument("--policy-hz", type=float, default=50.0)
    parser.add_argument("--head-kp", type=float, default=0.8)
    parser.add_argument("--head-max-rate", type=float, default=0.3, help="rad/s")
    parser.add_argument("--head-search-rate", type=float, default=0.15, help="rad/s")
    parser.add_argument("--video-fps", type=int, default=30)
    parser.add_argument("--video", type=Path, default=Path("out/dribble/left_eye.mp4"))
    parser.add_argument(
        "--annotated-video", type=Path, default=Path("out/dribble/detections.mp4")
    )
    parser.add_argument(
        "--observer-video", type=Path, default=Path("out/dribble/observer.mp4")
    )
    parser.add_argument("--trace", type=Path, default=Path("out/dribble/trace.json"))
    args = parser.parse_args()

    policy = load_policy(args.policy)
    vision: VisionWorker | None = None
    client = RobotClient(args.host, args.port)
    observer = RobotClient(args.host, args.observer_port)
    latest_state: dict | None = None
    body_actuators: list[str] | None = None
    head_yaw_actuator: str | None = None
    head_pitch_actuator: str | None = None
    body_targets = DEFAULT_DOF.copy()
    head_yaw = 0.0
    head_pitch = 0.0
    history = np.zeros(OBS_STEP_DIM * OBS_HISTORY, dtype=np.float32)
    last_action = np.zeros(20, dtype=np.float32)
    raw_frames: list[np.ndarray] = []
    annotated_frames: list[np.ndarray] = []
    observer_frames: list[np.ndarray] = []
    trace: list[dict[str, object]] = []
    next_record_time: float | None = None
    observer_next_record_time: float | None = None
    latest_ball_time = float("-inf")
    ball_error_x = 0.0
    ball_error_y = 0.0
    centered_count = 0
    inference_count = 0
    inference_ms_total = 0.0
    last_vision_sim_time: float | None = None
    latest_command = np.zeros(3, dtype=np.float32)
    latest_base_yaw = 0.0
    latest_yaw_error = 0.0
    yaw_integral = 0.0

    def pump(capture: bool = True) -> None:
        nonlocal latest_state, body_actuators, head_yaw_actuator, head_pitch_actuator
        nonlocal next_record_time
        for kind, payload in client.recv():
            if kind == "state":
                latest_state = payload
                if body_actuators is None:
                    available = set(payload.get("actuators", {}))
                    body_actuators = [f"{joint}_servo" for joint in JOINTS_MUJOCO]
                    head_yaw_actuator = "head_yaw_joint_servo"
                    head_pitch_actuator = "head_pitch_joint_servo"
                    required = body_actuators + [head_yaw_actuator, head_pitch_actuator]
                    missing = [name for name in required if name not in available]
                    if missing:
                        raise RuntimeError(f"robot is missing actuators: {missing}")
            elif kind in ("rgb", "camera") and payload:
                if not capture:
                    continue
                camera = next(
                    (item for item in payload if item.get("camera_name") == "left_eye"),
                    payload[0],
                )
                if not camera["data"]:
                    continue
                frame = camera_to_rgb(camera)
                sim_time = float(camera["sim_time"])
                assert vision is not None
                vision.submit(frame, sim_time)
                if next_record_time is None:
                    next_record_time = sim_time
                if sim_time + 1e-9 >= next_record_time:
                    raw_frames.append(frame)
                    interval = 1.0 / args.video_fps
                    while next_record_time <= sim_time + 1e-9:
                        next_record_time += interval

    def pump_observer(capture: bool = True) -> None:
        nonlocal observer_next_record_time
        for kind, payload in observer.recv():
            if kind not in ("rgb", "camera") or not payload:
                continue
            if not capture:
                continue
            camera = next(
                (item for item in payload if item.get("camera_name") == "left_eye"),
                payload[0],
            )
            if not camera["data"]:
                continue
            sim_time = float(camera["sim_time"])
            if observer_next_record_time is None:
                observer_next_record_time = sim_time
            if sim_time + 1e-9 >= observer_next_record_time:
                observer_frames.append(camera_to_rgb(camera))
                interval = 1.0 / args.video_fps
                while observer_next_record_time <= sim_time + 1e-9:
                    observer_next_record_time += interval

    def consume_vision(allow_lock: bool) -> bool:
        nonlocal head_yaw, head_pitch, centered_count, latest_ball_time
        nonlocal ball_error_x, ball_error_y, inference_count, inference_ms_total
        nonlocal last_vision_sim_time
        assert vision is not None
        locked = False
        for result in vision.poll():
            dt = (
                1.0 / 30.0
                if last_vision_sim_time is None
                else float(np.clip(result.sim_time - last_vision_sim_time, 1.0 / 120.0, 0.1))
            )
            last_vision_sim_time = result.sim_time
            inference_count += 1
            inference_ms_total += result.inference_ms
            balls = [d for d in result.detections if d["class_name"] == "ball"]
            ball = max(balls, key=lambda d: float(d["confidence"]), default=None)
            if ball is None:
                centered_count = 0
                # Initially the ball is below rp0's zero-pitch FOV. During a
                # dribble, continue in the last observed direction instead of
                # freezing when the close ball crosses the image boundary.
                pitch_direction = 1.0 if ball_error_y >= -0.05 else -1.0
                head_pitch = float(
                    np.clip(
                        head_pitch + pitch_direction * args.head_search_rate * dt,
                        -0.5,
                        0.9,
                    )
                )
                if dribble_started is not None:
                    yaw_rate = float(
                        np.clip(
                            -args.head_kp * ball_error_x,
                            -args.head_max_rate,
                            args.head_max_rate,
                        )
                    )
                    head_yaw = float(np.clip(head_yaw + yaw_rate * dt, -1.2, 1.2))
            else:
                x1, y1, x2, y2 = (float(v) for v in ball["box_xyxy"])
                height, width = result.frame.shape[:2]
                ball_error_x = ((x1 + x2) * 0.5 - width * 0.5) / (width * 0.5)
                ball_error_y = ((y1 + y2) * 0.5 - height * 0.55) / (height * 0.5)
                latest_ball_time = time.monotonic()
                yaw_rate = float(
                    np.clip(
                        -args.head_kp * ball_error_x,
                        -args.head_max_rate,
                        args.head_max_rate,
                    )
                )
                pitch_rate = float(
                    np.clip(
                        args.head_kp * ball_error_y,
                        -args.head_max_rate,
                        args.head_max_rate,
                    )
                )
                head_yaw = float(np.clip(head_yaw + yaw_rate * dt, -1.2, 1.2))
                # Positive MuJoCo rotation around +Y points the camera downward.
                head_pitch = float(np.clip(head_pitch + pitch_rate * dt, -0.5, 0.9))
                if abs(ball_error_x) < 0.12 and abs(ball_error_y) < 0.15:
                    centered_count += 1
                else:
                    centered_count = 0
                locked = allow_lock and centered_count >= 3
            annotated_frames.append(
                np.asarray(annotate(Image.fromarray(result.frame), result.detections))
            )
            trace.append(
                {
                    "sim_time": result.sim_time,
                    "phase": "dribble" if dribble_started is not None else "seek",
                    "inference_ms": result.inference_ms,
                    "head_yaw": head_yaw,
                    "head_pitch": head_pitch,
                    "command": latest_command.tolist(),
                    "base_yaw": latest_base_yaw,
                    "yaw_error": latest_yaw_error,
                    "base_pos": latest_state["base"]["pos"] if latest_state else None,
                    "ball": ball,
                }
            )
            if inference_count % 10 == 0:
                ball_conf = float(ball["confidence"]) if ball is not None else 0.0
                print(
                    f"[vision] frame={inference_count} ball={ball_conf:.2f} "
                    f"error=({ball_error_x:+.2f},{ball_error_y:+.2f}) "
                    f"head=({head_yaw:+.2f},{head_pitch:+.2f}) "
                    f"infer={result.inference_ms:.1f}ms"
                )
        return locked

    def save_failure_artifacts() -> None:
        args.trace.parent.mkdir(parents=True, exist_ok=True)
        args.trace.write_text(json.dumps(trace, indent=2) + "\n", encoding="utf-8")
        if annotated_frames:
            Image.fromarray(annotated_frames[-1]).save(
                args.trace.parent / "last_detection.png"
            )

    def send_targets() -> None:
        if body_actuators is None or head_yaw_actuator is None or head_pitch_actuator is None:
            return
        command = {
            name: float(value) for name, value in zip(body_actuators, body_targets)
        }
        command[head_yaw_actuator] = head_yaw
        command[head_pitch_actuator] = head_pitch
        client.send_command(command)

    dribble_started: float | None = None
    start_x = 0.0
    try:
        if args.reset_at_start:
            admin = AdminClient(args.host, args.admin_port)
            try:
                for actor_id in ("robot_rp0", args.ball_actor):
                    reply = admin.reset(actor_id)
                    if not reply.get("ok", False):
                        raise RuntimeError(f"failed to reset {actor_id}: {reply}")
                print(
                    f"[control] reset robot_rp0 and {args.ball_actor} "
                    "to their configured initial poses"
                )
            finally:
                admin.close()

        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline and (latest_state is None or body_actuators is None):
            pump(False)
            pump_observer(False)
            send_targets()
            time.sleep(0.002)
        if latest_state is None or body_actuators is None:
            raise RuntimeError(f"no robot state received on {args.host}:{args.port}")

        # Establish the standing controls before ROCm performs its expensive
        # first-use kernel compilation. MuJoCo retains these actuator targets
        # while the constructor warms the fixed TorchScript graph.
        send_targets()
        vision = VisionWorker(
            args.onnx,
            args.torchscript,
            args.vision_backend,
            args.confidence,
            args.iou,
        )
        print(f"[vision] backend={vision.backend} device={vision.device}")

        # Warm-up runs in the vision worker. Keep draining network traffic and
        # refreshing the standing command so neither physics nor TCP stalls.
        while not vision.ready.is_set():
            pump(False)
            pump_observer(False)
            send_targets()
            time.sleep(0.002)
        vision.poll()  # Surface a warm-up failure before seeking.

        print("[control] holding stance and centering the ball")
        seek_deadline = time.monotonic() + args.seek_timeout
        while time.monotonic() < seek_deadline:
            pump()
            pump_observer()
            if consume_vision(True):
                dribble_started = time.monotonic()
                start_x = float(latest_state["base"]["pos"][0])
                print(
                    f"[control] ball locked: yaw={head_yaw:+.3f} "
                    f"pitch={head_pitch:+.3f}"
                )
                break
            send_targets()
            time.sleep(0.001)
        if dribble_started is None:
            save_failure_artifacts()
            raise RuntimeError("ball was not centered before --seek-timeout")

        next_policy = time.monotonic()
        last_policy_time = next_policy
        while time.monotonic() - dribble_started < args.duration:
            pump()
            pump_observer()
            consume_vision(False)
            now = time.monotonic()
            if latest_state is not None:
                base = latest_state["base"]
                quat = base["quat"]  # [w, x, y, z]
                up = 1.0 - 2.0 * (float(quat[1]) ** 2 + float(quat[2]) ** 2)
                if float(base["pos"][2]) < 0.22 or up < 0.55:
                    print(f"[control] stopping on fall guard: z={base['pos'][2]:.2f} up={up:.2f}")
                    break
            if latest_state is not None and now >= next_policy:
                policy_dt = float(np.clip(now - last_policy_time, 1.0 / 200.0, 0.1))
                last_policy_time = now
                ball_recent = now - latest_ball_time < 0.75
                # Horizontal image error alone approaches zero once the head
                # tracks the ball. Include head yaw to retain the actual body-
                # relative direction, and correct it primarily with lateral
                # motion instead of forcing the robot to turn in place.
                steering_error = ball_error_x - head_yaw
                base = latest_state["base"]
                latest_base_yaw = yaw_from_wxyz(base["quat"])
                latest_yaw_error = math.atan2(
                    math.sin(args.yaw_target - latest_base_yaw),
                    math.cos(args.yaw_target - latest_base_yaw),
                )
                yaw_integral = float(
                    np.clip(
                        yaw_integral + latest_yaw_error * policy_dt,
                        -args.yaw_integral_limit,
                        args.yaw_integral_limit,
                    )
                )
                angular_velocity = base.get("vel", [0.0] * 6)
                measured_yaw_rate = float(angular_velocity[5])
                yaw_command = np.clip(
                    args.yaw_kp * latest_yaw_error
                    + args.yaw_ki * yaw_integral
                    - args.yaw_kd * measured_yaw_rate,
                    -args.max_yaw_rate,
                    args.max_yaw_rate,
                )
                command = np.asarray(
                    [
                        args.vx if ball_recent else 0.0,
                        # The locomotion policy uses +Y to move left. Camera-
                        # right is a positive image error, hence the minus.
                        np.clip(-args.vy_kp * steering_error, -args.max_vy, args.max_vy)
                        if ball_recent
                        else 0.0,
                        yaw_command,
                    ],
                    dtype=np.float32,
                )
                latest_command[:] = command
                step = observation(latest_state, command, last_action)
                history = np.roll(history, -OBS_STEP_DIM)
                history[-OBS_STEP_DIM:] = step
                with torch.inference_mode():
                    action = policy(
                        torch.from_numpy(np.clip(history, -100.0, 100.0)).unsqueeze(0)
                    )
                last_action = np.clip(
                    action.numpy().squeeze().astype(np.float32), -100.0, 100.0
                )
                body_targets = last_action[ISAAC_TO_MUJOCO] * ACTION_SCALE + DEFAULT_DOF
                next_policy = now + 1.0 / args.policy_hz
            send_targets()
            time.sleep(0.001)

        body_targets = DEFAULT_DOF.copy()
        send_targets()
        pump()
        pump_observer()
        consume_vision(False)
        end_x = float(latest_state["base"]["pos"][0]) if latest_state else start_x
        mean_ms = inference_ms_total / inference_count if inference_count else float("nan")
        print(f"[control] forward displacement: {end_x - start_x:+.3f} m")
        print(
            f"[control] final yaw: {math.degrees(latest_base_yaw):+.2f} deg "
            f"(error {math.degrees(latest_yaw_error):+.2f} deg)"
        )
        print(f"[vision] {inference_count} frames, mean inference {mean_ms:.2f} ms")
        write_video(raw_frames, args.video, args.video_fps)
        write_video(annotated_frames, args.annotated_video, args.video_fps)
        write_video(observer_frames, args.observer_video, args.video_fps)
        args.trace.parent.mkdir(parents=True, exist_ok=True)
        args.trace.write_text(json.dumps(trace, indent=2) + "\n", encoding="utf-8")
        print(f"saved {args.trace}")
        return 0
    finally:
        client.close()
        observer.close()
        if vision is not None:
            vision.close()


if __name__ == "__main__":
    raise SystemExit(main())
