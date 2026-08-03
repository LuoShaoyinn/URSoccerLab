#!/usr/bin/env python3
"""Import the MOS9 robot Blueprint from Assets/Robots/mos9/mos9.xml.

Run inside Unreal Editor via:
    $UE $PROJECT -ExecutePythonScript=Tools/editor/import_mos9.py -RenderOffscreen
"""

from __future__ import annotations
from pathlib import Path
import unreal

ROOT = Path(__file__).resolve().parents[2]
XML_PATH = ROOT / "Assets/Robots/mos9/mos9.xml"
ROBOT_NAME = "mos9"
IMPORT_TMP_PATH = "/Game/MuJoCoImports"
DEST_PATH = f"/Game/URSoccerLab/Robots/{ROBOT_NAME}"


def import_mjcf() -> str:
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    task = unreal.AssetImportTask()
    task.filename = str(XML_PATH)
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
    unreal.log(f"[import_mos9] imported Blueprint at {bp_path}")
    return bp_path


def migrate_assets(dependency_path: str) -> int:
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    registry.scan_paths_synchronous([dependency_path], force_rescan=True)

    blueprint = unreal.load_asset(f"{IMPORT_TMP_PATH}/{ROBOT_NAME}.{ROBOT_NAME}")
    if not blueprint:
        raise RuntimeError(f"Blueprint not found at {IMPORT_TMP_PATH}/{ROBOT_NAME}")

    rename_data = [unreal.AssetRenameData(blueprint, DEST_PATH, ROBOT_NAME)]

    for asset_data in registry.get_assets_by_path(dependency_path, recursive=True):
        asset = asset_data.get_asset()
        if not asset:
            continue
        if isinstance(asset, unreal.StaticMesh):
            new_package = f"{DEST_PATH}/Meshes"
        elif isinstance(asset, unreal.MaterialInterface):
            new_package = f"{DEST_PATH}/Materials"
        elif isinstance(asset, unreal.Texture):
            new_package = f"{DEST_PATH}/Textures"
        else:
            relative_path = str(asset_data.package_path).replace(dependency_path, "", 1)
            new_package = f"{DEST_PATH}/Dependencies{relative_path}"
        rename_data.append(unreal.AssetRenameData(asset, new_package, str(asset_data.asset_name)))

    if not unreal.AssetToolsHelpers.get_asset_tools().rename_assets(rename_data):
        raise RuntimeError(f"could not migrate assets to {DEST_PATH}")
    unreal.log(f"[import_mos9] migrated {len(rename_data)} asset(s) to {DEST_PATH}")
    return len(rename_data)


def remove_directory(path: str) -> None:
    if unreal.EditorAssetLibrary.does_directory_exist(path):
        if not unreal.EditorAssetLibrary.delete_directory(path):
            raise RuntimeError(f"could not remove {path}")


def cleanup_staging() -> None:
    remove_directory(IMPORT_TMP_PATH)
    staging_dir = ROOT / "Content" / "MuJoCoImports"
    if staging_dir.exists():
        for directory in sorted(
            (p for p in staging_dir.rglob("*") if p.is_dir()),
            key=lambda p: len(p.parts),
            reverse=True,
        ):
            directory.rmdir()
        staging_dir.rmdir()


def main() -> None:
    if not XML_PATH.is_file():
        raise FileNotFoundError(XML_PATH)

    remove_directory(DEST_PATH)
    import_mjcf()
    migrate_assets(f"{IMPORT_TMP_PATH}/{ROBOT_NAME}_Assets")
    cleanup_staging()

    bp_path = f"{DEST_PATH}/{ROBOT_NAME}.{ROBOT_NAME}"
    if not unreal.EditorAssetLibrary.does_asset_exist(bp_path):
        raise RuntimeError(f"Blueprint not found at {bp_path} after migration")
    unreal.log(f"[import_mos9] done: {bp_path}")


if __name__ == "__main__":
    main()
