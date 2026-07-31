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
    ("Blueprint", "Content/URSoccerLab/Robots/pi_plus/pi_plus.uasset"),
    ("Soccer ball", "Content/URSoccerLab/Objects/soccer_ball/soccer_ball.uasset"),
    ("Level", "Content/Levels/URS_SoccerField.umap"),
    ("Field mesh", "Content/URSoccerLab/Scenes/SoccerField/Field/StaticMeshes/Plane.uasset"),
    ("Environment mesh", "Content/URSoccerLab/Scenes/SoccerField/Environment/StaticMeshes/Material2.uasset"),
    ("Cloud material", "Content/URSoccerLab/Scenes/SoccerField/Lighting/MI_URS_OvercastCloud.uasset"),
    ("Field physics", "Content/URSoccerLab/Scenes/SoccerField/Physics/field_physics.uasset"),
]

EXPECTED_UE_PATHS = [
    "/Game/URSoccerLab/Robots/pi_plus/pi_plus.pi_plus",
    "/Game/URSoccerLab/Objects/soccer_ball/soccer_ball.soccer_ball",
    "/Game/Levels/URS_SoccerField",
    "/Game/URSoccerLab/Scenes/SoccerField/Physics/field_physics.field_physics",
]

ROBOT_SOURCE = ROOT / "Assets/Robots/pi_plus/pi_plus.xml"
BALL_SOURCE = ROOT / "Assets/Objects/soccer_ball/soccer_ball.xml"


def check_files() -> list[str]:
    errors = []
    for label, rel_path in EXPECTED_ASSETS:
        full = ROOT / rel_path
        if not full.exists():
            errors.append(f"{label}: file not found at {rel_path}")
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


def check_robot_source() -> list[str]:
    errors = []
    if not ROBOT_SOURCE.exists():
        return [f"Robot MJCF: file not found at {ROBOT_SOURCE.relative_to(ROOT)}"]

    robot_dir = ROBOT_SOURCE.parent
    mesh_dir = robot_dir / "meshes"
    root = ET.parse(ROBOT_SOURCE).getroot()
    visual_names = []
    for frame in root.iter("frame"):
        name = frame.get("name", "")
        if not name.startswith("visual__"):
            continue
        visual_name = name.removeprefix("visual__")
        visual_names.append(visual_name)
        if list(frame):
            errors.append(f"Visual frame must be empty: {name}")
        glb = mesh_dir / f"{visual_name}.glb"
        if not glb.is_file():
            errors.append(
                f"Visual frame {name}: file not found at {glb.relative_to(ROOT)}"
            )

    if not visual_names:
        errors.append("Robot MJCF contains no visual__ frames")
    if len(visual_names) != len(set(visual_names)):
        errors.append("Robot MJCF contains duplicate visual__ frame names")

    expected_glbs = {f"{name}.glb" for name in visual_names}
    actual_glbs = {path.name for path in mesh_dir.glob("*.glb")}
    for extra in sorted(actual_glbs - expected_glbs):
        errors.append(f"Unreferenced robot visual: {(mesh_dir / extra).relative_to(ROOT)}")

    if list(robot_dir.glob("*_ue.xml")):
        errors.append("Generated *_ue.xml files are not allowed in robot sources")
    if list(robot_dir.glob("*.urdf")):
        errors.append("URDF files are not allowed in robot sources")
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
    errors = check_files() + check_robot_source() + check_ball_source() + check_ue_assets()

    if errors:
        for e in errors:
            print(f"  FAIL: {e}", file=sys.stderr)
        return 1

    print("All baked assets validated successfully.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
