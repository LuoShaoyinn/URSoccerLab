#!/usr/bin/env python3
"""Import a robot MJCF/XML through URLab's factory and save the Blueprint.

Runs inside Unreal Editor via:
    $UE $PROJECT -ExecutePythonScript=Tools/editor/import_robot.py

Imports the MJCF, then saves the generated Blueprint and all dependency
assets to the project-owned /Game/URSoccerLab/Robots/ path.
"""

from __future__ import annotations

import argparse
from pathlib import Path

import unreal


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_XML = ROOT / "Assets/Robots/pi_plus/pi_plus.xml"
IMPORT_TMP_PATH = "/Game/MuJoCoImports"
ROBOT_NAME = "pi_plus"


def import_mjcf(xml_path: Path, robot_name: str) -> str:
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    task = unreal.AssetImportTask()
    task.filename = str(xml_path)
    task.destination_path = IMPORT_TMP_PATH
    task.destination_name = robot_name
    task.automated = True
    task.save = True
    task.replace_existing = True
    task.replace_existing_settings = True
    asset_tools.import_asset_tasks([task])

    bp_path = f"{IMPORT_TMP_PATH}/{robot_name}.{robot_name}"
    bp = unreal.load_asset(bp_path)
    if not bp:
        raise RuntimeError(f"import did not produce Blueprint at {bp_path}")
    unreal.log(f"[import_robot] imported Blueprint at {bp_path}")
    return bp_path


def migrate_robot_assets(
    blueprint_package_path: str,
    dependency_path: str,
    robot_path: str,
    robot_name: str,
) -> int:
    """Atomically move the imported Blueprint and every dependency asset."""
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    registry.scan_paths_synchronous([dependency_path], force_rescan=True)

    blueprint = unreal.load_asset(f"{blueprint_package_path}.{robot_name}")
    if not blueprint:
        raise RuntimeError(f"Blueprint not found at {blueprint_package_path}.{robot_name}")
    rename_data = [unreal.AssetRenameData(blueprint, robot_path, robot_name)]

    for asset_data in registry.get_assets_by_path(dependency_path, recursive=True):
        asset = asset_data.get_asset()
        if not asset:
            continue
        new_package = str(asset_data.package_path).replace(
            dependency_path, f"{robot_path}/dependencies", 1
        )
        rename_data.append(unreal.AssetRenameData(asset, new_package, str(asset_data.asset_name)))

    if not unreal.AssetToolsHelpers.get_asset_tools().rename_assets(rename_data):
        raise RuntimeError(f"could not migrate imported robot assets to {robot_path}")
    moved = len(rename_data)
    unreal.log(f"[import_robot] migrated {moved} asset(s) to {robot_path}")
    return moved


def remove_existing_robot_assets(robot_path: str) -> None:
    if unreal.EditorAssetLibrary.does_directory_exist(robot_path):
        if not unreal.EditorAssetLibrary.delete_directory(robot_path):
            raise RuntimeError(f"could not remove existing robot assets at {robot_path}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--xml", type=Path, default=DEFAULT_XML)
    parser.add_argument("--name", default=ROBOT_NAME)
    args, _ = parser.parse_known_args()

    if not args.xml.exists():
        raise FileNotFoundError(args.xml)

    import_mjcf(args.xml, args.name)

    old_assets = f"{IMPORT_TMP_PATH}/{args.name}_ue_Assets"
    new_prefix = f"/Game/URSoccerLab/Robots/{args.name}"

    remove_existing_robot_assets(new_prefix)
    migrate_robot_assets(f"{IMPORT_TMP_PATH}/{args.name}", old_assets, new_prefix, args.name)

    bp_path = f"{new_prefix}/{args.name}.{args.name}"
    if not unreal.EditorAssetLibrary.does_asset_exist(bp_path):
        raise RuntimeError(f"Blueprint not found at {bp_path} after migration")

    unreal.log(f"[import_robot] done: {bp_path}")


if __name__ == "__main__":
    main()
