#!/usr/bin/env python3
"""Apply the project-wide overcast lighting defaults to the soccer field.

Run inside Unreal Editor:

    UnrealEditor-Cmd URSoccerLab.uproject -run=PythonScript \
      -Script=Tools/editor/tune_environment_lighting.py -NullRHI -unattended
"""
from __future__ import annotations

import json
from pathlib import Path

import unreal


ROOT = Path(__file__).resolve().parents[2]
LEVEL_PATH = "/Game/Levels/URS_SoccerField"
REPORT_PATH = ROOT / "Saved/Diagnostics/environment_lighting.json"

# Dark, drizzly daylight: direct sun is heavily suppressed so the indoor lamps
# remain visible against the real-time Sky Light's diffuse overcast fill.
SUN_INTENSITY_LUX = 50.0
SUN_SOURCE_ANGLE_DEGREES = 5.0
FOG_ACTOR_LABEL = "URS_OvercastFog"
FOG_DENSITY = 0.006
FOG_HEIGHT_FALLOFF = 0.2
FOG_MAX_OPACITY = 0.65
FOG_START_DISTANCE_CM = 500.0
FOG_COLOR = unreal.LinearColor(0.65, 0.70, 0.75, 1.0)
CLOUD_ACTOR_LABEL = "URS_OvercastCloud"
CLOUD_PARENT_OBJECT_PATH = (
    "/Engine/EngineSky/VolumetricClouds/"
    "m_SimpleVolumetricCloud_Inst.m_SimpleVolumetricCloud_Inst"
)
CLOUD_MATERIAL_DIR = "/Game/URSoccerLab/Scenes/SoccerField/Lighting"
CLOUD_MATERIAL_PATH = f"{CLOUD_MATERIAL_DIR}/MI_URS_OvercastCloud"
CLOUD_COVERAGE = 0.65
CLOUD_DENSITY = 0.02
CLOUD_LAYER_BOTTOM_KM = 0.8
CLOUD_LAYER_HEIGHT_KM = 1.5


def load_or_create_cloud_material() -> unreal.MaterialInstanceConstant:
    if unreal.EditorAssetLibrary.does_asset_exist(CLOUD_MATERIAL_PATH):
        material = unreal.EditorAssetLibrary.load_asset(CLOUD_MATERIAL_PATH)
    else:
        parent = unreal.load_object(None, CLOUD_PARENT_OBJECT_PATH)
        if not parent:
            raise RuntimeError(
                f"could not load cloud parent {CLOUD_PARENT_OBJECT_PATH}"
            )
        asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
        material = asset_tools.create_asset(
            "MI_URS_OvercastCloud",
            CLOUD_MATERIAL_DIR,
            unreal.MaterialInstanceConstant,
            unreal.MaterialInstanceConstantFactoryNew(),
        )
        if not material:
            raise RuntimeError(f"could not create {CLOUD_MATERIAL_PATH}")
        material.set_editor_property("parent", parent)

    unreal.MaterialEditingLibrary.set_material_instance_scalar_parameter_value(
        material, "Cloud_GlobalCoverage", CLOUD_COVERAGE
    )
    unreal.MaterialEditingLibrary.set_material_instance_scalar_parameter_value(
        material, "Cloud_GlobalDensity", CLOUD_DENSITY
    )
    unreal.MaterialEditingLibrary.set_material_instance_vector_parameter_value(
        material,
        "Cloud_AlbedoColor",
        unreal.LinearColor(0.78, 0.81, 0.84, 0.75),
    )
    if not unreal.EditorAssetLibrary.save_loaded_asset(
        material, only_if_is_dirty=False
    ):
        raise RuntimeError(f"could not save {CLOUD_MATERIAL_PATH}")
    return material


def main() -> None:
    if not unreal.EditorLoadingAndSavingUtils.load_map(LEVEL_PATH):
        raise RuntimeError(f"could not load {LEVEL_PATH}")

    editor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    atmospheric_suns: list[tuple[unreal.Actor, unreal.DirectionalLightComponent]] = []
    skylights: list[tuple[unreal.Actor, unreal.SkyLightComponent]] = []
    atmosphere_components: list[unreal.SkyAtmosphereComponent] = []
    overcast_fog: unreal.ExponentialHeightFog | None = None
    overcast_cloud: unreal.VolumetricCloud | None = None

    for actor in editor.get_all_level_actors():
        for component in actor.get_components_by_class(
            unreal.DirectionalLightComponent
        ):
            if component.get_editor_property("atmosphere_sun_light"):
                atmospheric_suns.append((actor, component))
        for component in actor.get_components_by_class(unreal.SkyLightComponent):
            skylights.append((actor, component))
        atmosphere_components.extend(
            actor.get_components_by_class(unreal.SkyAtmosphereComponent)
        )
        if (
            isinstance(actor, unreal.ExponentialHeightFog)
            and actor.get_actor_label() == FOG_ACTOR_LABEL
        ):
            overcast_fog = actor
        if (
            isinstance(actor, unreal.VolumetricCloud)
            and actor.get_actor_label() == CLOUD_ACTOR_LABEL
        ):
            overcast_cloud = actor

    if len(atmospheric_suns) != 1:
        labels = [actor.get_actor_label() for actor, _ in atmospheric_suns]
        raise RuntimeError(
            f"expected exactly one atmospheric sun, found {len(labels)}: {labels}"
        )

    sun_actor, sun = atmospheric_suns[0]
    old_intensity = float(sun.get_editor_property("intensity"))
    old_source_angle = float(sun.get_editor_property("light_source_angle"))
    # Use the editor property rather than the runtime setter so the component
    # transaction marks its owning level package dirty and serializes the value.
    sun.set_editor_property("intensity", SUN_INTENSITY_LUX)
    sun.set_editor_property("light_source_angle", SUN_SOURCE_ANGLE_DEGREES)
    sun.set_editor_property("cast_cloud_shadows", True)
    sun.set_editor_property("cloud_shadow_strength", 1.0)
    sun.set_editor_property("cloud_shadow_on_atmosphere_strength", 0.8)
    sun.set_editor_property("cloud_shadow_on_surface_strength", 1.0)

    if overcast_fog is None:
        spawned = editor.spawn_actor_from_class(
            unreal.ExponentialHeightFog,
            unreal.Vector(0.0, 0.0, 0.0),
            unreal.Rotator(0.0, 0.0, 0.0),
        )
        if not spawned:
            raise RuntimeError("could not spawn overcast ExponentialHeightFog")
        overcast_fog = spawned
        overcast_fog.set_actor_label(FOG_ACTOR_LABEL)
        overcast_fog.tags = [unreal.Name("URS_OvercastFog")]

    fog_components = overcast_fog.get_components_by_class(
        unreal.ExponentialHeightFogComponent
    )
    if len(fog_components) != 1:
        raise RuntimeError(
            f"expected one fog component on {FOG_ACTOR_LABEL}, "
            f"found {len(fog_components)}"
        )
    fog = fog_components[0]
    fog.set_editor_property("fog_density", FOG_DENSITY)
    fog.set_editor_property("fog_height_falloff", FOG_HEIGHT_FALLOFF)
    fog.set_editor_property("fog_max_opacity", FOG_MAX_OPACITY)
    fog.set_editor_property("start_distance", FOG_START_DISTANCE_CM)
    fog.set_editor_property("fog_inscattering_luminance", FOG_COLOR)
    fog.set_editor_property("enable_volumetric_fog", False)

    cloud_material = load_or_create_cloud_material()
    if overcast_cloud is None:
        spawned = editor.spawn_actor_from_class(
            unreal.VolumetricCloud,
            unreal.Vector(0.0, 0.0, 0.0),
            unreal.Rotator(0.0, 0.0, 0.0),
        )
        if not spawned:
            raise RuntimeError("could not spawn overcast VolumetricCloud")
        overcast_cloud = spawned
        overcast_cloud.set_actor_label(CLOUD_ACTOR_LABEL)
        overcast_cloud.tags = [unreal.Name("URS_OvercastCloud")]

    cloud_components = overcast_cloud.get_components_by_class(
        unreal.VolumetricCloudComponent
    )
    if len(cloud_components) != 1:
        raise RuntimeError(
            f"expected one cloud component on {CLOUD_ACTOR_LABEL}, "
            f"found {len(cloud_components)}"
        )
    cloud = cloud_components[0]
    cloud.set_editor_property("material", cloud_material)
    cloud.set_editor_property("layer_bottom_altitude", CLOUD_LAYER_BOTTOM_KM)
    cloud.set_editor_property("layer_height", CLOUD_LAYER_HEIGHT_KM)
    cloud.set_editor_property("sky_light_cloud_bottom_occlusion", 0.7)
    cloud.set_editor_property("view_sample_count_scale", 0.5)
    cloud.set_editor_property("reflection_view_sample_count_scale_value", 0.25)

    if not unreal.EditorLoadingAndSavingUtils.save_current_level():
        raise RuntimeError(f"could not save {LEVEL_PATH}")

    REPORT_PATH.parent.mkdir(parents=True, exist_ok=True)
    REPORT_PATH.write_text(
        json.dumps(
            {
                "level": LEVEL_PATH,
                "sun_actor": sun_actor.get_actor_label(),
                "old_sun_intensity_lux": old_intensity,
                "new_sun_intensity_lux": SUN_INTENSITY_LUX,
                "old_sun_source_angle_degrees": old_source_angle,
                "new_sun_source_angle_degrees": SUN_SOURCE_ANGLE_DEGREES,
                "sun_rotation": {
                    "pitch": float(sun_actor.get_actor_rotation().pitch),
                    "yaw": float(sun_actor.get_actor_rotation().yaw),
                    "roll": float(sun_actor.get_actor_rotation().roll),
                },
                "skylights": [
                    {
                        "actor": actor.get_actor_label(),
                        "intensity": float(component.get_editor_property("intensity")),
                        "real_time_capture": bool(
                            component.get_editor_property("real_time_capture")
                        ),
                    }
                    for actor, component in skylights
                ],
                "fog": {
                    "actor": overcast_fog.get_actor_label(),
                    "density": FOG_DENSITY,
                    "height_falloff": FOG_HEIGHT_FALLOFF,
                    "max_opacity": FOG_MAX_OPACITY,
                    "start_distance_cm": FOG_START_DISTANCE_CM,
                    "color_linear": [
                        float(FOG_COLOR.r),
                        float(FOG_COLOR.g),
                        float(FOG_COLOR.b),
                    ],
                    "volumetric": False,
                },
                "cloud": {
                    "actor": overcast_cloud.get_actor_label(),
                    "material": CLOUD_MATERIAL_PATH,
                    "coverage": CLOUD_COVERAGE,
                    "density": CLOUD_DENSITY,
                    "layer_bottom_km": CLOUD_LAYER_BOTTOM_KM,
                    "layer_height_km": CLOUD_LAYER_HEIGHT_KM,
                    "casts_surface_shadows": True,
                },
            },
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )
    unreal.log(
        "[tune_environment_lighting] "
        f"{sun_actor.get_actor_label()}: {old_intensity:g} -> "
        f"{SUN_INTENSITY_LUX:g} lux, source angle "
        f"{old_source_angle:g} -> {SUN_SOURCE_ANGLE_DEGREES:g} degrees, "
        f"fog density {FOG_DENSITY:g}, cloud coverage {CLOUD_COVERAGE:g}; "
        f"saved {LEVEL_PATH}"
    )


main()
