#!/usr/bin/env python3
"""Bake the SoccerField MuJoCo collision scene into the existing UE level.

Run inside Unreal Editor:

    UnrealEditor URSoccerLab.uproject \
      -ExecutePythonScript=Tools/editor/bake_field_physics.py

The resulting Blueprint is URLab-owned physics data. The GLB field in the
level remains visual-only; this actor contributes the MuJoCo ground plane.
"""
from __future__ import annotations

from pathlib import Path

import unreal


ROOT = Path(__file__).resolve().parents[2]
XML_PATH = ROOT / "Assets/Scenes/SoccerField/physics/field_physics.xml"
LEVEL_PATH = "/Game/Levels/URS_SoccerField"
IMPORT_PATH = "/Game/MuJoCoImports"
TARGET_PATH = "/Game/URSoccerLab/Scenes/SoccerField/Physics"
ASSET_NAME = "field_physics"
ACTOR_LABEL = "URS_SoccerFieldPhysics"


def import_physics_blueprint() -> str:
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    task = unreal.AssetImportTask()
    task.filename = str(XML_PATH)
    task.destination_path = IMPORT_PATH
    task.destination_name = ASSET_NAME
    task.automated = True
    task.save = True
    task.replace_existing = True
    task.replace_existing_settings = True
    asset_tools.import_asset_tasks([task])

    blueprint = unreal.load_asset(f"{IMPORT_PATH}/{ASSET_NAME}.{ASSET_NAME}")
    if not blueprint:
        raise RuntimeError(f"URLab import did not produce {IMPORT_PATH}/{ASSET_NAME}")

    if unreal.EditorAssetLibrary.does_directory_exist(TARGET_PATH):
        if not unreal.EditorAssetLibrary.delete_directory(TARGET_PATH):
            raise RuntimeError(f"could not remove prior physics assets at {TARGET_PATH}")

    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    dependency_path = f"{IMPORT_PATH}/{ASSET_NAME}_ue_Assets"
    registry.scan_paths_synchronous([dependency_path], force_rescan=True)
    renames = [unreal.AssetRenameData(blueprint, TARGET_PATH, ASSET_NAME)]
    for asset_data in registry.get_assets_by_path(dependency_path, recursive=True):
        asset = asset_data.get_asset()
        if not asset:
            continue
        destination = str(asset_data.package_path).replace(
            dependency_path, f"{TARGET_PATH}/dependencies", 1
        )
        renames.append(unreal.AssetRenameData(asset, destination, str(asset_data.asset_name)))
    if not asset_tools.rename_assets(renames):
        raise RuntimeError("could not migrate imported field physics assets")

    blueprint_path = f"{TARGET_PATH}/{ASSET_NAME}.{ASSET_NAME}"
    if not unreal.EditorAssetLibrary.does_asset_exist(blueprint_path):
        raise RuntimeError(f"missing migrated physics Blueprint: {blueprint_path}")
    return blueprint_path


def place_physics_actor(blueprint_path: str) -> None:
    if not unreal.EditorLoadingAndSavingUtils.load_map(LEVEL_PATH):
        raise RuntimeError(f"could not load field level {LEVEL_PATH}")

    editor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    for actor in editor.get_all_level_actors():
        if actor.get_actor_label() == ACTOR_LABEL:
            editor.destroy_actor(actor)

    blueprint_class = unreal.EditorAssetLibrary.load_blueprint_class(blueprint_path)
    if not blueprint_class:
        raise RuntimeError(f"could not load generated class for {blueprint_path}")
    actor = editor.spawn_actor_from_class(
        blueprint_class, unreal.Vector(0.0, 0.0, 0.0), unreal.Rotator(0.0, 0.0, 0.0)
    )
    if not actor:
        raise RuntimeError("could not place field physics actor")
    # URLab uses the actor transform as the static MuJoCo articulation frame.
    actor.set_actor_location(unreal.Vector(0.0, 0.0, 0.0), False, False)
    actor.set_actor_rotation(unreal.Rotator(0.0, 0.0, 0.0), False)
    location = actor.get_actor_location()
    if abs(location.x) > 0.01 or abs(location.y) > 0.01 or abs(location.z) > 0.01:
        raise RuntimeError(f"field physics actor was not placed at origin: {location}")
    actor.set_actor_label(ACTOR_LABEL)
    unreal.EditorLoadingAndSavingUtils.save_current_level()


def main() -> None:
    if not XML_PATH.is_file():
        raise FileNotFoundError(XML_PATH)
    blueprint_path = import_physics_blueprint()
    place_physics_actor(blueprint_path)
    unreal.log(f"[bake_field_physics] placed {ACTOR_LABEL} from {blueprint_path}")


if __name__ == "__main__":
    main()
