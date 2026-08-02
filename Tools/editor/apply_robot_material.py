#!/usr/bin/env python3
"""Apply the powder-coated-metal PBR material to the pi_plus robot.

Stages (all idempotent):

  1. Import the powder-coated-metal PBR set (albedo / normal / roughness /
     metallic / ao / height) under Robots/pi_plus/Textures/powder_coated_metal_ue,
     with linear/normalmap compression on the data maps.
  2. (Re)create the Material powder-coated-metal: BaseColor/Metallic/Roughness/
     Normal/AO from the PBR set, sampling the authored mesh UVs (1:1, no tiling).
     Height is imported for later displacement use but not wired.
  3. Repoint every material slot of every StaticMesh under
     Robots/pi_plus/Meshes/ at the new material and save, so every spawned
     pi_plus picks up the powder-coated finish.

Run inside Unreal Editor against the built URSoccerLab project:

    UnrealEditor-Cmd URSoccerLab.uproject \
      -ExecutePythonScript="$PWD/Tools/editor/apply_robot_material.py" \
      -NullRHI -unattended -nop4 -nosplash
"""

from __future__ import annotations

from pathlib import Path

import unreal


ROOT = Path(__file__).resolve().parents[2]
PBR_DIR = ROOT / "refs" / "powder-coated-metal-ue"

PI_PLUS_ROOT = "/Game/URSoccerLab/Robots/pi_plus"
TEX_PACKAGE = f"{PI_PLUS_ROOT}/Textures/powder_coated_metal_ue"
MAT_PACKAGE = f"{PI_PLUS_ROOT}/Materials"
MAT_NAME = "powder-coated-metal"
MAT_PATH = f"{MAT_PACKAGE}/{MAT_NAME}"
MESH_PACKAGE = f"{PI_PLUS_ROOT}/Meshes"

# (source filename, asset name, compression, srgb)
PBR_MAPS = [
    ("powder-coated-metal_albedo.png", "pcm_albedo",
     unreal.TextureCompressionSettings.TC_DEFAULT, True),
    ("powder-coated-metal_normal-dx.png", "pcm_normal",
     unreal.TextureCompressionSettings.TC_NORMALMAP, False),
    ("powder-coated-metal_roughness.png", "pcm_rough",
     unreal.TextureCompressionSettings.TC_DEFAULT, False),
    ("powder-coated-metal_metallic.png", "pcm_metallic",
     unreal.TextureCompressionSettings.TC_DEFAULT, False),
    ("powder-coated-metal_ao.png", "pcm_ao",
     unreal.TextureCompressionSettings.TC_DEFAULT, False),
    ("powder-coated-metal_height.png", "pcm_height",
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
    if texture.get_editor_property("srgb") != srgb:
        texture.set_editor_property("srgb", srgb)
    if texture.get_editor_property("compression_settings") != compression:
        texture.set_editor_property("compression_settings", compression)
    unreal.EditorAssetLibrary.save_asset(f"{dest_package}/{dest_name}")
    return texture


def stage_import_pbr() -> dict[str, unreal.Texture2D]:
    if not PBR_DIR.is_dir():
        raise FileNotFoundError(PBR_DIR)
    unreal.EditorAssetLibrary.make_directory(TEX_PACKAGE)
    textures: dict[str, unreal.Texture2D] = {}
    for src_name, asset_name, compression, srgb in PBR_MAPS:
        src = PBR_DIR / src_name
        if not src.is_file():
            raise FileNotFoundError(src)
        unreal.log(f"[apply_robot_material] importing {src_name} -> {asset_name}")
        textures[asset_name] = _import_texture(
            src, TEX_PACKAGE, asset_name, srgb=srgb, compression=compression,
        )
    return textures


def _add_texture_sample(material, texture, x: int, y: int):
    node = unreal.MaterialEditingLibrary.create_material_expression(
        material, unreal.MaterialExpressionTextureSample, x, y
    )
    node.set_editor_property("texture", texture)
    return node


def _connect(expression, output_name: str, prop: unreal.MaterialProperty) -> None:
    unreal.MaterialEditingLibrary.connect_material_property(
        expression, output_name, prop
    )


def stage_build_material(textures: dict[str, unreal.Texture2D]) -> unreal.Material:
    if unreal.EditorAssetLibrary.does_asset_exist(MAT_PATH):
        if not unreal.EditorAssetLibrary.delete_asset(MAT_PATH):
            raise RuntimeError(f"could not delete existing material {MAT_PATH}")

    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    material = asset_tools.create_asset(MAT_NAME, MAT_PACKAGE, None, unreal.MaterialFactoryNew())
    if not material:
        raise RuntimeError(f"create_asset returned None for {MAT_PATH}")

    MP = unreal.MaterialProperty
    _connect(_add_texture_sample(material, textures["pcm_albedo"], -400, -200), "", MP.MP_BASE_COLOR)
    _connect(_add_texture_sample(material, textures["pcm_normal"], -400, 100), "", MP.MP_NORMAL)
    _connect(_add_texture_sample(material, textures["pcm_rough"], -400, 350), "", MP.MP_ROUGHNESS)
    _connect(_add_texture_sample(material, textures["pcm_metallic"], -400, 550), "", MP.MP_METALLIC)
    _connect(_add_texture_sample(material, textures["pcm_ao"], -400, 750), "", MP.MP_AMBIENT_OCCLUSION)

    unreal.MaterialEditingLibrary.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(MAT_PATH)
    unreal.log(f"[apply_robot_material] material built: {MAT_PATH}")
    return material


def stage_apply_to_meshes(material: unreal.Material) -> int:
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    registry.scan_paths_synchronous([MESH_PACKAGE], force_rescan=True)
    touched: int = 0
    skipped: list[str] = []
    for asset_data in registry.get_assets_by_path(MESH_PACKAGE, recursive=True):
        asset = asset_data.get_asset()
        if not isinstance(asset, unreal.StaticMesh):
            continue
        slot_count = len(asset.get_editor_property("static_materials"))
        if slot_count == 0:
            skipped.append(str(asset_data.asset_name))
            continue
        for slot in range(slot_count):
            asset.set_material(slot, material)
        unreal.EditorAssetLibrary.save_asset(asset.get_path_name())
        touched += 1
    unreal.log(
        f"[apply_robot_material] repointed {touched} StaticMesh asset(s) "
        f"to {MAT_PATH}"
        + (f"; skipped (no slots): {skipped}" if skipped else "")
    )
    return touched


def main() -> None:
    textures = stage_import_pbr()
    material = stage_build_material(textures)
    stage_apply_to_meshes(material)
    unreal.log("[apply_robot_material] done")


if __name__ == "__main__":
    main()
