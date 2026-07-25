#!/usr/bin/env python3
"""Import the floating-base pi_plus MJCF as a UE Blueprint.

Runs inside Unreal Editor via ``-ExecutePythonScript``. Produces
``/Game/MuJoCoImports/pi_plus_walk`` — a Blueprint with a free-joint base,
20 motor actuators, and a UE-renderable camera.
"""

from __future__ import annotations

import sys
from pathlib import Path

import unreal


ROOT = Path(sys.argv[0]).resolve().parents[1] if len(sys.argv) > 0 else Path.cwd()
XML_PATH = str(ROOT / "Assets" / "MosBrainCameraTest" / "pi_plus" / "pi_plus_walk.xml")
DEST_PATH = "/Game/MuJoCoImports"
DEST_NAME = "pi_plus_walk"
SUCCESS_MARKER = str(ROOT / "Saved" / "Logs" / "URS_ImportPiPlusWalk.done")


def main() -> int:
    if not Path(XML_PATH).exists():
        unreal.log_error(f"XML not found: {XML_PATH}")
        return 1

    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()

    # Destroy existing Blueprint so the factory doesn't refuse to overwrite.
    existing_path = f"{DEST_PATH}/{DEST_NAME}.{DEST_NAME}"
    if unreal.EditorAssetLibrary.does_asset_exist(existing_path):
        unreal.log(f"Destroying existing Blueprint: {existing_path}")
        unreal.EditorAssetLibrary.delete_asset(existing_path)

    import_data = unreal.AutomatedAssetImportData()
    import_data.set_editor_property("filenames", [XML_PATH])
    import_data.set_editor_property("destination_path", DEST_PATH)
    import_data.set_editor_property("replace_existing", True)
    import_data.set_editor_property("factory_name", "MujocoImportFactory")
    imported = asset_tools.import_assets_automated(import_data)

    if not imported:
        unreal.log_error("ImportAssetsAutomated returned no objects")
        return 1

    bp = imported[0]
    bp_name = bp.get_name()
    unreal.log(f"Imported Blueprint: {bp_name}")

    # Save the Blueprint package so it persists on disk.
    bp_package_path = f"{DEST_PATH}/{DEST_NAME}"
    unreal.EditorAssetLibrary.save_asset(bp_package_path)
    unreal.log(f"Saved: {bp_package_path}")

    # Also save the sub-asset directory.
    unreal.EditorAssetLibrary.save_directory(f"{DEST_PATH}/{DEST_NAME}_ue_Assets")
    unreal.log(f"Saved sub-assets: {DEST_PATH}/{DEST_NAME}_ue_Assets")

    Path(SUCCESS_MARKER).parent.mkdir(parents=True, exist_ok=True)
    Path(SUCCESS_MARKER).write_text(f"{DEST_PATH}/{DEST_NAME}\n")
    unreal.log(f"Success marker written: {SUCCESS_MARKER}")
    return 0


try:
    raise SystemExit(main())
except Exception as e:
    unreal.log_error(f"Import failed: {e}")
    import traceback
    traceback.print_exc()
    raise SystemExit(1)
