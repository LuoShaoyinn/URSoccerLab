#!/usr/bin/env python3
"""Convert imported emissive cylinder meshes into real Unreal area lights.

The GLB importer preserves emissive materials, but emissive surfaces alone do
not provide practical direct lighting. This script finds imported mesh actors
whose material has a non-zero ``EmissiveFactor``, welds split render vertices,
and places one movable, omnidirectional Point Light per disconnected physical
mesh volume.

Run inside Unreal Editor:

    UnrealEditor-Cmd URSoccerLab.uproject -run=PythonScript \
      -Script=Tools/editor/convert_emissive_lamps.py -NullRHI -unattended

The operation is idempotent: previously generated actors are removed first.
"""
from __future__ import annotations

import json
from pathlib import Path

import unreal


ROOT = Path(__file__).resolve().parents[2]
LEVEL_PATH = "/Game/Levels/URS_SoccerField"
ASSET_PATH_PREFIX = "/Game/URSoccerLab/Scenes/SoccerField/Environment"
GENERATED_LABEL_PREFIX = "URS_AutoEmissiveLamp_"
GENERATED_TAG = "URS_AutoEmissiveLamp"
GENERATED_FOLDER = "URS/GeneratedLights/EmissiveLamps"
REPORT_PATH = ROOT / "Saved/Diagnostics/emissive_lights.json"

# These are broad indoor-area-light defaults. They can be tuned in one place
# and the script rerun without accumulating duplicate actors.
INTENSITY_LUMENS = 280.0
EMISSIVE_STRENGTH = 1.0
EMISSIVE_FACTOR_LEVEL = 10.0
ATTENUATION_RADIUS_CM = 900.0
SOURCE_RADIUS_CM = 14.0
MIN_VOLUME_EXTENT_CM = 2.0
MAX_VOLUME_EXTENT_CM = 500.0
CAST_SHADOWS = True


def object_path(value: unreal.Object | None) -> str:
    return value.get_path_name() if value else ""


def as_list(value: unreal.Vector) -> list[float]:
    return [float(value.x), float(value.y), float(value.z)]


def parameter_color(
    material: unreal.MaterialInterface,
) -> tuple[float, float, float] | None:
    """Return a visible GLTF emissive color, or None for non-emissive material."""
    vector_names = {
        str(name)
        for name in unreal.MaterialEditingLibrary.get_vector_parameter_names(material)
    }
    if "EmissiveFactor" not in vector_names:
        return None

    factor = (
        unreal.MaterialEditingLibrary.get_material_instance_vector_parameter_value(
            material, "EmissiveFactor"
        )
    )
    scalar_names = {
        str(name)
        for name in unreal.MaterialEditingLibrary.get_scalar_parameter_names(material)
    }
    strength = 1.0
    if "EmissiveStrength" in scalar_names:
        strength = float(
            unreal.MaterialEditingLibrary.get_material_instance_scalar_parameter_value(
                material, "EmissiveStrength"
            )
        )
    rgb = (
        max(0.0, float(factor.r) * strength),
        max(0.0, float(factor.g) * strength),
        max(0.0, float(factor.b) * strength),
    )
    if max(rgb) <= 1.0e-4:
        return None

    # Light color is normalized; physical output is controlled in lumens.
    peak = max(rgb)
    return tuple(channel / peak for channel in rgb)


def set_emissive_strength(
    material: unreal.MaterialInterface,
) -> tuple[float, float] | None:
    """Set a restrained visible glow while analytic lights provide illumination."""
    scalar_names = {
        str(name)
        for name in unreal.MaterialEditingLibrary.get_scalar_parameter_names(material)
    }
    if "EmissiveStrength" not in scalar_names:
        return None

    old_strength = float(
        unreal.MaterialEditingLibrary.get_material_instance_scalar_parameter_value(
            material, "EmissiveStrength"
        )
    )
    unreal.MaterialEditingLibrary.set_material_instance_scalar_parameter_value(
        material, "EmissiveStrength", EMISSIVE_STRENGTH
    )
    vector_names = {
        str(name)
        for name in unreal.MaterialEditingLibrary.get_vector_parameter_names(material)
    }
    if "EmissiveFactor" in vector_names:
        factor = (
            unreal.MaterialEditingLibrary.get_material_instance_vector_parameter_value(
                material, "EmissiveFactor"
            )
        )
        peak = max(float(factor.r), float(factor.g), float(factor.b), 1.0e-6)
        unreal.MaterialEditingLibrary.set_material_instance_vector_parameter_value(
            material,
            "EmissiveFactor",
            unreal.LinearColor(
                float(factor.r) / peak * EMISSIVE_FACTOR_LEVEL,
                float(factor.g) / peak * EMISSIVE_FACTOR_LEVEL,
                float(factor.b) / peak * EMISSIVE_FACTOR_LEVEL,
                1.0,
            ),
        )
    # A luminous diffuser should not mirror the bright rectangular windows.
    # The imported glTF default is fully specular, which leaves white bars on
    # the otherwise uniform emitting surface.
    if "SpecularFactor" in scalar_names:
        unreal.MaterialEditingLibrary.set_material_instance_scalar_parameter_value(
            material, "SpecularFactor", 0.0
        )
    base_overrides = material.get_editor_property("base_property_overrides")
    base_overrides.set_editor_property("override_shading_model", True)
    base_overrides.set_editor_property(
        "shading_model", unreal.MaterialShadingModel.MSM_UNLIT
    )
    material.set_editor_property("base_property_overrides", base_overrides)
    if not unreal.EditorAssetLibrary.save_loaded_asset(
        material, only_if_is_dirty=False
    ):
        raise RuntimeError(f"could not save emissive material {object_path(material)}")
    return old_strength, EMISSIVE_STRENGTH


class DisjointSet:
    def __init__(self, size: int) -> None:
        self.parent = list(range(size))

    def find(self, value: int) -> int:
        while self.parent[value] != value:
            self.parent[value] = self.parent[self.parent[value]]
            value = self.parent[value]
        return value

    def union(self, left: int, right: int) -> None:
        left_root = self.find(left)
        right_root = self.find(right)
        if left_root != right_root:
            self.parent[right_root] = left_root


def triangle_vertex_indices(
    description: unreal.StaticMeshDescription,
    triangle_index: int,
    vertex_count: int,
) -> list[int]:
    # UE 5.7's generated binding currently prepends three unused entries to
    # this fixed-size return array, so retain the final three valid IDs.
    raw = description.get_triangle_vertices(unreal.TriangleID(triangle_index))
    indices = [
        int(value.get_editor_property("id_value"))
        for value in raw
        if 0 <= int(value.get_editor_property("id_value")) < vertex_count
    ]
    indices = indices[-3:]
    if len(indices) != 3:
        raise RuntimeError(
            f"triangle {triangle_index} returned invalid vertex IDs: {raw}"
        )
    return indices


def physical_mesh_volumes(mesh: unreal.StaticMesh) -> list[list[unreal.Vector]]:
    """Return disconnected volumes, welding render vertices split at hard edges."""
    description = mesh.get_static_mesh_description(0)
    if not description:
        raise RuntimeError(f"{object_path(mesh)} has no LOD 0 mesh description")

    vertex_count = description.get_vertex_count()
    positions = [
        description.get_vertex_position(unreal.VertexID(index))
        for index in range(vertex_count)
    ]
    sets = DisjointSet(vertex_count)

    for triangle_index in range(description.get_triangle_count()):
        first, second, third = triangle_vertex_indices(
            description, triangle_index, vertex_count
        )
        sets.union(first, second)
        sets.union(second, third)

    # GLB import duplicates cap/side vertices at hard normals. Spatial welding
    # reconnects those pieces into one physical cylinder.
    coincident: dict[tuple[int, int, int], int] = {}
    weld_tolerance = 0.01
    for index, position in enumerate(positions):
        key = (
            round(float(position.x) / weld_tolerance),
            round(float(position.y) / weld_tolerance),
            round(float(position.z) / weld_tolerance),
        )
        previous = coincident.get(key)
        if previous is None:
            coincident[key] = index
        else:
            sets.union(previous, index)

    volumes: dict[int, list[unreal.Vector]] = {}
    for index, position in enumerate(positions):
        volumes.setdefault(sets.find(index), []).append(position)
    return [points for points in volumes.values() if len(points) >= 8]


def world_bounds(
    points: list[unreal.Vector], transform: unreal.Transform
) -> tuple[unreal.Vector, unreal.Vector, list[float]]:
    transformed = [transform.transform_location(point) for point in points]
    low = unreal.Vector(
        min(point.x for point in transformed),
        min(point.y for point in transformed),
        min(point.z for point in transformed),
    )
    high = unreal.Vector(
        max(point.x for point in transformed),
        max(point.y for point in transformed),
        max(point.z for point in transformed),
    )
    center = unreal.Vector(
        (low.x + high.x) * 0.5,
        (low.y + high.y) * 0.5,
        (low.z + high.z) * 0.5,
    )
    sizes = [
        float(high.x - low.x),
        float(high.y - low.y),
        float(high.z - low.z),
    ]
    return center, low, sizes


def destroy_generated_lights(editor: unreal.EditorActorSubsystem) -> int:
    removed = 0
    for actor in editor.get_all_level_actors():
        tags = {str(tag) for tag in actor.tags}
        if (
            actor.get_actor_label().startswith(GENERATED_LABEL_PREFIX)
            or GENERATED_TAG in tags
        ):
            if editor.destroy_actor(actor):
                removed += 1
    return removed


def point_light_component(actor: unreal.Actor) -> unreal.PointLightComponent:
    components = actor.get_components_by_class(unreal.PointLightComponent)
    if len(components) != 1:
        raise RuntimeError(
            f"expected one PointLightComponent on {actor.get_actor_label()}"
        )
    return components[0]


def configure_light(
    component: unreal.PointLightComponent,
    color: tuple[float, float, float],
) -> None:
    component.set_mobility(unreal.ComponentMobility.MOVABLE)
    component.set_intensity_units(unreal.LightUnits.LUMENS)
    component.set_intensity(INTENSITY_LUMENS)
    component.set_attenuation_radius(ATTENUATION_RADIUS_CM)
    component.set_light_color(
        unreal.LinearColor(color[0], color[1], color[2], 1.0), True
    )
    component.set_source_radius(SOURCE_RADIUS_CM)
    # The visible mesh supplies the fixture's appearance. Prevent the hidden
    # analytic approximation from producing point/sphere-shaped highlights.
    component.set_specular_scale(0.0)
    component.set_cast_shadows(CAST_SHADOWS)
    component.set_indirect_lighting_intensity(1.0)
    component.set_volumetric_scattering_intensity(1.0)


def main() -> None:
    if not unreal.EditorLoadingAndSavingUtils.load_map(LEVEL_PATH):
        raise RuntimeError(f"could not load field level {LEVEL_PATH}")

    editor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    removed = destroy_generated_lights(editor)
    generated: list[dict] = []
    candidate_components = 0
    updated_materials: dict[str, dict[str, float | str]] = {}

    for source_actor in list(editor.get_all_level_actors()):
        for mesh_component in source_actor.get_components_by_class(
            unreal.StaticMeshComponent
        ):
            mesh = mesh_component.get_editor_property("static_mesh")
            if not mesh or not object_path(mesh).startswith(ASSET_PATH_PREFIX):
                continue

            material_colors = []
            non_emissive_materials = 0
            for material_index in range(mesh_component.get_num_materials()):
                material = mesh_component.get_material(material_index)
                color = parameter_color(material) if material else None
                if color is None:
                    non_emissive_materials += 1
                else:
                    material_colors.append((material, color))
            if not material_colors:
                continue
            candidate_components += 1
            if non_emissive_materials:
                unreal.log_warning(
                    "[convert_emissive_lamps] skipping mixed-material mesh "
                    f"{object_path(mesh)}; split emissive geometry into its own mesh"
                )
                continue

            for material, _ in material_colors:
                material_path = object_path(material)
                if material_path not in updated_materials:
                    strengths = set_emissive_strength(material)
                    if strengths is not None:
                        old_strength, new_strength = strengths
                        updated_materials[material_path] = {
                            "material": material_path,
                            "old_emissive_strength": old_strength,
                            "new_emissive_strength": new_strength,
                        }

            # The imported luminous cylinder is an opaque shell. Letting it cast
            # shadows would trap the centered analytic light inside itself.
            # Other scene geometry, including the hanging string, still casts
            # normal shadows because the Point Light retains shadowing.
            mesh_component.set_cast_shadow(False)
            # The analytic Point Light owns the physical illumination. Keep
            # high camera-visible emission from becoming an extra Lumen source.
            mesh_component.set_emissive_light_source(False)
            mesh_component.set_affect_dynamic_indirect_lighting(False)
            # Keep the visible emitter purely emissive. Otherwise its centered
            # analytic Point Light illuminates the opaque lamp shell itself,
            # producing a misleading bright dot instead of an even glow.
            mesh_component.set_editor_property(
                "lighting_channels",
                unreal.LightingChannels(
                    channel0=False,
                    channel1=True,
                    channel2=False,
                ),
            )

            color = material_colors[0][1]
            actor_transform = source_actor.get_actor_transform()
            volumes = physical_mesh_volumes(mesh)
            volume_records = []
            for points in volumes:
                center, low, sizes = world_bounds(points, actor_transform)
                ordered = sorted(sizes)
                if (
                    ordered[0] < MIN_VOLUME_EXTENT_CM
                    or ordered[2] > MAX_VOLUME_EXTENT_CM
                ):
                    unreal.log_warning(
                        "[convert_emissive_lamps] skipping implausible emissive "
                        f"volume on {source_actor.get_actor_label()}: {sizes} cm"
                    )
                    continue
                volume_records.append((center, low, sizes))

            volume_records.sort(
                key=lambda record: (
                    float(record[0].x),
                    float(record[0].y),
                    float(record[0].z),
                )
            )
            for volume_index, (center, low, sizes) in enumerate(volume_records):
                # These fixtures are luminous on their top, bottom, and sides.
                # A centered Point Light is the stable one-light approximation
                # for that omnidirectional emitting volume.
                location = center
                light = editor.spawn_actor_from_class(
                    unreal.PointLight,
                    location,
                    unreal.Rotator(0.0, 0.0, 0.0),
                )
                if not light:
                    raise RuntimeError(f"failed to spawn light at {location}")
                label = (
                    f"{GENERATED_LABEL_PREFIX}"
                    f"{source_actor.get_actor_label()}_{volume_index:03d}"
                )
                light.set_actor_label(label)
                light.tags = [unreal.Name(GENERATED_TAG)]
                light.set_folder_path(GENERATED_FOLDER)
                configure_light(point_light_component(light), color)
                generated.append(
                    {
                        "label": label,
                        "light_type": "point",
                        "source_actor": source_actor.get_actor_label(),
                        "source_mesh": object_path(mesh),
                        "source_material": object_path(material_colors[0][0]),
                        "location_cm": as_list(location),
                        "volume_center_cm": as_list(center),
                        "volume_size_cm": sizes,
                        "source_radius_cm": SOURCE_RADIUS_CM,
                        "color_linear": list(color),
                        "intensity_lumens": INTENSITY_LUMENS,
                        "attenuation_radius_cm": ATTENUATION_RADIUS_CM,
                        "cast_shadows": CAST_SHADOWS,
                    }
                )

    if candidate_components == 0:
        raise RuntimeError(
            f"no emissive imported mesh components found below {ASSET_PATH_PREFIX}"
        )
    if not generated:
        raise RuntimeError("emissive meshes were found, but no light volumes were valid")

    if not unreal.EditorLoadingAndSavingUtils.save_current_level():
        raise RuntimeError(f"could not save {LEVEL_PATH}")

    REPORT_PATH.parent.mkdir(parents=True, exist_ok=True)
    REPORT_PATH.write_text(
        json.dumps(
            {
                "level": LEVEL_PATH,
                "removed_previous_lights": removed,
                "candidate_mesh_components": candidate_components,
                "generated_light_count": len(generated),
                "updated_materials": list(updated_materials.values()),
                "lights": generated,
            },
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )
    unreal.log(
        "[convert_emissive_lamps] "
        f"replaced {removed} old actors with {len(generated)} lights; "
        f"saved {LEVEL_PATH}"
    )


if __name__ == "__main__":
    main()
