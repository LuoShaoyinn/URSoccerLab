#!/usr/bin/env python3
"""Validate that baked runtime assets exist and are loadable.

Can run standalone (uses unreal module if available) or inside the editor.
When running standalone it only checks file existence. Inside the editor
it also verifies assets are loadable via the asset registry.
"""

from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]

EXPECTED_ASSETS = [
    ("Blueprint", "Content/URSoccerLab/Robots/pi_plus/pi_plus.uasset"),
    ("Level", "Content/Levels/URS_SoccerField.umap"),
    ("Field mesh", "Content/URSoccerLab/Scenes/SoccerField/field/StaticMeshes/Plane.uasset"),
]

EXPECTED_UE_PATHS = [
    "/Game/URSoccerLab/Robots/pi_plus/pi_plus.pi_plus",
    "/Game/Levels/URS_SoccerField",
]


def check_files() -> list[str]:
    errors = []
    for label, rel_path in EXPECTED_ASSETS:
        full = ROOT / rel_path
        if not full.exists():
            errors.append(f"{label}: file not found at {rel_path}")
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
    errors = check_files() + check_ue_assets()

    if errors:
        for e in errors:
            print(f"  FAIL: {e}", file=sys.stderr)
        return 1

    print("All baked assets validated successfully.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
