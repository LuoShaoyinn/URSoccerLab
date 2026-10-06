#!/usr/bin/env python3
"""Split the imported lamp mesh into native static meshes for Lumen lighting.

Editor-only: run with -EnablePlugins=GeometryScripting -ExecutePythonScript=...
-RenderOffscreen -unattended (actor placement requires an active RHI).
Keeps the original mesh asset; bakes its transform into recentered lamp meshes.
"""
import json
from pathlib import Path
import unreal

ROOT = Path(__file__).resolve().parents[2]
FOLDER = '/Game/URSoccerLab/Scenes/SoccerField/Environment/EmissiveLamps'
TAG = 'URS_EmissiveLampSurface'


def main():
    assert unreal.EditorLoadingAndSavingUtils.load_map('/Game/Levels/URS_SoccerField')
    editor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    sources = [a for a in editor.get_all_level_actors() if 'vr_node_19' in a.get_path_name()]
    assert len(sources) == 1, sources
    source = sources[0]
    original = source.get_component_by_class(unreal.StaticMeshComponent)
    mesh = unreal.DynamicMesh()
    mesh, outcome = unreal.GeometryScript_AssetUtils.copy_mesh_from_static_mesh_v2(
        original.static_mesh, mesh, unreal.GeometryScriptCopyMeshFromAssetOptions(),
        unreal.GeometryScriptMeshReadLOD())
    assert outcome == unreal.GeometryScriptOutcomePins.SUCCESS, outcome
    _, parts = unreal.GeometryScript_MeshDecomposition.split_mesh_by_vertex_overlap(mesh, None, 0.01)
    assert len(parts) == 21, len(parts)
    for actor in editor.get_all_level_actors():
        if TAG in [str(t) for t in actor.tags]:
            editor.destroy_actor(actor)
    if unreal.EditorAssetLibrary.does_directory_exist(FOLDER):
        assert unreal.EditorAssetLibrary.delete_directory(FOLDER)
    report = []
    transform = original.get_world_transform()
    for index, part in enumerate(parts):
        unreal.GeometryScript_MeshTransforms.transform_mesh(part, transform)
        bounds = unreal.GeometryScript_MeshQueries.get_mesh_bounding_box(part)
        center = unreal.Vector((bounds.min.x + bounds.max.x) * 0.5,
            (bounds.min.y + bounds.max.y) * 0.5, (bounds.min.z + bounds.max.z) * 0.5)
        unreal.GeometryScript_MeshTransforms.translate_mesh(part, unreal.Vector(-center.x, -center.y, -center.z))
        asset, outcome = unreal.GeometryScript_NewAssetUtils.create_new_static_mesh_asset_from_mesh(
            part, f'{FOLDER}/Lamp_{index:02d}',
            unreal.GeometryScriptCreateNewStaticMeshAssetOptions(enable_collision=False))
        assert outcome == unreal.GeometryScriptOutcomePins.SUCCESS, outcome
        asset.set_material(0, original.get_material(0))
        assert unreal.EditorAssetLibrary.save_loaded_asset(asset, only_if_is_dirty=False)
        actor = editor.spawn_actor_from_class(unreal.StaticMeshActor, center)
        actor.set_actor_label(f'URS_EmissiveLampSurface_{index:02d}')
        actor.tags = [TAG]
        component = actor.get_component_by_class(unreal.StaticMeshComponent)
        component.set_static_mesh(asset)
        component.set_emissive_light_source(True)
        component.set_affect_dynamic_indirect_lighting(True)
        component.set_cast_shadow(True)
        component.set_editor_property('visible_in_ray_tracing', True)
        component.set_editor_property('lighting_channels', unreal.LightingChannels(channel0=True))
        actor.modify()
        report.append(dict(asset=asset.get_path_name(),center_cm=[center.x,center.y,center.z]))
    source.modify(); original.modify()
    original.set_visibility(False)
    original.set_hidden_in_game(True)
    original.set_affect_dynamic_indirect_lighting(False)
    for actor in editor.get_all_level_actors():
        if 'URS_AutoEmissiveLamp' in [str(t) for t in actor.tags]:
            for light in actor.get_components_by_class(unreal.PointLightComponent):
                actor.modify(); light.modify(); light.set_intensity(0)
    assert unreal.EditorLoadingAndSavingUtils.save_current_level()
    output = ROOT/'artifacts/tests/lumen-split-lamps'
    output.mkdir(parents=True, exist_ok=True)
    (output/'setup.json').write_text(json.dumps(report,indent=2)+'\n')
    unreal.log(f'Split {len(parts)} emissive lamps')


if __name__ == '__main__':
    main()
