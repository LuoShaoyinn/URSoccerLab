#!/usr/bin/env python3
"""Generate a reproducible indoor synthetic-turf pitch and PBR texture set.

The pitch image and blade mesh cover the playing surface and visual border.
The detail maps tile every 0.5 m and are visual-only; MuJoCo continues to use
the flat field_ground plane and the friction settings in scene.json.

Run from any directory:

    python3 Tools/field/generate_grass_assets.py

The default output is external/field/, which is versioned so a fresh checkout
can launch the bundled field scenes. Pass --out-dir to place generated files
elsewhere.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import struct
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUT = ROOT / "external" / "field"
DEFAULT_GRASS_MESH_OUT = ROOT / "Assets" / "Scenes" / "SoccerField" / "visual" / "grass_blades.glb"
DEFAULT_TURF_ALBEDO_TILE = ROOT / "Assets" / "Scenes" / "SoccerField" / "visual" / "grass_albedo_tile.png"
DETAIL_DIR = "grass1-ue"
DETAIL_TILE_SIZE_M = 0.5
TURF_COLOR_TILE_SIZE_M = 0.5
TURF_FIBERS_PER_M2 = 180_000
GRASS_FIBERS_PER_M2 = 180_000
LINE_WIDTH_M = 0.05
MAX_TEXTURE_DIMENSION = 8192
POSITION_QUANTUM_M = 0.0002


def _distance_to_segment(x: float, z: float, a: tuple[float, float], b: tuple[float, float]) -> float:
    """Distance from a field point to a painted-line segment."""
    dx, dz = b[0] - a[0], b[1] - a[1]
    length_sq = dx * dx + dz * dz
    if length_sq <= 1e-12:
        return math.hypot(x - a[0], z - a[1])
    t = max(0.0, min(1.0, ((x - a[0]) * dx + (z - a[1]) * dz) / length_sq))
    return math.hypot(x - (a[0] + t * dx), z - (a[1] + t * dz))


def _field_line_segments(
    length_m: float, width_m: float
) -> tuple[list[tuple[tuple[float, float], tuple[float, float]]],
           list[tuple[float, float, float]], list[tuple[float, float, float]]]:
    """Return painted line segments and circular markings for the default pitch."""
    half_l, half_w = length_m * 0.5, width_m * 0.5
    segments = [
        ((-half_l, -half_w), (half_l, -half_w)),
        ((half_l, -half_w), (half_l, half_w)),
        ((half_l, half_w), (-half_l, half_w)),
        ((-half_l, half_w), (-half_l, -half_w)),
        ((0.0, -half_w), (0.0, half_w)),
    ]
    circles = [(0.0, 0.0, min(2.0, 0.18 * min(length_m, width_m)))]
    spots = [(0.0, 0.0, 0.07)]
    area_depth = min(3.0, 0.20 * length_m)
    area_width = min(8.0, 0.50 * width_m)
    goal_depth = min(1.0, area_depth * 0.55)
    goal_width = min(3.0, max(2.4, 0.34 * width_m))
    for sign in (-1.0, 1.0):
        end_x = sign * half_l
        area_inner_x = end_x - sign * area_depth
        goal_inner_x = end_x - sign * goal_depth
        for inner_x, half_mark_width in ((area_inner_x, area_width * 0.5),
                                         (goal_inner_x, goal_width * 0.5)):
            segments.extend([
                ((inner_x, -half_mark_width), (inner_x, half_mark_width)),
                ((inner_x, -half_mark_width), (end_x, -half_mark_width)),
                ((inner_x, half_mark_width), (end_x, half_mark_width)),
            ])
        penalty_x = end_x - sign * (area_depth * 0.68)
        spots.append((penalty_x, 0.0, 0.07))
    return segments, circles, spots


def _write_grass_mesh(
    target: Path,
    length_m: float,
    width_m: float,
    border_x_m: float,
    border_y_m: float,
    seed: int,
    albedo_tile_path: Path,
) -> dict[str, int | str]:
    """Write a compact, dense GLB of laid synthetic-turf fibers.

    Coordinates follow glTF's Y-up convention: X/Z are the pitch plane and Y
    is blade height. Painted lines are left clear so the field markings remain
    readable over the flat MuJoCo contact plane. Geometry is streamed in row
    batches so raising density does not require holding the whole field mesh in
    memory at once. Signed-short positions use KHR_mesh_quantization; the
    albedo tile supplies per-vertex color variation without a large index or
    UV buffer.
    """
    rng = np.random.default_rng(seed + 3)
    extent_x = length_m + 2.0 * border_x_m
    extent_z = width_m + 2.0 * border_y_m
    spacing = 1.0 / math.sqrt(GRASS_FIBERS_PER_M2)
    nx = max(1, math.ceil(extent_x / spacing))
    nz = max(1, math.ceil(extent_z / spacing))
    step_x = extent_x / nx
    step_z = extent_z / nz
    segments, circles, spots = _field_line_segments(length_m, width_m)
    clearance = LINE_WIDTH_M * 0.5 + 0.001
    tile = np.asarray(Image.open(albedo_tile_path).convert("RGB"), dtype=np.uint8)
    tile_h, tile_w = tile.shape[:2]
    position_quantum = max(
        POSITION_QUANTUM_M,
        (max(extent_x, extent_z) * 0.5 + 0.03) / 32767.0,
    )
    block_rows = max(1, min(nz, 250_000 // nx))
    x_indices = np.arange(nx, dtype=np.float32)[None, :]
    all_segments = segments
    count = 0
    min_q = np.array([32767, 32767, 32767], dtype=np.int32)
    max_q = np.array([-32768, -32768, -32768], dtype=np.int32)
    target.parent.mkdir(parents=True, exist_ok=True)
    positions_tmp = target.with_name(target.name + ".positions.tmp")
    colors_tmp = target.with_name(target.name + ".colors.tmp")
    output_tmp = target.with_name(target.name + ".tmp")

    try:
        with positions_tmp.open("wb") as position_file, colors_tmp.open("wb") as color_file:
            for row_start in range(0, nz, block_rows):
                rows = np.arange(row_start, min(row_start + block_rows, nz), dtype=np.float32)[:, None]
                jitter_x = rng.uniform(-0.20, 0.20, (len(rows), nx)).astype(np.float32)
                jitter_z = rng.uniform(-0.20, 0.20, (len(rows), nx)).astype(np.float32)
                roots_x = (-extent_x * 0.5 + (x_indices + 0.5 + jitter_x) * step_x).ravel()
                roots_z = np.broadcast_to(
                    -extent_z * 0.5 + (rows + 0.5) * step_z, (len(rows), nx)
                )
                roots_z = (roots_z + jitter_z * step_z).ravel()
                keep = np.ones(roots_x.shape, dtype=bool)

                for a, b in all_segments:
                    ax, az = a
                    dx, dz = b[0] - ax, b[1] - az
                    length_sq = dx * dx + dz * dz
                    t = np.clip(((roots_x - ax) * dx + (roots_z - az) * dz) / max(length_sq, 1e-12), 0.0, 1.0)
                    distance_sq = (roots_x - (ax + t * dx)) ** 2 + (roots_z - (az + t * dz)) ** 2
                    keep &= distance_sq >= clearance * clearance
                for cx, cz, radius in circles:
                    keep &= np.abs(np.hypot(roots_x - cx, roots_z - cz) - radius) >= clearance
                for sx, sz, radius in spots:
                    keep &= np.hypot(roots_x - sx, roots_z - sz) >= radius + clearance

                roots_x = roots_x[keep]
                roots_z = roots_z[keep]
                if roots_x.size == 0:
                    continue

                batch_count = roots_x.size
                yaw = rng.uniform(0.0, math.tau, batch_count)
                direction_x, direction_z = np.cos(yaw), np.sin(yaw)
                across_x, across_z = -direction_z, direction_x
                lean = rng.uniform(0.018, 0.025, batch_count)
                tip_y = rng.uniform(0.005, 0.011, batch_count)
                half_width = rng.uniform(0.0009, 0.0012, batch_count)

                pos = np.empty((batch_count, 3, 3), dtype=np.float32)
                pos[:, 0, 0] = roots_x + across_x * half_width
                pos[:, 0, 1] = 0.0015
                pos[:, 0, 2] = roots_z + across_z * half_width
                pos[:, 1, 0] = roots_x + direction_x * lean
                pos[:, 1, 1] = tip_y
                pos[:, 1, 2] = roots_z + direction_z * lean
                pos[:, 2, 0] = roots_x - across_x * half_width
                pos[:, 2, 1] = 0.0015
                pos[:, 2, 2] = roots_z - across_z * half_width

                # Sample the same photo-like albedo tile used on the field
                # backing, then store glTF-linear vertex colors on the fibers.
                tile_u = np.mod(pos[:, :, 0], TURF_COLOR_TILE_SIZE_M) / TURF_COLOR_TILE_SIZE_M
                tile_v = np.mod(-pos[:, :, 2], TURF_COLOR_TILE_SIZE_M) / TURF_COLOR_TILE_SIZE_M
                px = np.floor(tile_u * tile_w).astype(np.int32) % tile_w
                py = np.floor(tile_v * tile_h).astype(np.int32) % tile_h
                srgb = tile[py, px].astype(np.float32) / 255.0
                linear = np.where(
                    srgb <= 0.04045,
                    srgb / 12.92,
                    np.power((srgb + 0.055) / 1.055, 2.4),
                )
                fiber_tint = rng.uniform(0.93, 1.07, (batch_count, 1, 1)).astype(np.float32)
                rgba = np.empty((batch_count, 3, 4), dtype=np.uint8)
                rgba[:, :, :3] = np.round(np.clip(linear * fiber_tint, 0.0, 1.0) * 255.0).astype(np.uint8)
                rgba[:, :, 3] = 255

                quantized = np.rint(pos / position_quantum).astype("<i2").reshape(-1, 3)
                min_q = np.minimum(min_q, quantized.min(axis=0))
                max_q = np.maximum(max_q, quantized.max(axis=0))
                quantized.tofile(position_file)
                rgba.reshape(-1, 4).tofile(color_file)
                count += batch_count

        if count == 0:
            raise ValueError("field dimensions produced no grass fibers")

        vertex_count = count * 3
        position_bytes = vertex_count * 3 * 2
        color_offset = (position_bytes + 3) & ~3
        color_bytes = vertex_count * 4
        bin_bytes = color_offset + color_bytes
        gltf = {
            "asset": {"version": "2.0", "generator": "URSoccerLab dense synthetic turf"},
            "extensionsUsed": ["KHR_mesh_quantization"],
            "extensionsRequired": ["KHR_mesh_quantization"],
            "scene": 0,
            "scenes": [{"nodes": [0]}],
            "nodes": [{"mesh": 0, "name": "Dense low synthetic turf", "scale": [position_quantum] * 3}],
            "meshes": [{"name": "Dense low synthetic turf", "primitives": [{
                "attributes": {"POSITION": 0, "COLOR_0": 1}, "material": 0, "mode": 4,
            }]}],
            "materials": [{"name": "Dense artificial turf fibers", "doubleSided": True,
                           "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1],
                                                    "metallicFactor": 0, "roughnessFactor": 0.96}}],
            "buffers": [{"byteLength": bin_bytes}],
            "bufferViews": [
                {"buffer": 0, "byteOffset": 0, "byteLength": position_bytes, "target": 34962},
                {"buffer": 0, "byteOffset": color_offset, "byteLength": color_bytes, "target": 34962},
            ],
            "accessors": [
                {"bufferView": 0, "componentType": 5122, "count": vertex_count, "type": "VEC3",
                 "min": min_q.astype(int).tolist(), "max": max_q.astype(int).tolist()},
                {"bufferView": 1, "componentType": 5121, "normalized": True,
                 "count": vertex_count, "type": "VEC4"},
            ],
            "extras": {
                "fieldLengthM": length_m,
                "fieldWidthM": width_m,
                "fibersPerSquareMeter": GRASS_FIBERS_PER_M2,
                "fiberTipHeightRangeM": [0.005, 0.011],
                "fiberLeanLengthRangeM": [0.018, 0.025],
                "fiberRootWidthRangeM": [0.0018, 0.0024],
                "positionQuantizationM": position_quantum,
                "visualExtentXM": extent_x,
                "visualExtentZM": extent_z,
                "sourceAlbedoTile": str(albedo_tile_path),
                "units": "meters; glTF Y-up",
            },
        }
        json_chunk = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
        json_chunk += b" " * ((4 - len(json_chunk) % 4) % 4)
        total_length = 12 + 8 + len(json_chunk) + 8 + bin_bytes
        with output_tmp.open("wb") as output_file:
            output_file.write(struct.pack("<4sII", b"glTF", 2, total_length))
            output_file.write(struct.pack("<I4s", len(json_chunk), b"JSON"))
            output_file.write(json_chunk)
            output_file.write(struct.pack("<I4s", bin_bytes, b"BIN\0"))
            with positions_tmp.open("rb") as position_file:
                while chunk := position_file.read(8 * 1024 * 1024):
                    output_file.write(chunk)
            output_file.write(b"\0" * (color_offset - position_bytes))
            with colors_tmp.open("rb") as color_file:
                while chunk := color_file.read(8 * 1024 * 1024):
                    output_file.write(chunk)
        os.replace(output_tmp, target)
        return {
            "vertices": vertex_count,
            "triangles": count,
            "fibers": count,
            "bytes": target.stat().st_size,
        }
    finally:
        for temporary in (positions_tmp, colors_tmp, output_tmp):
            if temporary.exists():
                temporary.unlink()


def _periodic_noise(size: int, rng: np.random.Generator, exponent: float) -> np.ndarray:
    """Create deterministic, seamless fractal noise with an FFT filter."""
    white = rng.standard_normal((size, size))
    fy = np.fft.fftfreq(size)[:, None]
    fx = np.fft.fftfreq(size)[None, :]
    radius = np.sqrt(fx * fx + fy * fy)
    radius[0, 0] = 1.0
    spectrum = np.fft.fft2(white) / np.power(radius, exponent)
    spectrum[0, 0] = 0.0
    noise = np.fft.ifft2(spectrum).real
    noise -= noise.mean()
    noise /= max(float(noise.std()), 1e-8)
    return noise.astype(np.float32)


def _blade_tile(
    size: int,
    rng: np.random.Generator,
    tile_size_m: float = DETAIL_TILE_SIZE_M,
    fibers_per_m2: int = TURF_FIBERS_PER_M2,
) -> np.ndarray:
    """Draw densely packed, antialiased synthetic-turf fibers into a seamless tile."""
    scale = 2
    canvas = Image.new("L", (size * scale * 3, size * scale * 3), 128)
    draw = ImageDraw.Draw(canvas)
    blade_count = round(tile_size_m * tile_size_m * fibers_per_m2)
    pixels_per_m = size / tile_size_m
    for _ in range(blade_count):
        x = int(rng.integers(0, size))
        y = int(rng.integers(0, size))
        length = float(rng.uniform(0.010, 0.026)) * pixels_per_m
        angle = float(rng.vonmises(0.0, 1.2))
        bend = float(rng.uniform(-0.9, 0.9))
        dx = length * np.cos(angle)
        dy = length * np.sin(angle)
        color = int(np.clip(rng.normal(129, 30), 54, 204))
        width = max(1, round(float(rng.uniform(0.00045, 0.00095)) * pixels_per_m * scale))
        points_m = [
            (x, y),
            (x + dx * 0.55 - dy * bend * 0.4,
             y + dy * 0.55 + dx * bend * 0.4),
            (x + dx - dy * bend, y + dy + dx * bend),
        ]
        min_x = min(point[0] for point in points_m)
        max_x = max(point[0] for point in points_m)
        min_y = min(point[1] for point in points_m)
        max_y = max(point[1] for point in points_m)
        pad = width / scale + 2.0
        x_offsets = [0]
        y_offsets = [0]
        if min_x < pad:
            x_offsets.append(size)
        if max_x > size - pad:
            x_offsets.append(-size)
        if min_y < pad:
            y_offsets.append(size)
        if max_y > size - pad:
            y_offsets.append(-size)
        for offset_x in x_offsets:
            for offset_y in y_offsets:
                points = [
                    (round((px + size + offset_x) * scale),
                     round((py + size + offset_y) * scale))
                    for px, py in points_m
                ]
                draw.line(points, fill=color, width=width, joint="curve")

    center = canvas.crop((size * scale, size * scale,
                          size * scale * 2, size * scale * 2))
    resampling = getattr(Image, "Resampling", Image)
    center = center.resize((size, size), resampling.LANCZOS)
    return np.asarray(center, dtype=np.float32) / 255.0


def _write_detail_maps(out_dir: Path, seed: int) -> dict[str, str]:
    size = 1024
    rng = np.random.default_rng(seed)
    blades = _blade_tile(size, rng, DETAIL_TILE_SIZE_M)
    broad = _periodic_noise(size, rng, 1.35)
    fine = _periodic_noise(size, rng, 0.40)

    height = np.clip(0.50 + 0.48 * (blades - 0.5) + 0.022 * broad + 0.010 * fine,
                     0.0, 1.0)
    dh_dx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 0.5
    dh_dy = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * 0.5

    # DirectX tangent-space normal convention (the runtime default).
    nx = -dh_dx * 5.0
    ny = dh_dy * 5.0
    nz = np.ones_like(nx)
    norm = np.sqrt(nx * nx + ny * ny + nz * nz)
    normal = np.stack((nx / norm, ny / norm, nz / norm), axis=2)
    normal_rgb = np.clip((normal * 0.5 + 0.5) * 255.0, 0, 255).astype(np.uint8)

    rough_noise = 0.035 * broad + 0.020 * fine + 0.045 * (blades - 0.5)
    roughness = np.clip(0.88 + rough_noise, 0.74, 0.99)
    ao_noise = 0.015 * broad + 0.010 * fine
    ao = np.clip(0.98 - 0.28 * np.clip(0.50 - blades, 0.0, 0.35) + ao_noise, 0.78, 1.0)

    target = out_dir / DETAIL_DIR
    target.mkdir(parents=True, exist_ok=True)
    files = {
        "normal_map": "grass1-normal1-dx.png",
        "roughness_map": "grass1-rough.png",
        "ao_map": "grass1-ao.png",
        "height_source": "grass1-height.png",
    }
    Image.fromarray(normal_rgb, "RGB").save(target / files["normal_map"], optimize=True)
    Image.fromarray(np.round(roughness * 255).astype(np.uint8), "L").save(
        target / files["roughness_map"], optimize=True
    )
    Image.fromarray(np.round(ao * 255).astype(np.uint8), "L").save(
        target / files["ao_map"], optimize=True
    )
    Image.fromarray(np.round(height * 255).astype(np.uint8), "L").save(
        target / files["height_source"], optimize=True
    )
    return {key: f"{DETAIL_DIR}/{filename}" for key, filename in files.items()}


def _field_base_color(
    length_m: float,
    width_m: float,
    border_x_m: float,
    border_y_m: float,
    pixels_per_m: int,
    seed: int,
    albedo_tile_path: Path,
) -> Image.Image:
    extent_x = length_m + 2.0 * border_x_m
    extent_y = width_m + 2.0 * border_y_m
    image_width = max(64, round(extent_x * pixels_per_m))
    image_height = max(64, round(extent_y * pixels_per_m))

    rng = np.random.default_rng(seed + 1)
    xs = (np.arange(image_width, dtype=np.float32) + 0.5) / image_width * extent_x
    ys = extent_y * 0.5 - (np.arange(image_height, dtype=np.float32) + 0.5) / image_height * extent_y
    world_x = xs - extent_x * 0.5
    world_y = ys
    inside_x = np.abs(world_x) <= length_m * 0.5
    inside_y = np.abs(world_y) <= width_m * 0.5
    pitch_mask = inside_y[:, None] & inside_x[None, :]

    # Short, dark green apron around the playable rectangle, with subtle turf
    # variation across the whole image so the transition is not a hard slab.
    tile_pixels = max(256, round(TURF_COLOR_TILE_SIZE_M * pixels_per_m))
    source_tile = Image.open(albedo_tile_path).convert("RGB")
    resampling = getattr(Image, "Resampling", Image)
    source_tile = source_tile.resize((tile_pixels, tile_pixels), resampling.LANCZOS)
    tile_rgb = np.asarray(source_tile, dtype=np.uint8)
    tiled = tile_rgb[
        np.arange(image_height)[:, None] % tile_pixels,
        np.arange(image_width)[None, :] % tile_pixels,
    ]

    # Gentle field-scale mottling, fine fiber variation and restrained mowing
    # bands reproduce a dense indoor synthetic pitch without a flat green slab.
    mottle = _periodic_noise(192, rng, 1.35)
    resampling = getattr(Image, "Resampling", Image)
    mottle = np.asarray(Image.fromarray(mottle, mode="F").resize(
        (image_width, image_height), resampling.BICUBIC), dtype=np.float32)
    fine_noise = rng.normal(0.0, 1.4, (image_height, image_width)).astype(np.float32)
    stripe_index = np.floor((world_y + width_m * 0.5) / 1.6).astype(np.int32)
    stripes = np.where(stripe_index % 2 == 0, 1.012, 0.988).astype(np.float32)
    broad_delta = 3.0 * mottle + fine_noise
    shade = np.where(pitch_mask, 1.0, 0.94).astype(np.float32)
    rgb = np.clip(
        (tiled.astype(np.float32) + broad_delta[:, :, None])
        * stripes[:, None, None]
        * shade[:, :, None],
        0,
        255,
    ).astype(np.uint8)
    base = Image.fromarray(rgb, "RGB")
    draw = ImageDraw.Draw(base)

    def pixel_x(x_m: float) -> int:
        return round((x_m + extent_x * 0.5) / extent_x * (image_width - 1))

    def pixel_y(y_m: float) -> int:
        return round((extent_y * 0.5 - y_m) / extent_y * (image_height - 1))

    def point(x_m: float, y_m: float) -> tuple[int, int]:
        return pixel_x(x_m), pixel_y(y_m)

    line_px = max(2, round(LINE_WIDTH_M * pixels_per_m))
    line_color = (224, 231, 218)
    half_l = length_m * 0.5
    half_w = width_m * 0.5
    left, right = pixel_x(-half_l), pixel_x(half_l)
    top, bottom = pixel_y(half_w), pixel_y(-half_w)

    # Touchlines, halfway line, and center circle.
    draw.rectangle((left, top, right, bottom), outline=line_color, width=line_px)
    draw.line((pixel_x(0.0), top, pixel_x(0.0), bottom), fill=line_color, width=line_px)
    center_radius = min(2.0, 0.18 * min(length_m, width_m))
    cx, cy = point(0.0, 0.0)
    rx = max(1, round(center_radius / extent_x * image_width))
    ry = max(1, round(center_radius / extent_y * image_height))
    draw.ellipse((cx - rx, cy - ry, cx + rx, cy + ry), outline=line_color, width=line_px)
    spot_radius = max(2, round(0.07 * pixels_per_m))
    draw.ellipse((cx - spot_radius, cy - spot_radius, cx + spot_radius, cy + spot_radius),
                 fill=line_color)

    # Symmetric penalty and goal areas, scaled conservatively for small fields.
    area_depth = min(3.0, 0.20 * length_m)
    area_width = min(8.0, 0.50 * width_m)
    goal_area_depth = min(1.0, area_depth * 0.55)
    goal_area_width = min(3.0, max(2.4, 0.34 * width_m))
    for sign in (-1.0, 1.0):
        end_x = sign * half_l
        inner_x = end_x - sign * area_depth
        y0, y1 = -area_width * 0.5, area_width * 0.5
        p0, p1 = point(end_x, y0), point(inner_x, y1)
        draw.rectangle((p0[0], p0[1], p1[0], p1[1]), outline=line_color, width=line_px)

        goal_inner_x = end_x - sign * goal_area_depth
        gy0, gy1 = -goal_area_width * 0.5, goal_area_width * 0.5
        g0, g1 = point(end_x, gy0), point(goal_inner_x, gy1)
        draw.rectangle((g0[0], g0[1], g1[0], g1[1]), outline=line_color, width=line_px)

        penalty_x = end_x - sign * (area_depth * 0.68)
        px, py = point(penalty_x, 0.0)
        draw.ellipse((px - spot_radius, py - spot_radius, px + spot_radius, py + spot_radius),
                     fill=line_color)

    # Keep painted lines bright while retaining a hint of fiber and pigment
    # variation, as with white synthetic turf line inserts.
    pixels = np.asarray(base, dtype=np.uint8).copy()
    line_mask = pixels[:, :, 0] > 185
    if np.any(line_mask):
        mark_noise = rng.normal(0.0, 2.0, pixels.shape[:2]).astype(np.float32)
        for channel, weight in enumerate((0.9, 1.0, 0.85)):
            values = pixels[:, :, channel].astype(np.float32)
            values[line_mask] += mark_noise[line_mask] * weight
            pixels[:, :, channel] = np.clip(values, 0, 255).astype(np.uint8)
    return Image.fromarray(pixels, "RGB")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--length-m", type=float, default=9.0)
    parser.add_argument("--width-m", type=float, default=6.0)
    parser.add_argument("--border-x-m", type=float, default=0.8)
    parser.add_argument("--border-y-m", type=float, default=0.9)
    parser.add_argument("--pixels-per-m", type=int, default=512)
    parser.add_argument("--seed", type=int, default=20261007)
    parser.add_argument("--turf-albedo-tile", type=Path, default=DEFAULT_TURF_ALBEDO_TILE,
                        help="seamless photo-like synthetic-turf color tile")
    parser.add_argument("--grass-mesh-out", type=Path, default=DEFAULT_GRASS_MESH_OUT,
                        help="tracked 3D GLB destination (default: Assets/Scenes/SoccerField/visual/grass_blades.glb)")
    args = parser.parse_args()

    for name in ("length_m", "width_m", "pixels_per_m"):
        if getattr(args, name) <= 0:
            parser.error(f"--{name.replace('_', '-')} must be positive")
    for name in ("border_x_m", "border_y_m"):
        if getattr(args, name) < 0:
            parser.error(f"--{name.replace('_', '-')} cannot be negative")

    max_extent = max(args.length_m + 2 * args.border_x_m,
                     args.width_m + 2 * args.border_y_m)
    if max_extent * args.pixels_per_m > MAX_TEXTURE_DIMENSION:
        parser.error(
            f"requested base map exceeds {MAX_TEXTURE_DIMENSION}px on an axis; "
            "reduce --pixels-per-m or the field dimensions"
        )

    out_dir = args.out_dir.expanduser().resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    albedo_tile_path = args.turf_albedo_tile.expanduser().resolve()
    if not albedo_tile_path.is_file():
        parser.error(f"--turf-albedo-tile does not exist: {albedo_tile_path}")
    pitch = _field_base_color(
        args.length_m, args.width_m, args.border_x_m, args.border_y_m,
        args.pixels_per_m, args.seed, albedo_tile_path,
    )
    pitch_path = out_dir / "example.png"
    pitch.save(pitch_path, optimize=True)
    detail_paths = _write_detail_maps(out_dir, args.seed)
    grass_mesh_path = args.grass_mesh_out.expanduser().resolve()
    grass_mesh = _write_grass_mesh(grass_mesh_path, args.length_m, args.width_m,
                                   args.border_x_m, args.border_y_m, args.seed,
                                   albedo_tile_path)

    manifest = {
        "field_length_m": args.length_m,
        "field_width_m": args.width_m,
        "border_x_m": args.border_x_m,
        "border_y_m": args.border_y_m,
        "base_color_map": "example.png",
        "turf_albedo_tile": str(albedo_tile_path.relative_to(ROOT))
        if albedo_tile_path.is_relative_to(ROOT) else str(albedo_tile_path),
        "detail_tile_size_m": DETAIL_TILE_SIZE_M,
        "line_width_m": LINE_WIDTH_M,
        "turf_fibers_per_m2": TURF_FIBERS_PER_M2,
        "grass_fibers_per_m2": GRASS_FIBERS_PER_M2,
        "fiber_tip_height_range_m": [0.005, 0.011],
        "fiber_lean_length_range_m": [0.018, 0.025],
        "fiber_root_width_range_m": [0.0018, 0.0024],
        "pixels_per_m": args.pixels_per_m,
        "seed": args.seed,
        **detail_paths,
        "grass_mesh": str(grass_mesh_path.relative_to(ROOT)) if grass_mesh_path.is_relative_to(ROOT) else str(grass_mesh_path),
        "grass_mesh_vertices": grass_mesh["vertices"],
        "grass_mesh_triangles": grass_mesh["triangles"],
        "grass_mesh_bytes": grass_mesh["bytes"],
        "grass_mesh_fibers": grass_mesh["fibers"],
        "grass_mesh_note": "Dense, low 3D synthetic-turf fibers for rendering; collision remains the flat MuJoCo plane.",
        "height_source_note": "Authoring aid only; URSoccerLab runtime does not load height maps.",
        "physics_note": "Flat MuJoCo plane; configure contact friction in scene.json.",
    }
    (out_dir / "generated_manifest.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )

    print(f"Wrote {pitch_path} ({pitch.width}x{pitch.height})")
    for key, relative_path in detail_paths.items():
        print(f"Wrote {out_dir / relative_path} [{key}]")
    print(f"Wrote {grass_mesh_path} ({grass_mesh['triangles']} triangles)")
    print(f"Wrote {out_dir / 'generated_manifest.json'}")


if __name__ == "__main__":
    main()
