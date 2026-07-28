#!/usr/bin/env python3
"""Import a robot MJCF/XML through URLab's factory and save the Blueprint.

Runs inside Unreal Editor via:
    $UE $PROJECT -game -ExecCmds="py Tools/ue_import_robot.py; Quit"

Imports the MJCF, then saves the generated Blueprint and all dependency
assets to the project-owned /Game/URSoccerLab/Robots/ path.
"""

from __future__ import annotations

import argparse
from pathlib import Path

import unreal


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_XML = ROOT / "Assets/Robots/pi_plus/pi_plus.xml"
IMPORT_TMP_PATH = "/Game/MuJoCoImports"
ROBOT_PATH = "/Game/URSoccerLab/Robots/pi_plus"
ROBOT_NAME = "pi_plus"


def import_mjcf(xml_path: Path) -> str:
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    task = unreal.AssetImportTask()
    task.filename = str(xml_path)
    task.destination_path = IMPORT_TMP_PATH
    task.destination_name = ROBOT_NAME
    task.automated = True
    task.save = True
    task.replace_existing = True
    task.replace_existing_settings = True
    asset_tools.import_asset_tasks([task])

    bp_path = f"{IMPORT_TMP_PATH}/{ROBOT_NAME}.{ROBOT_NAME}"
    bp = unreal.load_asset(bp_path)
    if not bp:
        raise RuntimeError(f"import did not produce Blueprint at {bp_path}")
    unreal.log(f"[import_robot] imported Blueprint at {bp_path}")
    return bp_path


def migrate_assets(old_prefix: str, new_prefix: str) -> int:
    """Rename all assets under old_prefix to new_prefix using asset tools."""
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    assets = registry.get_assets_by_path(old_prefix, recursive=True)
    rename_data = []
    moved = 0

    for asset_data in assets:
        old_path = asset_data.package_name
        asset = asset_data.get_asset()
        if not asset:
            continue

        new_path = str(old_path).replace(old_prefix, new_prefix, 1)
        obj_name = asset_data.asset_name
        if str(old_path) == f"{old_prefix}/{ROBOT_NAME}":
            new_pkg = new_prefix
            new_name = ROBOT_NAME
        else:
            new_pkg = str(asset_data.package_path).replace(old_prefix, new_prefix, 1)
            new_name = str(obj_name)

        rd = unreal.AssetRenameData(asset, new_pkg, new_name)
        rename_data.append(rd)
        moved += 1

    if rename_data:
        asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
        asset_tools.rename_assets(rename_data)

    unreal.log(f"[import_robot] migrated {moved} asset(s) to {new_prefix}")
    return moved


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--xml", type=Path, default=DEFAULT_XML)
    parser.add_argument("--name", default=ROBOT_NAME)
    args, _ = parser.parse_known_args()

    if not args.xml.exists():
        raise FileNotFoundError(args.xml)

    import_mjcf(args.xml)

    old_prefix = f"{IMPORT_TMP_PATH}/{args.name}"
    old_assets = f"{IMPORT_TMP_PATH}/{args.name}_ue_Assets"
    new_prefix = f"{ROBOT_PATH}"
    new_assets = f"{ROBOT_PATH}/dependencies"

    migrate_assets(old_prefix, new_prefix)
    migrate_assets(old_assets, new_assets)

    bp_path = f"{ROBOT_PATH}/{ROBOT_NAME}.{ROBOT_NAME}"
    if not unreal.EditorAssetLibrary.does_asset_exist(bp_path):
        raise RuntimeError(f"Blueprint not found at {bp_path} after migration")

    unreal.log(f"[import_robot] done: {bp_path}")


if __name__ == "__main__":
    main()
