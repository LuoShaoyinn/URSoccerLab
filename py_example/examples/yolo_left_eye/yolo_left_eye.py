#!/usr/bin/env python3
"""Run the trained YOLO26 detector on one simulated left-eye frame.

With no ``--image``, this connects to a running URSoccerLab robot and uses RGB
camera index 0 (the left eye).  ``--image`` is useful for repeatable inference
on a frame captured by an earlier example.
"""
from __future__ import annotations

import argparse
import ast
import json
import time
from pathlib import Path

import numpy as np
import onnxruntime as ort
from PIL import Image, ImageDraw

from ursoccerlab.media import camera_to_rgb
from ursoccerlab.tcp import RobotClient


REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_MODEL = REPO_ROOT / "refs/vision/models/yolo26/yolo26s_best.onnx"
DEFAULT_CLASSES = {
    0: "ball",
    1: "goalpost",
    2: "robot",
    3: "L-Intersection",
    4: "T-Intersection",
    5: "X-Intersection",
    6: "obstacle",
}
COLORS = [
    "#ffb000",
    "#00d7ff",
    "#ff4f81",
    "#73d13d",
    "#9254de",
    "#36cfc9",
    "#fa541c",
]


def capture_left_eye(host: str, port: int, timeout_s: float) -> Image.Image:
    """Receive one RGB set and return camera index 0, the left eye."""
    client = RobotClient(host, port)
    deadline = time.monotonic() + timeout_s
    try:
        while time.monotonic() < deadline:
            for kind, payload in client.recv():
                if kind not in ("rgb", "camera") or not payload:
                    continue
                left_eye = payload[0]
                if left_eye["data"]:
                    name = left_eye.get("camera_name", "camera index 0")
                    print(f"Captured left eye: {name}")
                    return Image.fromarray(camera_to_rgb(left_eye))
            time.sleep(0.001)
    finally:
        client.close()
    raise TimeoutError(f"no left-eye RGB frame received from {host}:{port}")


def class_names(session: ort.InferenceSession) -> dict[int, str]:
    encoded = session.get_modelmeta().custom_metadata_map.get("names")
    if not encoded:
        return DEFAULT_CLASSES
    parsed = ast.literal_eval(encoded)
    return {int(class_id): str(name) for class_id, name in parsed.items()}


def letterbox(
    image: Image.Image, input_w: int, input_h: int
) -> tuple[np.ndarray, float, int, int]:
    src_w, src_h = image.size
    gain = min(input_w / src_w, input_h / src_h)
    resized_w = max(1, round(src_w * gain))
    resized_h = max(1, round(src_h * gain))
    pad_x = max(0, (input_w - resized_w) // 2)
    pad_y = max(0, (input_h - resized_h) // 2)

    canvas = Image.new("RGB", (input_w, input_h), (114, 114, 114))
    resized = image.resize((resized_w, resized_h), Image.Resampling.BILINEAR)
    canvas.paste(resized, (pad_x, pad_y))
    tensor = np.asarray(canvas, dtype=np.float32).transpose(2, 0, 1)
    return np.ascontiguousarray(tensor[None] / 255.0), gain, pad_x, pad_y


def infer(
    session: ort.InferenceSession,
    image: Image.Image,
    confidence: float,
    iou_threshold: float,
) -> tuple[list[dict[str, object]], float]:
    model_input = session.get_inputs()[0]
    shape = model_input.shape
    if len(shape) != 4 or not isinstance(shape[2], int) or not isinstance(shape[3], int):
        raise ValueError(f"expected a fixed NCHW model input, received {shape}")
    input_h, input_w = shape[2], shape[3]
    tensor, gain, pad_x, pad_y = letterbox(image, input_w, input_h)

    started = time.perf_counter()
    output = session.run(None, {model_input.name: tensor})[0]
    elapsed_ms = (time.perf_counter() - started) * 1000.0

    detections = decode_detections(
        output,
        image.size,
        (input_w, input_h),
        gain,
        pad_x,
        pad_y,
        class_names(session),
        confidence,
        iou_threshold,
    )
    return detections, elapsed_ms


def decode_detections(
    output: np.ndarray,
    image_size: tuple[int, int],
    input_size: tuple[int, int],
    gain: float,
    pad_x: int,
    pad_y: int,
    names: dict[int, str],
    confidence: float,
    iou_threshold: float,
) -> list[dict[str, object]]:
    """Map an end-to-end-shaped YOLO tensor back to source-image pixels."""

    if output.ndim == 3:
        output = output[0]
    if output.ndim != 2:
        raise ValueError(f"expected a 2-D detection tensor, received {output.shape}")
    if output.shape[1] != 6 and output.shape[0] == 6:
        output = output.T
    if output.shape[1] != 6:
        raise ValueError(
            f"expected end-to-end [N,6] detections, received {output.shape}"
        )

    input_w, input_h = input_size
    src_w, src_h = image_size
    detections: list[dict[str, object]] = []
    for x1, y1, x2, y2, score, class_value in output:
        if float(score) < confidence:
            continue
        class_id = int(round(float(class_value)))
        if class_id not in names:
            continue
        if 0.0 <= x1 and x2 <= 1.5 and 0.0 <= y1 and y2 <= 1.5:
            x1, x2 = x1 * input_w, x2 * input_w
            y1, y2 = y1 * input_h, y2 * input_h
        box = [
            float(np.clip((x1 - pad_x) / gain, 0, src_w - 1)),
            float(np.clip((y1 - pad_y) / gain, 0, src_h - 1)),
            float(np.clip((x2 - pad_x) / gain, 0, src_w - 1)),
            float(np.clip((y2 - pad_y) / gain, 0, src_h - 1)),
        ]
        if box[2] <= box[0] or box[3] <= box[1]:
            continue
        detections.append(
            {
                "class_id": class_id,
                "class_name": names[class_id],
                "confidence": float(score),
                "box_xyxy": box,
            }
        )
    return non_max_suppression(detections, iou_threshold)


def non_max_suppression(
    detections: list[dict[str, object]], iou_threshold: float
) -> list[dict[str, object]]:
    """Apply class-aware NMS to the checkpoint's end-to-end-shaped output."""
    kept: list[dict[str, object]] = []
    for candidate in sorted(
        detections, key=lambda item: float(item["confidence"]), reverse=True
    ):
        candidate_box = np.asarray(candidate["box_xyxy"], dtype=np.float32)
        suppress = False
        for accepted in kept:
            if candidate["class_id"] != accepted["class_id"]:
                continue
            accepted_box = np.asarray(accepted["box_xyxy"], dtype=np.float32)
            intersection_min = np.maximum(candidate_box[:2], accepted_box[:2])
            intersection_max = np.minimum(candidate_box[2:], accepted_box[2:])
            intersection_wh = np.maximum(0.0, intersection_max - intersection_min)
            intersection = float(intersection_wh[0] * intersection_wh[1])
            candidate_area = float(np.prod(candidate_box[2:] - candidate_box[:2]))
            accepted_area = float(np.prod(accepted_box[2:] - accepted_box[:2]))
            union = candidate_area + accepted_area - intersection
            if union > 0.0 and intersection / union > iou_threshold:
                suppress = True
                break
        if not suppress:
            kept.append(candidate)
    return kept


def annotate(image: Image.Image, detections: list[dict[str, object]]) -> Image.Image:
    result = image.copy()
    draw = ImageDraw.Draw(result)
    for detection in detections:
        class_id = int(detection["class_id"])
        box = tuple(float(value) for value in detection["box_xyxy"])
        color = COLORS[class_id % len(COLORS)]
        label = f'{detection["class_name"]} {float(detection["confidence"]):.2f}'
        draw.rectangle(box, outline=color, width=3)
        text_box = draw.textbbox((box[0], box[1]), label)
        text_h = text_box[3] - text_box[1]
        label_y = max(0.0, box[1] - text_h - 4)
        label_box = draw.textbbox((box[0] + 2, label_y + 2), label)
        draw.rectangle(
            (box[0], label_y, label_box[2] + 2, label_box[3] + 2), fill=color
        )
        draw.text((box[0] + 2, label_y + 2), label, fill="black")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, help="use a saved frame instead of TCP")
    parser.add_argument("--model", type=Path, default=DEFAULT_MODEL)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=10000)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--confidence", type=float, default=0.4)
    parser.add_argument("--iou", type=float, default=0.45)
    parser.add_argument("--out", type=Path, default=Path("out/yolo_left_eye"))
    args = parser.parse_args()

    if not 0.0 <= args.confidence <= 1.0:
        parser.error("--confidence must be between 0 and 1")
    if not 0.0 <= args.iou <= 1.0:
        parser.error("--iou must be between 0 and 1")
    if not args.model.is_file():
        parser.error(f"model does not exist: {args.model}")

    image = (
        Image.open(args.image).convert("RGB")
        if args.image
        else capture_left_eye(args.host, args.port, args.timeout)
    )
    args.out.mkdir(parents=True, exist_ok=True)
    image.save(args.out / "left_eye.png")

    session = ort.InferenceSession(
        str(args.model), providers=["CPUExecutionProvider"]
    )
    detections, elapsed_ms = infer(session, image, args.confidence, args.iou)
    result = {
        "model": str(args.model.resolve()),
        "provider": session.get_providers()[0],
        "source": str(args.image.resolve()) if args.image else f"{args.host}:{args.port}",
        "camera": "left_eye",
        "image_size": list(image.size),
        "confidence_threshold": args.confidence,
        "iou_threshold": args.iou,
        "inference_ms": elapsed_ms,
        "detections": detections,
    }
    (args.out / "detections.json").write_text(
        json.dumps(result, indent=2) + "\n", encoding="utf-8"
    )
    annotate(image, detections).save(args.out / "annotated.png")

    print(f"YOLO: {len(detections)} detection(s) in {elapsed_ms:.1f} ms")
    for detection in detections:
        print(
            f'  {detection["class_name"]:<16} '
            f'{float(detection["confidence"]):.3f} {detection["box_xyxy"]}'
        )
    print(f"Wrote {args.out / 'annotated.png'} and {args.out / 'detections.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
