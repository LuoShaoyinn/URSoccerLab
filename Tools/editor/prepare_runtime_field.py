#!/usr/bin/env python3
"""Prepare runtime field assets and remove the baked pitch and goal meshes.

Run UnrealEditor-Cmd -run=pythonscript -script=Tools/editor/prepare_runtime_field.py.
This edits source Content assets only; it does not cook or package an AppImage.
"""
import unreal

BASE = '/Game/URSoccerLab/Scenes/SoccerField'
MATERIAL = BASE + '/Runtime/M_RuntimeField'
LEVEL = '/Game/Levels/URS_SoccerField'

def expression(material, kind, x, y):
    return unreal.MaterialEditingLibrary.create_material_expression(material, kind, x, y)

def connect(source, destination, input_name, output=''):
    unreal.MaterialEditingLibrary.connect_material_expressions(source, output, destination, input_name)

unreal.EditorAssetLibrary.make_directory(BASE + '/Runtime')
material = unreal.load_asset(MATERIAL)
if material is None:
    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        'M_RuntimeField', BASE + '/Runtime', unreal.Material, unreal.MaterialFactoryNew())
    world = expression(material, unreal.MaterialExpressionWorldPosition, -1000, 0)
    uv = []
    for index, (name, value, channel) in enumerate([
            ('FieldLengthCm', 1060.0, 'r'), ('FieldWidthCm', 780.0, 'g')]):
        mask = expression(material, unreal.MaterialExpressionComponentMask, -800, index*200)
        mask.set_editor_property(channel, True)
        connect(world, mask, 'Input')
        extent = expression(material, unreal.MaterialExpressionScalarParameter, -800, index*200+80)
        extent.set_editor_property('parameter_name', name)
        extent.set_editor_property('default_value', value)
        divide = expression(material, unreal.MaterialExpressionDivide, -600, index*200)
        connect(mask, divide, 'A')
        connect(extent, divide, 'B')
        offset = expression(material, unreal.MaterialExpressionAdd, -400, index*200)
        offset.set_editor_property('const_b', 0.5)
        connect(divide, offset, 'A')
        uv.append(offset)
    append = expression(material, unreal.MaterialExpressionAppendVector, -200, 0)
    connect(uv[0], append, 'A')
    connect(uv[1], append, 'B')
    texture = expression(material, unreal.MaterialExpressionTextureSampleParameter2D, 0, 0)
    texture.set_editor_property('parameter_name', 'FieldMap')
    texture.set_editor_property('texture', unreal.load_asset('/Engine/EngineResources/WhiteSquareTexture'))
    connect(append, texture, 'Coordinates')
    unreal.MaterialEditingLibrary.connect_material_property(texture, 'RGB', unreal.MaterialProperty.MP_BASE_COLOR)
    roughness = expression(material, unreal.MaterialExpressionConstant, 0, 200)
    roughness.set_editor_property('r', 0.9)
    unreal.MaterialEditingLibrary.connect_material_property(roughness, '', unreal.MaterialProperty.MP_ROUGHNESS)
    unreal.MaterialEditingLibrary.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(MATERIAL)

world = unreal.EditorLoadingAndSavingUtils.load_map(LEVEL)
for actor in unreal.GameplayStatics.get_all_actors_of_class(world, unreal.Actor):
    for component in actor.get_components_by_class(unreal.StaticMeshComponent):
        mesh = component.static_mesh
        if mesh and mesh.get_path_name().startswith(BASE + '/Field/StaticMeshes/'):
            # The pitch and both goals are generated from mandatory configuration.
            unreal.log('Removing baked field/goal actor: ' + actor.get_actor_label())
            unreal.get_editor_subsystem(unreal.EditorActorSubsystem).destroy_actor(actor)
            break
unreal.EditorLoadingAndSavingUtils.save_map(world, LEVEL)
# Remove obsolete assets only once their external references have been removed.
obsolete = [BASE+'/Field/StaticMeshes/Plane', BASE+'/Field/Materials/Field',
            BASE+'/Field/Materials/grass1-ue', BASE+'/Field/Textures/field']
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
