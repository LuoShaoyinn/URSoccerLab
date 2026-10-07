#!/usr/bin/env python3
"""Validate that baked runtime assets exist and are loadable.

Can run standalone (uses unreal module if available) or inside the editor.
When running standalone it only checks file existence. Inside the editor
it also verifies assets are loadable via the asset registry.
"""

from __future__ import annotations

import sys
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]

EXPECTED_ASSETS = [
    ("Soccer ball", "Content/URSoccerLab/Objects/soccer_ball/soccer_ball.uasset"),
    ("Level", "Content/Levels/URS_SoccerField.umap"),
    ("Runtime field mesh", "Content/URSoccerLab/Scenes/SoccerField/Runtime/SM_RuntimeField.uasset"),
    ("Runtime field material", "Content/URSoccerLab/Scenes/SoccerField/Runtime/MI_RuntimeField.uasset"),
    ("Environment mesh", "Content/URSoccerLab/Scenes/SoccerField/Environment/StaticMeshes/Material2.uasset"),
    ("Field physics", "Content/URSoccerLab/Scenes/SoccerField/Physics/field_physics.uasset"),
]

EXPECTED_UE_PATHS = [
    "/Game/URSoccerLab/Objects/soccer_ball/soccer_ball.soccer_ball",
    "/Game/Levels/URS_SoccerField",
    "/Game/URSoccerLab/Scenes/SoccerField/Runtime/MI_RuntimeField",
    "/Game/URSoccerLab/Scenes/SoccerField/Runtime/SM_RuntimeField",
    "/Game/URSoccerLab/Scenes/SoccerField/Physics/field_physics.field_physics",
]

BALL_SOURCE = ROOT / "Assets/Objects/soccer_ball/soccer_ball.xml"


def check_files() -> list[str]:
    errors = []
    for label, rel_path in EXPECTED_ASSETS:
        full = ROOT / rel_path
        if not full.exists():
            errors.append(f"{label}: file not found at {rel_path}")
    if (ROOT / "Content/URSoccerLab/Robots").exists():
        errors.append("Robot assets must remain external, outside cooked Content")
    staging_dir = ROOT / "Content/MuJoCoImports"
    if staging_dir.exists():
        errors.append(
            f"Temporary Unreal import directory remains: {staging_dir.relative_to(ROOT)}"
        )
    for legacy_dir in (
        ROOT / "Content/URSoccerLab/Environment",
        ROOT / "Content/URSoccerLab/Scenes/SoccerField/field",
        ROOT
        / "Content/URSoccerLab/Scenes/SoccerField"
        / "ege_carpets_canvas_collage_octo_blue_in_situ_vr",
        ROOT / "Assets/Scenes/SoccerField/source",
    ):
        if legacy_dir.exists():
            errors.append(f"Legacy scene directory remains: {legacy_dir.relative_to(ROOT)}")
    return errors


def check_ball_source() -> list[str]:
    errors = []
    if not BALL_SOURCE.is_file():
        return [f"Ball MJCF: file not found at {BALL_SOURCE.relative_to(ROOT)}"]
    root = ET.parse(BALL_SOURCE).getroot()
    frames = [
        frame.get("name", "") for frame in root.iter("frame")
        if frame.get("name", "").startswith("visual__")
    ]
    if frames != ["visual__soccer_ball"]:
        errors.append(f"Ball MJCF has unexpected visual frames: {frames}")
    visual = BALL_SOURCE.parent / "meshes/soccer_ball.glb"
    if not visual.is_file():
        errors.append(f"Ball visual: file not found at {visual.relative_to(ROOT)}")
    geoms = [geom for geom in root.iter("geom") if geom.get("name") == "ball"]
    if len(geoms) != 1 or geoms[0].get("type") != "sphere" or geoms[0].get("size") != "0.075":
        errors.append("Ball collision must be one sphere with radius 0.075 m")
    return errors


def check_ue_assets() -> list[str]:
    try:
        import unreal
    except ImportError:
        return []  # Not in editor, skip UE checks

    errors = []
    for path in EXPECTED_UE_PATHS:
        if not unreal.EditorAssetLibrary.does_asset_exist(path):
            errors.append(f"UE asset not found: {path}")
    return errors


def main() -> int:
    errors = check_files() + check_ball_source() + check_ue_assets()

    if errors:
        for e in errors:
            print(f"  FAIL: {e}", file=sys.stderr)
        return 1

    print("All baked assets validated successfully.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
