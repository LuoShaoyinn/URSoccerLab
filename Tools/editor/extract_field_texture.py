#!/usr/bin/env python3
"""Export a baked UTexture2D to a standalone PNG image under refs/.

The default target is the soccer-field pitch texture baked into the production
level. The exported PNG is intended as a human/ML reference asset (mirroring
the refs/<name>/ PBR layout), not as a source asset that feeds the build.

Run inside Unreal Editor:

    UnrealEditor-Cmd URSoccerLab.uproject \
      -ExecutePythonScript="$PWD/Tools/editor/extract_field_texture.py" \
      -NullRHI -unattended -nop4 -nosplash

Override the source asset or destination with:

    -ExecutePythonScript=...extract_field_texture.py \
      -Arg="--asset=/Game/.../TextureName" -Arg="--out=refs/foo.png"

If the editor build does not forward `-Arg` tokens to sys.argv, call the tool's
`main()` from a small wrapper that sets `sys.argv` explicitly (the production
project's URSoccerLab module is not required to export a UTexture2D).

PNG encoding uses only the Python standard library (zlib + struct), so it does
not depend on Pillow being present in the editor's bundled interpreter. The
texture is first exported through UE's AssetExportTask as a TGA (the only
stock UTexture2D exporter), then converted in-process.
"""

from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path

import unreal


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_ASSET = "/Game/URSoccerLab/Scenes/SoccerField/Field/Textures/field"
DEFAULT_OUT = ROOT / "refs" / "field.png"


def _write_png(path: Path, width: int, height: int, color_type: int,
               rows: list[bytes]) -> None:
    def chunk(tag: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload))
            + tag
            + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
        )

    raw = bytearray()
    for row in rows:
        raw.append(0)
        raw.extend(row)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8,
                                            color_type, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(chunk(b"IEND", b""))


def _tga_to_png(tga_path: Path, png_path: Path) -> tuple[int, int]:
    data = tga_path.read_bytes()
    if len(data) < 18:
        raise ValueError("truncated TGA header")

    id_len = data[0]
    cmap_type = data[1]
    image_type = data[2]
    width = int.from_bytes(data[12:14], "little")
    height = int.from_bytes(data[14:16], "little")
    bpp = data[16]
    descriptor = data[17]
    top_to_bottom = bool(descriptor & 0x20)

    if cmap_type != 0:
        raise ValueError(f"colormap TGA not supported (color_map_type={cmap_type})")
    if image_type not in (2, 10):
        raise ValueError(f"unsupported TGA image_type={image_type}")
    if bpp not in (24, 32):
        raise ValueError(f"unsupported TGA pixel depth={bpp}bpp")

    channels = 3 if bpp == 24 else 4
    bytes_per_pixel = bpp // 8
    row_bytes = width * bytes_per_pixel
    pixel_bytes = width * height * bytes_per_pixel

    pos = 18 + id_len
    if image_type == 2:
        raw = data[pos:pos + pixel_bytes]
        if len(raw) != pixel_bytes:
            raise ValueError("truncated uncompressed TGA pixel data")
    else:
        raw = bytearray()
        while len(raw) < pixel_bytes and pos < len(data):
            packet = data[pos]
            pos += 1
            count = (packet & 0x7F) + 1
            if packet & 0x80:
                if pos + bytes_per_pixel > len(data):
                    raise ValueError("truncated RLE run packet")
                pixel = data[pos:pos + bytes_per_pixel]
                pos += bytes_per_pixel
                raw.extend(pixel * count)
            else:
                n = count * bytes_per_pixel
                if pos + n > len(data):
                    raise ValueError("truncated RLE raw packet")
                raw.extend(data[pos:pos + n])
                pos += n
        if len(raw) != pixel_bytes:
            raise ValueError(
                f"RLE decode size mismatch: got {len(raw)}, expected {pixel_bytes}"
            )

    rows: list[bytes] = []
    for y in range(height):
        src_y = y if top_to_bottom else (height - 1 - y)
        start = src_y * row_bytes
        src = raw[start:start + row_bytes]
        out = bytearray(width * channels)
        if channels == 3:
            out[0::3] = src[2::3]
            out[1::3] = src[1::3]
            out[2::3] = src[0::3]
        else:
            out[0::4] = src[2::4]
            out[1::4] = src[1::4]
            out[2::4] = src[0::4]
            out[3::4] = src[3::4]
        rows.append(bytes(out))

    color_type = 2 if channels == 3 else 6
    _write_png(png_path, width, height, color_type, rows)
    return width, height


def _export_texture_tga(texture: unreal.Texture2D, tga_path: Path) -> None:
    task = unreal.AssetExportTask()
    task.set_editor_properties({
        "automated": True,
        "prompt": False,
        "filename": str(tga_path),
        "object": texture,
        "replace_identical": False,
    })
    if not unreal.Exporter.run_asset_export_task(task):
        raise RuntimeError(
            f"AssetExportTask failed for {tga_path}; the texture may have no "
            "loadable pixel data (missing .uexp/.ubulk companion or empty source)"
        )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--asset", default=DEFAULT_ASSET,
                        help="UE asset path of the UTexture2D to export")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT,
                        help="destination PNG path")
    args, _ = parser.parse_known_args()

    texture = unreal.load_asset(args.asset)
    if not isinstance(texture, unreal.Texture2D):
        asset_class = type(texture).__name__ if texture else "None"
        raise RuntimeError(
            f"{args.asset} is not a Texture2D (got {asset_class}); "
            "this tool exports baked UTexture2D assets only"
        )

    unreal.log(f"[extract_field_texture] asset={args.asset} class={type(texture).__name__}")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    tga_path = args.out.with_suffix(".tga")
    try:
        _export_texture_tga(texture, tga_path)
        if not tga_path.is_file():
            raise RuntimeError(f"TGA export produced no file at {tga_path}")
        png_width, png_height = _tga_to_png(tga_path, args.out)
    finally:
        if tga_path.is_file():
            tga_path.unlink()

    size = args.out.stat().st_size
    unreal.log(
        f"[extract_field_texture] done: {args.out} "
        f"({png_width}x{png_height}, {size} bytes)"
    )


if __name__ == "__main__":
    main()
