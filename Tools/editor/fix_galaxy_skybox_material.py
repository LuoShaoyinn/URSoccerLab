#!/usr/bin/env python3
"""Make the galaxy skybox render as a pure image (no light, no shadow).

The glTF importer did not produce a visible shell, and the user wants the
galaxy to be a backdrop image only — not a Lumen light source and not a shadow
caster. This tool:

  1. Builds a fresh Unlit, Two-Sided material (Emissive Color = the 4K galaxy
     texture). Unlit renders the texture directly with no dependency on scene
     lights, and Two-Sided makes the inward-facing shell visible from inside.
  2. Assigns that material to slot 0 of the GalaxySkybox StaticMesh.
  3. On the placed URS_GalaxySkybox actor, disables Cast Shadow and
     Affect Dynamic Indirect Lighting so the shell casts no shadow and its
     emissive cannot feed Lumen GI (=> "just an image", no real light).

Run inside Unreal Editor against the built URSoccerLab project. Because it
only edits assets and an existing actor (no spawn), ``-NullRHI`` is fine:

    UnrealEditor-Cmd URSoccerLab.uproject \
      -ExecutePythonScript="$PWD/Tools/editor/fix_galaxy_skybox_material.py" \
      -NullRHI -unattended -nop4 -nosplash
"""

from __future__ import annotations

import unreal


SKYBOX_PACKAGE = "/Game/URSoccerLab/Scenes/SoccerField/Skybox"
MAT_NAME = "GalaxySkybox_M"
MAT_PATH = f"{SKYBOX_PACKAGE}/{MAT_NAME}"
LEVEL_PATH = "/Game/Levels/URS_SoccerField"
ACTOR_LABEL = "URS_GalaxySkybox"


def _first_asset(package: str, cls) -> object:
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    registry.scan_paths_synchronous([package], force_rescan=True)
    for asset_data in registry.get_assets_by_path(package, recursive=True):
        asset = asset_data.get_asset()
        if isinstance(asset, cls):
            return asset
    return None


def _unlit_shading_model():
    for name in ("MSM_UNLIT", "MSM_Unlit", "UNLIT"):
        value = getattr(unreal.MaterialShadingModel, name, None)
        if value is not None:
            return value
    available = [n for n in dir(unreal.MaterialShadingModel) if not n.startswith("_")]
    raise RuntimeError(f"could not resolve MaterialShadingModel Unlit; available: {available}")


def build_material() -> unreal.Material:
    texture = _first_asset(f"{SKYBOX_PACKAGE}/Inside_galaxy_HDRI/Textures", unreal.Texture2D)
    if texture is None:
        raise RuntimeError("galaxy HDRI texture not found under Skybox/Inside_galaxy_HDRI/Textures")

    if unreal.EditorAssetLibrary.does_asset_exist(MAT_PATH):
        unreal.EditorAssetLibrary.delete_asset(MAT_PATH)
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    material = asset_tools.create_asset(MAT_NAME, SKYBOX_PACKAGE, None, unreal.MaterialFactoryNew())
    if not material:
        raise RuntimeError(f"create_asset returned None for {MAT_PATH}")

    material.set_editor_property("shading_model", _unlit_shading_model())
    material.set_editor_property("two_sided", True)

    sample = unreal.MaterialEditingLibrary.create_material_expression(
        material, unreal.MaterialExpressionTextureSample, -400, 0
    )
    sample.set_editor_property("texture", texture)
    unreal.MaterialEditingLibrary.connect_material_property(
        sample, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR
    )
    unreal.MaterialEditingLibrary.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(MAT_PATH)
    unreal.log(f"[fix_skybox] built Unlit Two-Sided material {MAT_PATH} (tex={texture.get_name()})")
    return material


def assign_material_to_mesh(material: unreal.Material) -> None:
    mesh = _first_asset(f"{SKYBOX_PACKAGE}/Inside_galaxy_HDRI/StaticMeshes", unreal.StaticMesh)
    if mesh is None:
        raise RuntimeError("GalaxySkybox StaticMesh not found")
    slot_count = len(mesh.get_editor_property("static_materials"))
    for slot in range(slot_count):
        mesh.set_material(slot, material)
    unreal.EditorAssetLibrary.save_asset(mesh.get_path_name())
    unreal.log(f"[fix_skybox] assigned {MAT_NAME} to {slot_count} slot(s) on {mesh.get_name()}")


def configure_actor() -> None:
    if not unreal.EditorLoadingAndSavingUtils.load_map(LEVEL_PATH):
        raise RuntimeError(f"could not load level {LEVEL_PATH}")
    editor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    target = None
    for actor in editor.get_all_level_actors():
        if actor.get_actor_label() == ACTOR_LABEL:
            target = actor
            break
    if target is None:
        raise RuntimeError(f"actor {ACTOR_LABEL} not found in level")
    component = target.get_editor_property("static_mesh_component")
    # Pure backdrop: no shadow casting and no Lumen GI contribution.
    component.set_editor_property("cast_shadow", False)
    try:
        component.set_editor_property("affect_dynamic_indirect_lighting", False)
    except Exception as exc:
        unreal.log_warning(f"[fix_skybox] affect_dynamic_indirect_lighting skipped: {exc}")
    try:
        component.set_editor_property("cast_volumetric_shadow", False)
    except Exception:
        pass
    unreal.EditorLoadingAndSavingUtils.save_current_level()
    unreal.log(f"[fix_skybox] configured {ACTOR_LABEL}: no shadow, no GI contribution")


def main() -> None:
    material = build_material()
    assign_material_to_mesh(material)
    configure_actor()
    unreal.log("[fix_skybox] done")


if __name__ == "__main__":
    main()
