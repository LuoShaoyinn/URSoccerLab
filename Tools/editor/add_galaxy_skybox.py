#!/usr/bin/env python3
"""Wire the inside-galaxy skybox HDRI into the production field level.

Imports the self-contained skybox GLB (an inverted sphere with the 4K galaxy
panorama as an emissive material) into Content and spawns one huge instance at
the world origin so it encloses the scene as a backdrop. The glTF material is
already emissive (unlit), so it is unaffected by scene lighting and renders as
a self-lit background wherever no closer geometry occludes it.

Spawning uses ``EditorActorSubsystem.spawn_actor_from_class``. That routes
through editor viewport placement, which divides by zero (SIGFPE) under
``-NullRHI``; run this tool with ``UnrealEditor ... -RenderOffscreen`` (real
offscreen Vulkan) instead of ``-NullRHI``.

Run inside Unreal Editor against the built URSoccerLab project:

    UnrealEditor URSoccerLab.uproject \
      -ExecutePythonScript="$PWD/Tools/editor/add_galaxy_skybox.py" \
      -RenderOffscreen -unattended -nop4 -nosplash
"""

from __future__ import annotations

from pathlib import Path

import unreal


ROOT = Path(__file__).resolve().parents[2]
GLB_PATH = ROOT / "refs/inside-galaxy-skybox-hdri-360-panorama/source/Inside galaxy HDRI.glb"

SKYBOX_PACKAGE = "/Game/URSoccerLab/Scenes/SoccerField/Skybox"
SKYBOX_ASSET_NAME = "GalaxySkybox"
LEVEL_PATH = "/Game/Levels/URS_SoccerField"
ACTOR_LABEL = "URS_GalaxySkybox"
TARGET_RADIUS_UE = 100000.0  # 1 km; encloses the field as a far backdrop


def _find_skybox_mesh() -> unreal.StaticMesh | None:
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    registry.scan_paths_synchronous([SKYBOX_PACKAGE], force_rescan=True)
    for asset_data in registry.get_assets_by_path(SKYBOX_PACKAGE, recursive=True):
        asset = asset_data.get_asset()
        if isinstance(asset, unreal.StaticMesh):
            return asset
    return None


def import_skybox_glb() -> unreal.StaticMesh:
    existing = _find_skybox_mesh()
    if existing is not None:
        unreal.log(f"[add_galaxy_skybox] reusing existing mesh {existing.get_path_name()}")
        return existing
    if not GLB_PATH.is_file():
        raise FileNotFoundError(GLB_PATH)
    unreal.EditorAssetLibrary.make_directory(SKYBOX_PACKAGE)

    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", str(GLB_PATH))
    task.set_editor_property("destination_path", SKYBOX_PACKAGE)
    task.set_editor_property("destination_name", SKYBOX_ASSET_NAME)
    task.set_editor_property("automated", True)
    task.set_editor_property("save", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("replace_existing_settings", True)
    asset_tools.import_asset_tasks([task])

    mesh = _find_skybox_mesh()
    if mesh is None:
        raise RuntimeError(
            f"glTF import of {GLB_PATH.name} produced no StaticMesh under "
            f"{SKYBOX_PACKAGE}"
        )
    # glTF imports as Nanite by default under Interchange; a skybox does not
    # need it and Nanite cannot represent a cleanly inverted two-sided shell.
    try:
        settings = mesh.get_editor_property("nanite_settings")
        settings.set_editor_property("enabled", False)
        mesh.set_editor_property("nanite_settings", settings)
        unreal.EditorAssetLibrary.save_asset(mesh.get_path_name())
    except Exception as exc:
        unreal.log_warning(f"[add_galaxy_skybox] nanite tweak skipped: {exc}")
    return mesh


def _mesh_radius_ue(mesh: unreal.StaticMesh) -> float:
    box = mesh.get_bounding_box()
    extent = box.max - box.min
    return float(max(extent.x, extent.y, extent.z)) * 0.5


def place_in_level(mesh: unreal.StaticMesh) -> unreal.Actor:
    if not unreal.EditorLoadingAndSavingUtils.load_map(LEVEL_PATH):
        raise RuntimeError(f"could not load level {LEVEL_PATH}")

    editor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    for actor in editor.get_all_level_actors():
        if actor.get_actor_label() == ACTOR_LABEL:
            editor.destroy_actor(actor)

    mesh_radius = _mesh_radius_ue(mesh)
    scale = TARGET_RADIUS_UE / mesh_radius if mesh_radius > 0 else 1.0
    actor = editor.spawn_actor_from_class(
        unreal.StaticMeshActor, unreal.Vector(0.0, 0.0, 0.0), unreal.Rotator(0.0, 0.0, 0.0)
    )
    actor.set_actor_label(ACTOR_LABEL)
    component = actor.get_editor_property("static_mesh_component")
    component.set_static_mesh(mesh)
    component.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
    actor.set_actor_scale3d(unreal.Vector(scale, scale, scale))

    unreal.EditorLoadingAndSavingUtils.save_current_level()
    unreal.log(
        f"[add_galaxy_skybox] placed {ACTOR_LABEL} (mesh radius {mesh_radius:.1f} uu, "
        f"scale {scale:.1f}, final radius ~{TARGET_RADIUS_UE:.0f} uu)"
    )
    return actor


def main() -> None:
    mesh = import_skybox_glb()
    place_in_level(mesh)
    unreal.log("[add_galaxy_skybox] done")


if __name__ == "__main__":
    main()
