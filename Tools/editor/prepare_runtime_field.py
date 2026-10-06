#!/usr/bin/env python3
"""Prepare runtime field assets and remove the baked pitch and goal meshes.

Run UnrealEditor-Cmd -run=pythonscript -script=Tools/editor/prepare_runtime_field.py.
This edits source Content assets only; it does not cook or package an AppImage.
"""
import unreal

BASE = '/Game/URSoccerLab/Scenes/SoccerField'
MATERIAL = BASE + '/Runtime/MI_RuntimeField'
MESH = BASE + '/Runtime/SM_RuntimeField'
LEVEL = '/Game/Levels/URS_SoccerField'

unreal.EditorAssetLibrary.make_directory(BASE + '/Runtime')
material = unreal.load_asset(MATERIAL)
if material is None:
    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        'MI_RuntimeField', BASE + '/Runtime', unreal.MaterialInstanceConstant,
        unreal.MaterialInstanceConstantFactoryNew())
# Match the original field's glTF parent, including its two-sided override and
# UV/PBR inputs. Never assign a built-in pitch image to this generic instance.
parent = unreal.load_asset('/InterchangeAssets/gltf/MaterialInstances/MI_Default_Opaque_DS')
unreal.MaterialEditingLibrary.set_material_instance_parent(material, parent)
unreal.MaterialEditingLibrary.set_material_instance_texture_parameter_value(
    material, 'BaseColorTexture', unreal.load_asset('/Engine/EngineResources/WhiteSquareTexture'))
unreal.MaterialEditingLibrary.set_material_instance_scalar_parameter_value(material, 'MetallicFactor', 0.0)
# Keep every supported PBR path in the compiled template. Runtime dynamic
# instances bind external textures and neutral defaults without shader changes.
for parameter in ['bHasBaseColorTexture', 'bHasNormalTexture',
                  'bHasMetallicRoughnessTexture', 'bHasOcclusionTexture']:
    unreal.MaterialEditingLibrary.set_material_instance_static_switch_parameter_value(
        material, parameter, True)
unreal.EditorAssetLibrary.save_loaded_asset(material)
mesh = unreal.load_asset(MESH)
if mesh is None:
    raise RuntimeError('Missing generic Nanite field mesh: ' + MESH)
# This mesh preserves the original plane geometry, UVs, and Nanite settings.
# Replace its material slot so it cannot retain any built-in pitch dependency.
mesh.set_material(0, material)
unreal.EditorAssetLibrary.save_loaded_asset(mesh)

world = unreal.EditorLoadingAndSavingUtils.load_map(LEVEL)
removed_actor = False
for actor in unreal.GameplayStatics.get_all_actors_of_class(world, unreal.Actor):
    for component in actor.get_components_by_class(unreal.StaticMeshComponent):
        mesh = component.static_mesh
        if mesh and mesh.get_path_name().startswith(BASE + '/Field/StaticMeshes/'):
            # The pitch and both goals are generated from mandatory configuration.
            unreal.log('Removing baked field/goal actor: ' + actor.get_actor_label())
            unreal.get_editor_subsystem(unreal.EditorActorSubsystem).destroy_actor(actor)
            removed_actor = True
            break
if removed_actor:
    unreal.EditorLoadingAndSavingUtils.save_map(world, LEVEL)
# Remove obsolete assets only once their external references have been removed.
obsolete = [BASE+'/Field/StaticMeshes/Plane', BASE+'/Field/Materials/Field',
            BASE+'/Field/Materials/grass1-ue', BASE+'/Field/Textures/field',
            BASE+'/Runtime/M_RuntimeField']
obsolete += [BASE+'/Field/StaticMeshes/'+name for name in ['goal_0','goal_00','goal_01','goal_1','goal_10','goal_11']]
for path in obsolete:
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        refs = unreal.EditorAssetLibrary.find_package_referencers_for_asset(path, True)
        unexpected = [ref for ref in refs if ref not in obsolete and ref != LEVEL]
        if unexpected:
            raise RuntimeError(f'Unexpected references to {path}: {unexpected}')
        if not unreal.EditorAssetLibrary.delete_asset(path):
            raise RuntimeError('Could not remove obsolete asset: '+path)
unreal.log('URS_RUNTIME_FIELD: material prepared; baked pitch/image/goals removed')
