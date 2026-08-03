#!/usr/bin/env python3
"""Prepare the soccer-field pitch texture for the production plane.

Source: a generated top-down pitch image placed in ``refs/`` (gitignored -- it
is a large external asset). Output: ``refs/field_fixed.png``, which is then
re-imported into the baked texture
``Content/URSoccerLab/Scenes/SoccerField/Field/Textures/field`` (in place, so
every reference to ``field`` picks it up).

Field-alignment contract (the "note" -- these are what make the pitch line up
with the geometry in ``URS_SoccerField.umap``):

  * The ground plane mesh ``Field/StaticMeshes/Plane`` is 1060 x 780 uu and is
    centered at the world origin, so texture UV [0,1] maps to plane
    X in [-530, +530], Y in [-390, +390].
  * The goal actors (``URS_SoccerField_*_goal_*``) sit at X = +/-450 uu, so the
    goal lines must land at texture U = 0.0755 and U = 0.9245.
  * The plane's aspect is 1060/780 = 1.3590; the source image must be cropped to
    that aspect so a 1:1 UV map has square texels and the pitch is not stretched.
  * The pitch must be top-bottom mirror symmetric (one half is drawn heavier in
    the source, ~5.5% mirror asymmetry, which reads as "skewed"). We V-center
    the pitch so the center circle lies on the mirror axis, then mirror the top
    half to the bottom -- crisp, no averaging, so no ghost lines.
  * ``U_SHIFT`` is an optional extra perceptual shift (normalized U) for final
    eyeball tuning; 0 by default.

Pipeline:
  1. crop to the plane aspect (square texels);
  2. horizontal affine mapping the markings bbox onto U[0.0755, 0.9245];
  3. V-center + top-to-bottom mirror (force T-B symmetry);
  4. optional U_SHIFT.

Run (no Unreal needed; uses numpy + Pillow, e.g. from the py_example venv):

    py_example/.venv/bin/python Tools/editor/fix_pitch_image.py

Then re-import the result into the texture with UnrealEditor, e.g.:

    UnrealEditor-Cmd URSoccerLab.uproject -NullRHI -unattended -nop4 -nosplash \\
      -ExecutePythonScript=<a small AssetImportTask that reimports
      refs/field_fixed.png into /Game/URSoccerLab/Scenes/SoccerField/Field/
      Textures/field with replace_existing=True>
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "refs" / "ChatGPT Image Aug 3, 2026, 01_03_38 AM(1).png"
OUT = ROOT / "refs" / "field_fixed.png"

# Goal-line U on the plane (goals at X = +/-450 uu, plane half-width 530 uu).
MX0_TGT, MX1_TGT = 0.0755, 0.9245
# Plane geometry (uu). The texture is cropped to this aspect so a 1:1 UV map
# has square texels and the pitch is not stretched.
PLANE_W, PLANE_H = 1060.0, 780.0
PLANE_ASPECT = PLANE_W / PLANE_H
# Extra perceptual shift of the whole pitch (normalized U). +/- = toward +/-X.
U_SHIFT = 0.0


def main() -> None:
    if not SRC.is_file():
        raise FileNotFoundError(f"place the source pitch image at {SRC}")
    img = Image.open(SRC).convert("RGB")
    w, h = img.size

    # 1. Crop to the plane aspect so 1:1 UV mapping is undistorted.
    if abs(w / h - PLANE_ASPECT) > 1e-4:
        if w / h < PLANE_ASPECT:
            new_h = int(round(w / PLANE_ASPECT))
            top = (h - new_h) // 2
            img = img.crop((0, top, w, top + new_h))
        else:
            new_w = int(round(h * PLANE_ASPECT))
            left = (w - new_w) // 2
            img = img.crop((left, 0, left + new_w, h))
        w, h = img.size
        print(f"cropped to plane aspect {PLANE_ASPECT:.4f} -> {w}x{h}")

    gray = np.asarray(img.convert("L")).astype(float)
    markings = gray > 180

    # 2. Horizontal affine: map the source markings bbox onto the goal-line U.
    xs = np.where(markings.any(axis=0))[0]
    mx0_src = xs.min() / w
    mx1_src = xs.max() / w
    s_u = (mx1_src - mx0_src) / (MX1_TGT - MX0_TGT)
    c_u = (mx0_src - s_u * MX0_TGT) * w  # px; x_in = s_u * x_out + c_u
    if c_u < 0 or s_u * w + c_u > w:
        raise RuntimeError(
            f"U affine would sample out of bounds for width {w}; "
            "refusing to pad with non-grass"
        )
    aligned = img.transform(
        (w, h), Image.AFFINE, (s_u, 0.0, c_u, 0.0, 1.0, 0.0),
        resample=Image.BICUBIC,
    )

    # 3. Force T-B symmetry: V-center, then mirror the top half to the bottom.
    arr = np.asarray(aligned)
    ys_rows = np.where(markings.any(axis=1))[0]
    vc = (ys_rows.min() + ys_rows.max()) / 2.0
    shift_v = int(round(h / 2.0 - vc))
    centered = np.empty_like(arr)
    if shift_v >= 0:
        centered[shift_v:] = arr[:h - shift_v]
        if shift_v:
            centered[:shift_v] = arr[shift_v:2 * shift_v][::-1]
    else:
        sv = -shift_v
        centered[:h - sv] = arr[sv:]
        if sv:
            centered[h - sv:] = arr[h - 2 * sv:h - sv][::-1]
    top = centered[:h // 2, :, :]
    aligned = Image.fromarray(np.concatenate([top, np.flipud(top)], axis=0)[:h, :, :])

    # 4. Optional perceptual shift; exposed edge is mirror-filled with grass.
    shift_px = int(round(U_SHIFT * w))
    if shift_px != 0:
        arr = np.asarray(aligned)
        shifted = np.empty_like(arr)
        if shift_px > 0:
            shifted[:, shift_px:] = arr[:, :w - shift_px]
            shifted[:, :shift_px] = arr[:, shift_px:2 * shift_px][:, ::-1]
        else:
            sp = -shift_px
            shifted[:, :w - sp] = arr[:, sp:]
            shifted[:, w - sp:] = arr[:, w - 2 * sp:w - sp][:, ::-1]
        aligned = Image.fromarray(shifted)

    OUT.parent.mkdir(parents=True, exist_ok=True)
    aligned.save(OUT)
    print(f"wrote {OUT} ({w}x{h}, aspect {w/h:.4f}); goal lines "
          f"U[{MX0_TGT},{MX1_TGT}]; U_SHIFT={U_SHIFT}")


if __name__ == "__main__":
    main()
