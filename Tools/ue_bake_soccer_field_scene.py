#!/usr/bin/env python3
"""Bake the soccer-field GLB plus default lighting into a UE level.

Runs inside Unreal Editor via ``-game -ExecCmds="py Tools/ue_bake_soccer_field_scene.py; Quit"``.
Uses only native UE Python APIs — no project C++ dependency.
"""

from __future__ import annotations

import json
import struct
from pathlib import Path

import unreal


ROOT = Path(__file__).resolve().parents[1]
FIELD_GLB = ROOT / "Assets/Scenes/SoccerField/source/field.glb"
FIELD_IMPORT_PATH = "/Game/URSoccerLab/Scenes/SoccerField"
LEVEL_NAME = "URS_SoccerField"
LEVEL_PATH = f"/Game/Levels/{LEVEL_NAME}"
SKY_CUBEMAP = "/Engine/MapTemplates/Sky/DaylightAmbientCubemap.DaylightAmbientCubemap"
SKY_INTENSITY = 3.0


def read_glb_nodes(path: Path) -> list[dict]:
    data = path.read_bytes()
    magic = struct.unpack_from("<I", data, 0)[0]
    if magic != 0x46546C67:
        raise RuntimeError(f"{path} is not a GLB file")
    chunk_length = struct.unpack_from("<I", data, 12)[0]
    glb = json.loads(data[20 : 20 + chunk_length].decode("utf-8"))
    scene_index = glb.get("scene", 0)
    scene_nodes = set(glb["scenes"][scene_index]["nodes"])
    return [n for i, n in enumerate(glb["nodes"]) if i in scene_nodes]


def import_field_meshes() -> dict[str, unreal.StaticMesh]:
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    task = unreal.AssetImportTask()
    task.filename = str(FIELD_GLB)
    task.destination_path = FIELD_IMPORT_PATH
    task.automated = True
    task.save = True
    task.replace_existing = True
    task.replace_existing_settings = True
    asset_tools.import_asset_tasks([task])

    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    meshes: dict[str, unreal.StaticMesh] = {}
    for asset_data in registry.get_assets_by_path(FIELD_IMPORT_PATH, recursive=True):
        if str(asset_data.asset_class_path.asset_name) != "StaticMesh":
            continue
        mesh = asset_data.get_asset()
        if mesh:
            meshes[str(asset_data.asset_name)] = mesh
    if not meshes:
        raise RuntimeError(f"no static meshes imported from {FIELD_GLB}")
    return meshes


def create_level() -> None:
    if unreal.EditorAssetLibrary.does_asset_exist(LEVEL_PATH):
        unreal.EditorAssetLibrary.delete_asset(LEVEL_PATH)
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    factory = unreal.WorldFactory()
    factory.world_type = unreal.WorldType.Editor
    asset_tools.create_asset(LEVEL_NAME, "/Game/Levels", None, factory)


def spawn_mesh_actors(meshes: dict[str, unreal.StaticMesh], nodes: list[dict]) -> None:
    editor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    origin = unreal.Vector(0.0, 0.0, 0.0)
    rot = unreal.Rotator(0.0, 0.0, 0.0)
    scale = unreal.Vector(1.0, 1.0, 1.0)
    for index, node in enumerate(nodes):
        name = node.get("name")
        if not name or "mesh" not in node:
            continue
        mesh = meshes.get(name)
        if mesh is None:
            raise RuntimeError(f"missing imported static mesh for GLB node {name}")
        actor = editor.spawn_actor_from_object(mesh, origin, rot, scale)
        if actor:
            actor.set_actor_label(f"URS_SoccerField_{index}_{name}")
            actor.set_editor_property("actor_guid", unreal.Guid.new_guid())


def spawn_skylight() -> None:
    editor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    cubemap = unreal.load_asset(SKY_CUBEMAP)
    sky = editor.spawn_actor_from_class(
        unreal.SkyLight, unreal.Vector(0, 0, 0), unreal.Rotator(0, 0, 0)
    )
    if sky:
        sky.set_editor_property("source_type", unreal.SkyLightSourceType.SLS_SPECIFIED_CUBEMAP)
        sky.set_editor_property("cubemap", cubemap)
        sky.set_editor_property("intensity", SKY_INTENSITY)
        sky.set_actor_label("URS_DefaultSkyLight")


def save_level() -> None:
    unreal.EditorLoadingAndSavingUtils.save_current_level()


def main() -> None:
    nodes = read_glb_nodes(FIELD_GLB)
    meshes = import_field_meshes()
    create_level()
    spawn_mesh_actors(meshes, nodes)
    spawn_skylight()
    save_level()
    unreal.log(f"URSoccerLab soccer field scene ready at {LEVEL_PATH}")


if __name__ == "__main__":
    main()
