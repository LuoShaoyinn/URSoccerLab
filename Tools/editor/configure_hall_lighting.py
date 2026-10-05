#!/usr/bin/env python3
"""Save the indoor hall lighting preset without changing meshes or materials.

Run in UnrealEditor-Cmd with -ExecutePythonScript=<absolute script path> -NullRHI.
"""
import json
from pathlib import Path
import unreal

ROOT = Path(__file__).resolve().parents[2]
LEVEL_PATH = '/Game/Levels/URS_SoccerField'
LUMENS = 224.0
SOURCE_RADIUS_CM = 60.0
SPECULAR_SCALE = 0.1
EXPOSURE_COMPENSATION = 2.5


def main():
    if not unreal.EditorLoadingAndSavingUtils.load_map(LEVEL_PATH):
        raise RuntimeError('Could not load hall')
    editor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    lights = []
    volumes = []
    for actor in editor.get_all_level_actors():
        if 'URS_AutoEmissiveLamp' in [str(tag) for tag in actor.tags]:
            for light in actor.get_components_by_class(unreal.PointLightComponent):
                light.set_intensity_units(unreal.LightUnits.LUMENS)
                light.set_intensity(LUMENS)
                light.set_source_radius(SOURCE_RADIUS_CM)
                light.set_specular_scale(SPECULAR_SCALE)
                lights.append(light.get_path_name())
        if isinstance(actor, unreal.PostProcessVolume) and actor.get_actor_label() == 'URS_GlobalPostProcess':
            settings = actor.get_editor_property('settings')
            settings.set_editor_property('override_auto_exposure_method', True)
            settings.set_editor_property('auto_exposure_method', unreal.AutoExposureMethod.AEM_MANUAL)
            settings.set_editor_property('override_auto_exposure_bias', True)
            settings.set_editor_property('auto_exposure_bias', EXPOSURE_COMPENSATION)
            settings.set_editor_property('override_auto_exposure_apply_physical_camera_exposure', True)
            settings.set_editor_property('auto_exposure_apply_physical_camera_exposure', False)
            actor.set_editor_property('settings', settings)
            volumes.append(actor.get_path_name())
    if len(lights) != 21 or len(volumes) != 1:
        raise RuntimeError(f'Expected 21 hall lamps and one global volume, found {len(lights)}/{len(volumes)}')
    if not unreal.EditorLoadingAndSavingUtils.save_current_level():
        raise RuntimeError('Could not save hall')
    report = ROOT / 'artifacts/diagnostics/hall_lighting.json'
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(json.dumps(dict(lumens=LUMENS, source_radius_cm=SOURCE_RADIUS_CM,
        specular_scale=SPECULAR_SCALE, exposure_compensation=EXPOSURE_COMPENSATION,
        lights=lights, post_process_volumes=volumes), indent=2)+'\n')
    unreal.log('Saved indoor hall lighting preset')


if __name__ == '__main__':
    main()
