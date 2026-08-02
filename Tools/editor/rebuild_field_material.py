#!/usr/bin/env python3
"""Rebuild the soccer-field ground material from the ChatGPT pitch image and the
grass1-ue PBR set.

Stages (all idempotent):

  1. Re-import refs/ChatGPT Image...png into the existing Content texture
     /Game/URSoccerLab/Scenes/SoccerField/Field/Textures/field (in place, so the
     asset path and all references are preserved).
  2. Import the non-albedo grass1-ue PBR maps (normal / roughness / ao / height)
     under Field/Textures/grass1_ue/, with linear/normalmap compression.
  3. (Re)create the Material grass1-ue: BaseColor = field texture (the ChatGPT
     pitch), Normal/Roughness/AO from the grass PBR (tiled via a TextureCoordinate
     node). Height is imported for later parallax use but not wired.
  4. Load the production level and override the material slot of every mesh
     component currently using the Field material with grass1-ue, then save.

Run inside Unreal Editor against the built URSoccerLab project:

    UnrealEditor-Cmd URSoccerLab.uproject \
      -ExecutePythonScript="$PWD/Tools/editor/rebuild_field_material.py" \
      -NullRHI -unattended -nop4 -nosplash
"""

from __future__ import annotations

from pathlib import Path

import unreal


ROOT = Path(__file__).resolve().parents[2]

CHATGPT_PNG = ROOT / "refs" / "ChatGPT Image Aug 2, 2026, 10_04_25 PM.png"
GRASS_PBR_DIR = ROOT / "refs" / "grass1-ue"

FIELD_TEX_PACKAGE = "/Game/URSoccerLab/Scenes/SoccerField/Field/Textures"
FIELD_TEX_NAME = "field"
FIELD_TEX_PATH = f"{FIELD_TEX_PACKAGE}/{FIELD_TEX_NAME}"

FIELD_MAT_PACKAGE = "/Game/URSoccerLab/Scenes/SoccerField/Field/Materials"
FIELD_MAT_PATH = f"{FIELD_MAT_PACKAGE}/Field"

GRASS_TEX_PACKAGE = f"{FIELD_TEX_PACKAGE}/grass1_ue"
GRASS_MAT_NAME = "grass1-ue"
GRASS_MAT_PATH = f"{FIELD_MAT_PACKAGE}/{GRASS_MAT_NAME}"

LEVEL_PATH = "/Game/Levels/URS_SoccerField"

DETAIL_TILING = 8

# (source filename, asset name, compression settings, srgb)
GRASS_MAPS = [
    ("grass1-normal1-dx.png", "grass1_ue_normal",
     unreal.TextureCompressionSettings.TC_NORMALMAP, False),
    ("grass1-rough.png", "grass1_ue_rough",
     unreal.TextureCompressionSettings.TC_DEFAULT, False),
    ("grass1-ao.png", "grass1_ue_ao",
     unreal.TextureCompressionSettings.TC_DEFAULT, False),
    ("grass1-height.png", "grass1_ue_height",
     unreal.TextureCompressionSettings.TC_DEFAULT, False),
]


def _import_texture(src: Path, dest_package: str, dest_name: str,
                    srgb: bool, compression) -> unreal.Texture2D:
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", str(src))
    task.set_editor_property("destination_path", dest_package)
    task.set_editor_property("destination_name", dest_name)
    task.set_editor_property("automated", True)
    task.set_editor_property("save", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("replace_existing_settings", True)
    asset_tools.import_asset_tasks([task])

    texture = unreal.load_asset(f"{dest_package}/{dest_name}")
    if not isinstance(texture, unreal.Texture2D):
        raise RuntimeError(
            f"import of {src} did not produce a Texture2D at "
            f"{dest_package}/{dest_name}"
        )

    changed = False
    if texture.get_editor_property("srgb") != srgb:
        texture.set_editor_property("srgb", srgb)
        changed = True
    if texture.get_editor_property("compression_settings") != compression:
        texture.set_editor_property("compression_settings", compression)
        changed = True
    if changed:
        unreal.EditorAssetLibrary.save_asset(f"{dest_package}/{dest_name}")
    return texture


def stage_import_field_texture() -> unreal.Texture2D:
    if not CHATGPT_PNG.is_file():
        raise FileNotFoundError(CHATGPT_PNG)
    unreal.log(f"[rebuild_field] reimporting field texture from {CHATGPT_PNG.name}")
    return _import_texture(
        CHATGPT_PNG, FIELD_TEX_PACKAGE, FIELD_TEX_NAME,
        srgb=True, compression=unreal.TextureCompressionSettings.TC_DEFAULT,
    )


def stage_import_grass_textures() -> dict[str, unreal.Texture2D]:
    if not GRASS_PBR_DIR.is_dir():
        raise FileNotFoundError(GRASS_PBR_DIR)
    unreal.EditorAssetLibrary.make_directory(GRASS_TEX_PACKAGE)
    textures: dict[str, unreal.Texture2D] = {}
    for src_name, asset_name, compression, srgb in GRASS_MAPS:
        src = GRASS_PBR_DIR / src_name
        if not src.is_file():
            raise FileNotFoundError(src)
        unreal.log(f"[rebuild_field] importing {src_name} -> {asset_name}")
        textures[asset_name] = _import_texture(
            src, GRASS_TEX_PACKAGE, asset_name,
            srgb=srgb, compression=compression,
        )
    return textures


def _connect(expression, output_name: str, prop: unreal.MaterialProperty) -> None:
    unreal.MaterialEditingLibrary.connect_material_property(
        expression, output_name, prop
    )


def _link_uv(texcoord, sample) -> None:
    unreal.MaterialEditingLibrary.connect_material_expressions(
        texcoord, "", sample, "UVs"
    )


def _add_texture_sample(material, texture, x: int, y: int) -> object:
    node = unreal.MaterialEditingLibrary.create_material_expression(
        material, unreal.MaterialExpressionTextureSample, x, y
    )
    node.set_editor_property("texture", texture)
    return node


def stage_build_grass_material(
    field_texture: unreal.Texture2D,
    grass_textures: dict[str, unreal.Texture2D],
) -> unreal.Material:
    if unreal.EditorAssetLibrary.does_asset_exist(GRASS_MAT_PATH):
        if not unreal.EditorAssetLibrary.delete_asset(GRASS_MAT_PATH):
            raise RuntimeError(f"could not delete existing material {GRASS_MAT_PATH}")

    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    factory = unreal.MaterialFactoryNew()
    material = asset_tools.create_asset(
        GRASS_MAT_NAME, FIELD_MAT_PACKAGE, None, factory
    )
    if not material:
        raise RuntimeError(f"create_asset returned None for {GRASS_MAT_PATH}")

    MP = unreal.MaterialProperty

    base = _add_texture_sample(material, field_texture, -400, -150)
    _connect(base, "", MP.MP_BASE_COLOR)

    tiling = unreal.MaterialEditingLibrary.create_material_expression(
        material, unreal.MaterialExpressionTextureCoordinate, -900, 250
    )
    tiling.set_editor_property("utiling", DETAIL_TILING)
    tiling.set_editor_property("vtiling", DETAIL_TILING)

    normal = _add_texture_sample(material, grass_textures["grass1_ue_normal"], -400, 150)
    _link_uv(tiling, normal)
    _connect(normal, "", MP.MP_NORMAL)

    rough = _add_texture_sample(material, grass_textures["grass1_ue_rough"], -400, 400)
    _link_uv(tiling, rough)
    _connect(rough, "", MP.MP_ROUGHNESS)

    ao = _add_texture_sample(material, grass_textures["grass1_ue_ao"], -400, 600)
    _link_uv(tiling, ao)
    _connect(ao, "", MP.MP_AMBIENT_OCCLUSION)

    unreal.MaterialEditingLibrary.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(GRASS_MAT_PATH)
    unreal.log(f"[rebuild_field] material built: {GRASS_MAT_PATH}")
    return material


def stage_assign_in_level(grass_material: unreal.Material) -> int:
    if not unreal.EditorLoadingAndSavingUtils.load_map(LEVEL_PATH):
        raise RuntimeError(f"could not load level {LEVEL_PATH}")

    editor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    field_package = FIELD_MAT_PATH
    grass_path = grass_material.get_path_name()

    touched_components: int = 0
    touched_actors: list[str] = []
    for actor in editor.get_all_level_actors():
        components = actor.get_components_by_class(unreal.StaticMeshComponent)
        for comp in components:
            try:
                slot_count = comp.get_num_materials()
            except Exception:
                continue
            swapped = False
            for slot in range(slot_count):
                current = comp.get_material(slot)
                if not current:
                    continue
                if current.get_path_name().split(".")[0] == field_package:
                    comp.set_material(slot, grass_material)
                    swapped = True
            if swapped:
                touched_components += 1
                touched_actors.append(actor.get_actor_label())

    if touched_components == 0:
        unreal.log_warning(
            "[rebuild_field] no StaticMeshComponent currently references the "
            f"Field material at {field_package}; level left unchanged"
        )
    else:
        unreal.EditorLoadingAndSavingUtils.save_current_level()
    unreal.log(
        f"[rebuild_field] assigned grass1-ue to {touched_components} component(s) "
        f"on actor(s): {touched_actors}"
    )
    return touched_components


def main() -> None:
    field_texture = stage_import_field_texture()
    grass_textures = stage_import_grass_textures()
    material = stage_build_grass_material(field_texture, grass_textures)
    stage_assign_in_level(material)
    unreal.log("[rebuild_field] done")


if __name__ == "__main__":
    main()
