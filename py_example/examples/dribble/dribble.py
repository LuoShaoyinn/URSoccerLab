#!/usr/bin/env python3
"""Look at the ball with the left eye, then walk forward to dribble it.

Launch the runtime with this folder's ``scene.json`` via the nDisplay backend
(run from the project root)::

    uv run --project py_example python Tools/runtime/run_scene.py \
      --scene-config py_example/examples/dribble/scene.json

The ball detector is the Ultralytics COCO ``yolo26s.pt`` checkpoint
(class 32 = sports ball, normalized to ``ball``); resize/NMS are handled by
Ultralytics so no fixed-shape ONNX is required. Defaults to the ROCm GPU
(``0``); pass ``--ultralytics-device cpu`` for CPU. The 50 Hz gait loop
remains independent from camera inference.
"""
from __future__ import annotations

import argparse
import json
import math
import queue
import threading
import time
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import torch
from PIL import Image, ImageDraw

from policy import (  # noqa: E402
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
from ursoccerlab.media import camera_to_rgb, write_video  # noqa: E402
from ursoccerlab.tcp import AdminClient, RobotClient  # noqa: E402

DEFAULT_ULTRALYTICS_PT = Path(
    "/home/luoshaoyinn/workspace/tmp2/refs/视觉_0625/"
    "soccer_backend_TensorRT(1)/soccer_backend_TensorRT/yolo26s.pt"
)

_COLORS = ["#ffb000", "#00d7ff", "#ff4f81", "#73d13d", "#9254de", "#36cfc9", "#fa541c"]


def annotate(frame: np.ndarray, detections: list[dict[str, object]]) -> np.ndarray:
    image = Image.fromarray(frame)
    draw = ImageDraw.Draw(image)
    for det in detections:
        cid = int(det["class_id"])
        color = _COLORS[cid % len(_COLORS)]
        x1, y1, x2, y2 = (float(v) for v in det["box_xyxy"])
        label = f'{det["class_name"]} {float(det["confidence"]):.2f}'
        draw.rectangle([x1, y1, x2, y2], outline=color, width=3)
        text_box = draw.textbbox((x1, y1), label)
        text_h = text_box[3] - text_box[1]
        label_y = max(0.0, y1 - text_h - 4)
        label_box = draw.textbbox((x1 + 2, label_y + 2), label)
        draw.rectangle((x1, label_y, label_box[2] + 2, label_box[3] + 2), fill=color)
        draw.text((x1 + 2, label_y + 2), label, fill="black")
    return np.asarray(image)


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
        ultralytics_pt: Path,
        device: str = "0",
        confidence: float = 0.15,
        iou: float = 0.45,
        imgsz: int = 640,
        class_id: int = 32,
    ):
        from ultralytics import YOLO

        self.confidence = confidence
        self.iou = iou
        self.imgsz = imgsz
        self.class_id = class_id
        self.pending: queue.Queue[tuple[np.ndarray, float] | None] = queue.Queue(maxsize=1)
        self.results: queue.Queue[VisionResult] = queue.Queue()
        self.error: BaseException | None = None
        self.ready = threading.Event()

        self.model = YOLO(str(ultralytics_pt))
        use_cuda = device not in ("cpu", "") and torch.cuda.is_available()
        self.device = torch.device("cuda" if use_cuda else "cpu")
        self.device_str = device if use_cuda else "cpu"
        self.backend = f"ultralytics:{Path(ultralytics_pt).name}:{self.device_str}"

        self.thread = threading.Thread(target=self._run, name="ball-yolo", daemon=True)
        self.thread.start()

    def _run(self) -> None:
        try:
            dummy = np.zeros((480, 640, 3), dtype=np.uint8)
            began = time.perf_counter()
            for _ in range(3):
                self.model.predict(
                    dummy, conf=self.confidence, iou=self.iou, imgsz=self.imgsz,
                    classes=[self.class_id], device=self.device_str, verbose=False,
                )
            if self.device.type == "cuda":
                torch.cuda.synchronize()
            print(
                f"[vision] Ultralytics warm-up completed in "
                f"{time.perf_counter() - began:.2f}s"
            )
            self.ready.set()
            while True:
                item = self.pending.get()
                if item is None:
                    return
                frame, sim_time = item
                started = time.perf_counter()
                results = self.model.predict(
                    frame, conf=self.confidence, iou=self.iou, imgsz=self.imgsz,
                    classes=[self.class_id], device=self.device_str, verbose=False,
                )
                result = results[0]
                out: list[dict[str, object]] = []
                if result.boxes is not None and len(result.boxes):
                    names = result.names
                    xyxy = result.boxes.xyxy.detach().cpu().numpy()
                    confs = result.boxes.conf.detach().cpu().numpy()
                    clses = result.boxes.cls.detach().cpu().numpy().astype(int)
                    for box, score, cls_id in zip(xyxy, confs, clses):
                        cname = str(names.get(int(cls_id), str(int(cls_id))))
                        if cname == "sports ball":
                            cname = "ball"
                        out.append(
                            {
                                "class_id": int(cls_id),
                                "class_name": cname,
                                "confidence": float(score),
                                "box_xyxy": [float(v) for v in box],
                            }
                        )
                elapsed_ms = (time.perf_counter() - started) * 1000.0
                self.results.put(VisionResult(frame, sim_time, out, elapsed_ms))
        except BaseException as exc:
            self.error = exc
            self.ready.set()

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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
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
    parser.add_argument("--ultralytics-pt", type=Path, default=DEFAULT_ULTRALYTICS_PT)
    parser.add_argument("--ultralytics-device", default="0", help="ROCm GPU id (0) | cpu")
    parser.add_argument("--ultralytics-conf", type=float, default=0.15)
    parser.add_argument("--ultralytics-imgsz", type=int, default=640)
    parser.add_argument(
        "--ultralytics-class-id", type=int, default=32,
        help="COCO class id to track (32 = sports ball)",
    )
    parser.add_argument("--seek-timeout", type=float, default=8.0)
    parser.add_argument("--duration", type=float, default=1.0, help="dribbling duration")
    parser.add_argument("--vx", type=float, default=0.3)
    parser.add_argument(
        "--vy-kp", type=float, default=0.55,
        help="lateral velocity gain for horizontal ball displacement",
    )
    parser.add_argument("--max-vy", type=float, default=0.4, help="maximum |vy| in m/s")
    parser.add_argument("--yaw-target", type=float, default=0.0, help="world yaw in rad")
    parser.add_argument("--yaw-kp", type=float, default=1.2)
    parser.add_argument("--yaw-ki", type=float, default=0.08)
    parser.add_argument("--yaw-kd", type=float, default=0.15)
    parser.add_argument("--yaw-integral-limit", type=float, default=0.5)
    parser.add_argument("--max-yaw-rate", type=float, default=0.5, help="maximum body |yaw rate|")
    parser.add_argument("--policy-hz", type=float, default=50.0)
    parser.add_argument("--head-kp", type=float, default=0.8)
    parser.add_argument("--head-max-rate", type=float, default=0.3, help="rad/s")
    parser.add_argument("--head-search-rate", type=float, default=0.15, help="rad/s")
    parser.add_argument("--video-fps", type=int, default=30)
    parser.add_argument("--video", type=Path, default=Path("out/dribble/left_eye.mp4"))
    parser.add_argument("--annotated-video", type=Path, default=Path("out/dribble/detections.mp4"))
    parser.add_argument("--observer-video", type=Path, default=Path("out/dribble/observer.mp4"))
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
                # During a dribble, continue in the last observed direction
                # instead of freezing when the close ball crosses the image
                # boundary.
                pitch_direction = 1.0 if ball_error_y >= -0.05 else -1.0
                head_pitch = float(
                    np.clip(
                        head_pitch + pitch_direction * args.head_search_rate * dt,
                        -0.5, 0.9,
                    )
                )
                if dribble_started is not None:
                    yaw_rate = float(
                        np.clip(-args.head_kp * ball_error_x, -args.head_max_rate, args.head_max_rate)
                    )
                    head_yaw = float(np.clip(head_yaw + yaw_rate * dt, -1.2, 1.2))
            else:
                x1, y1, x2, y2 = (float(v) for v in ball["box_xyxy"])
                height, width = result.frame.shape[:2]
                ball_error_x = ((x1 + x2) * 0.5 - width * 0.5) / (width * 0.5)
                ball_error_y = ((y1 + y2) * 0.5 - height * 0.55) / (height * 0.5)
                latest_ball_time = time.monotonic()
                yaw_rate = float(
                    np.clip(-args.head_kp * ball_error_x, -args.head_max_rate, args.head_max_rate)
                )
                pitch_rate = float(
                    np.clip(args.head_kp * ball_error_y, -args.head_max_rate, args.head_max_rate)
                )
                head_yaw = float(np.clip(head_yaw + yaw_rate * dt, -1.2, 1.2))
                # Positive MuJoCo rotation around +Y points the camera downward.
                head_pitch = float(np.clip(head_pitch + pitch_rate * dt, -0.5, 0.9))
                if abs(ball_error_x) < 0.12 and abs(ball_error_y) < 0.15:
                    centered_count += 1
                else:
                    centered_count = 0
                locked = allow_lock and centered_count >= 3
            annotated_frames.append(annotate(result.frame, result.detections))
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
            Image.fromarray(annotated_frames[-1]).save(args.trace.parent / "last_detection.png")

    def send_targets() -> None:
        if body_actuators is None or head_yaw_actuator is None or head_pitch_actuator is None:
            return
        command = {name: float(value) for name, value in zip(body_actuators, body_targets)}
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
                print(f"[control] reset robot_rp0 and {args.ball_actor} to their configured initial poses")
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

        send_targets()
        vision = VisionWorker(
            args.ultralytics_pt,
            device=args.ultralytics_device,
            confidence=args.ultralytics_conf,
            iou=0.45,
            imgsz=args.ultralytics_imgsz,
            class_id=args.ultralytics_class_id,
        )
        print(f"[vision] backend={vision.backend} device={vision.device}")

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
                print(f"[control] ball locked: yaw={head_yaw:+.3f} pitch={head_pitch:+.3f}")
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
                # Include head yaw to retain the actual body-relative direction,
                # corrected primarily with lateral motion.
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
                        -args.yaw_integral_limit, args.yaw_integral_limit,
                    )
                )
                angular_velocity = base.get("vel", [0.0] * 6)
                measured_yaw_rate = float(angular_velocity[5])
                yaw_command = np.clip(
                    args.yaw_kp * latest_yaw_error
                    + args.yaw_ki * yaw_integral
                    - args.yaw_kd * measured_yaw_rate,
                    -args.max_yaw_rate, args.max_yaw_rate,
                )
                command = np.asarray(
                    [
                        args.vx if ball_recent else 0.0,
                        # The locomotion policy uses +Y to move left. Camera-right
                        # is a positive image error, hence the minus.
                        np.clip(-args.vy_kp * steering_error, -args.max_vy, args.max_vy)
                        if ball_recent else 0.0,
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
                last_action = np.clip(action.numpy().squeeze().astype(np.float32), -100.0, 100.0)
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
