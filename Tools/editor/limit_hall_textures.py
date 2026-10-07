"""Audit hall textures; pass --apply to cap hall and skybox at 1024.

Run with UnrealEditor-Cmd -ExecutePythonScript="<script> [--apply]" -NullRHI.
Only MaxTextureSize changes: source images remain intact and limits can be undone.
External field, ball and robot textures are unaffected.
"""
import argparse
import json
from pathlib import Path
import unreal

ROOT = Path(__file__).resolve().parents[2]
BASE = '/Game/URSoccerLab/Scenes/SoccerField'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    report = []
    for folder, limit in [('Environment/Textures', 1024), ('Skybox', 1024)]:
        for path in unreal.EditorAssetLibrary.list_assets(BASE+'/'+folder, recursive=True):
            texture = unreal.load_asset(path)
            if not isinstance(texture, unreal.Texture2D):
                continue
            before = texture.get_editor_property('max_texture_size')
            width, height = texture.blueprint_get_size_x(), texture.blueprint_get_size_y()
            changed = args.apply and before != limit and (before > 0 or max(width, height) > limit)
            if changed:
                texture.set_editor_property('max_texture_size', limit)
                unreal.EditorAssetLibrary.save_loaded_asset(texture)
            report.append({'asset': path, 'width': width, 'height': height,
                           'previous_max_texture_size': before,
                           'max_texture_size': limit if changed else before,
                           'proposed_limit': limit})
    output = ROOT/'artifacts/tests/hall-textures'
    output.mkdir(parents=True, exist_ok=True)
    (output/('limits.json' if args.apply else 'audit.json')).write_text(json.dumps(report, indent=2)+'\n')
    unreal.log('[hall-textures] '+json.dumps(report))


main()
