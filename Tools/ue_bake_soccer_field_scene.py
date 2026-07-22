#!/usr/bin/env python3
"""Bake the soccer-field GLB plus default lighting into a UE level.

This script runs inside Unreal Editor via ``-ExecutePythonScript``. It is
deliberately editor-only: packaged simulator builds should load the baked
``/Game/Levels/URS_SoccerField`` map, not recreate it.

The UE Interchange glTF importer (default ``bBakeMeshes = true``) bakes
each glTF scene-node's full global transform (translation, rotation,
scale, plus the 100x m-to-cm conversion) directly into the mesh
vertices.  The imported ``UStaticMesh`` assets are therefore already in
the correct world-space position and orientation.  We spawn every mesh
actor at the origin with identity rotation and unit scale so we do not
double-apply the node transform.
"""

from __future__ import annotations

import json
import math
import struct
from pathlib import Path

import unreal


ROOT = Path(__file__).resolve().parents[1]
FIELD_GLB = ROOT / "Assets/Scenes/SoccerField/source/field.glb"
FIELD_IMPORT_PATH = "/Game/URSoccerLab/Scenes/SoccerField"
LEVEL_PATH = "/Game/Levels/URS_SoccerField"
SUCCESS_MARKER = ROOT / "Saved/Logs/URS_CreateSoccerFieldScene.done"
SKY_CUBEMAP = "/Engine/MapTemplates/Sky/DaylightAmbientCubemap.DaylightAmbientCubemap"
SKY_INTENSITY = 3.0


def read_glb_json(path: Path) -> dict:
    data = path.read_bytes()
    magic, _version, _length = struct.unpack_from("<III", data, 0)
    if magic != 0x46546C67:
        raise RuntimeError(f"{path} is not a GLB file")
    chunk_length, chunk_type = struct.unpack_from("<II", data, 12)
    if chunk_type != 0x4E4F534A:
        raise RuntimeError(f"{path} first GLB chunk is not JSON")
    return json.loads(data[20 : 20 + chunk_length].decode("utf-8"))


def glb_translation_to_ue_cm(value: list[float] | None) -> unreal.Vector:
    """Convert a glTF translation (metres, Y-up) to UE (cm, Z-up).

    Kept for reference; not used in ``spawn_field`` because the Interchange
    importer already bakes translations into mesh vertices.
    """
    x, y_up, z_width = value or [0.0, 0.0, 0.0]
    return unreal.Vector(x * 100.0, z_width * 100.0, y_up * 100.0)


def glb_scale_to_ue(value: list[float] | None) -> unreal.Vector:
    """Convert a glTF scale (Y-up) to UE (Z-up).

    Kept for reference; not used in ``spawn_field`` because the Interchange
    importer already bakes scale into mesh vertices.
    """
    x, y_up, z_width = value or [1.0, 1.0, 1.0]
    return unreal.Vector(x, z_width, y_up)


def glb_rotation_to_ue(rotation: list[float] | None) -> unreal.Rotator:
    """Convert a glTF quaternion (Y-up) to UE rotator (Z-up).

    Kept for reference; not used in ``spawn_field`` because the Interchange
    importer already bakes rotation into mesh vertices.
    """
    if not rotation:
        return unreal.Rotator(0.0, 0.0, 0.0)

    x, y, z, w = rotation
    half_sqrt = math.sqrt(0.5)
    if (
        math.isclose(abs(x), half_sqrt, abs_tol=1e-5)
        and math.isclose(y, 0.0, abs_tol=1e-5)
        and math.isclose(z, 0.0, abs_tol=1e-5)
        and math.isclose(abs(w), half_sqrt, abs_tol=1e-5)
    ):
        return unreal.Rotator(0.0, 0.0, 90.0 if x * w >= 0.0 else -90.0)

    raise RuntimeError(f"unsupported GLB node rotation {rotation}; add an explicit conversion")


def import_field_meshes() -> dict[str, unreal.StaticMesh]:
    if not FIELD_GLB.exists():
        raise FileNotFoundError(FIELD_GLB)

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


def new_level() -> None:
    if not unreal.URSSceneBakeLibrary.create_or_replace_level("URS_SoccerField"):
        raise RuntimeError(f"failed to create level {LEVEL_PATH}")


def spawn_field(meshes: dict[str, unreal.StaticMesh], nodes: list[dict]) -> None:
    """Spawn each imported mesh at the origin with identity transform.

    The Interchange importer (``bBakeMeshes = true`` by default) already
    baked the full glTF node transform — translation, rotation, scale,
    and the 100x m-to-cm conversion — into each mesh's vertices.  We
    must NOT re-apply those transforms here, or every mesh would be
    double-scaled / double-translated.
    """
    for index, node in enumerate(nodes):
        name = node.get("name")
        if not name or "mesh" not in node:
            continue

        mesh = meshes.get(name)
        if mesh is None:
            raise RuntimeError(f"missing imported static mesh for GLB node {name}")

        if not unreal.URSSceneBakeLibrary.spawn_static_mesh_actor(
            mesh,
            f"URS_SoccerField_{index}_{name}",
            unreal.Vector(0.0, 0.0, 0.0),
            unreal.Rotator(0.0, 0.0, 0.0),
            unreal.Vector(1.0, 1.0, 1.0),
            "URLab.ActorId=soccer_field_visual",
        ):
            raise RuntimeError(f"failed to spawn field mesh node {name}")


def spawn_default_skylight() -> None:
    cubemap = unreal.load_asset(SKY_CUBEMAP)
    if not unreal.URSSceneBakeLibrary.spawn_specified_cubemap_sky_light(
        cubemap,
        "URS_DefaultSkyLight",
        SKY_INTENSITY,
        "URLab.ActorId=default_sky_light",
    ):
        raise RuntimeError("failed to spawn default skylight")


def main() -> None:
    glb = read_glb_json(FIELD_GLB)
    scene_index = glb.get("scene", 0)
    scene_nodes = set(glb["scenes"][scene_index]["nodes"])
    nodes = [node for index, node in enumerate(glb["nodes"]) if index in scene_nodes]

    meshes = import_field_meshes()
    new_level()
    spawn_field(meshes, nodes)
    spawn_default_skylight()

    if not unreal.URSSceneBakeLibrary.save_current_level(LEVEL_PATH):
        raise RuntimeError(f"failed to save {LEVEL_PATH}")
    SUCCESS_MARKER.parent.mkdir(parents=True, exist_ok=True)
    SUCCESS_MARKER.write_text(json.dumps({"level": LEVEL_PATH}) + "\n", encoding="utf-8")
    unreal.log(f"URSoccerLab soccer field scene ready at {LEVEL_PATH}")


if __name__ == "__main__":
    main()
