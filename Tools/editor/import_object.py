#!/usr/bin/env python3
"""Import a dynamic-object MJCF and migrate its complete baked asset tree.

Run inside Unreal Editor:

    UnrealEditor-Cmd URSoccerLab.uproject \
      -ExecutePythonScript="$PWD/Tools/editor/import_object.py" \
      -NullRHI -Unattended -NoSplash
"""

from __future__ import annotations

import argparse
from pathlib import Path

import unreal


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_NAME = "soccer_ball"
DEFAULT_XML = ROOT / "Assets/Objects" / DEFAULT_NAME / f"{DEFAULT_NAME}.xml"
IMPORT_ROOT = "/Game/MuJoCoImports/Objects"
TARGET_ROOT = "/Game/URSoccerLab/Objects"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--xml", type=Path, default=DEFAULT_XML)
    parser.add_argument("--name", default=DEFAULT_NAME)
    args, _ = parser.parse_known_args()
    if not args.xml.is_file():
        raise FileNotFoundError(args.xml)

    target_path = f"{TARGET_ROOT}/{args.name}"
    if unreal.EditorAssetLibrary.does_directory_exist(target_path):
        raise RuntimeError(
            f"baked assets already exist at {target_path}; move the corresponding "
            "Content directory outside the project before launching Unreal to "
            "perform a clean rebuild"
        )

    import_path = f"{IMPORT_ROOT}/{args.name}"

    task = unreal.AssetImportTask()
    task.filename = str(args.xml)
    task.destination_path = import_path
    task.destination_name = args.name
    task.automated = True
    task.save = True
    task.replace_existing = True
    task.replace_existing_settings = True
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])

    blueprint = unreal.load_asset(f"{import_path}/{args.name}.{args.name}")
    if not blueprint:
        raise RuntimeError("URLab did not produce the object Blueprint")

    dependency_path = f"{import_path}/{args.name}_Assets"
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    registry.scan_paths_synchronous([dependency_path], force_rescan=True)
    renames = [unreal.AssetRenameData(blueprint, target_path, args.name)]
    for asset_data in registry.get_assets_by_path(dependency_path, recursive=True):
        asset = asset_data.get_asset()
        if not asset:
            continue
        relative = str(asset_data.package_path).replace(dependency_path, "", 1)
        renames.append(
            unreal.AssetRenameData(
                asset,
                f"{target_path}/Dependencies{relative}",
                str(asset_data.asset_name),
            )
        )
    if not unreal.AssetToolsHelpers.get_asset_tools().rename_assets(renames):
        raise RuntimeError(f"could not migrate object assets to {target_path}")

    # Interchange enables Nanite by default for GLBs, but its stock glTF
    # material parents are not all Nanite-compatible. Dynamic simulation
    # visuals are small and move every frame, so preserve their PBR materials
    # on the conventional static-mesh path.
    registry.scan_paths_synchronous([target_path], force_rescan=True)
    for asset_data in registry.get_assets_by_path(target_path, recursive=True):
        asset = asset_data.get_asset()
        if not isinstance(asset, unreal.StaticMesh):
            continue
        settings = asset.get_editor_property("nanite_settings")
        settings.set_editor_property("enabled", False)
        asset.set_editor_property("nanite_settings", settings)

    if unreal.EditorAssetLibrary.does_directory_exist(import_path):
        if not unreal.EditorAssetLibrary.delete_directory(import_path):
            raise RuntimeError("could not remove import staging assets")

    baked = f"{target_path}/{args.name}.{args.name}"
    if not unreal.EditorAssetLibrary.does_asset_exist(baked):
        raise RuntimeError(f"missing baked object Blueprint: {baked}")
    unreal.EditorAssetLibrary.save_directory(target_path, only_if_is_dirty=False)
    unreal.log(f"[import_object] done: {baked}")


if __name__ == "__main__":
    main()
