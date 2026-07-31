#!/usr/bin/env python3
"""Remove the outdoor lighting stack from the production soccer-field level.

Run inside Unreal Editor:

    UnrealEditor-Cmd URSoccerLab.uproject \
      -ExecutePythonScript=Tools/editor/configure_indoor_production.py \
      -Unattended -NoSplash -DDC-ForceMemoryCache

The indoor Rect Lights, emissive lamp materials, and other scene actors are
left untouched.  The removed actors are recorded under Saved/Diagnostics so a
binary level edit can be reviewed without inspecting the umap directly.
"""
from __future__ import annotations

import json
from pathlib import Path

import unreal


ROOT = Path(__file__).resolve().parents[2]
LEVEL_PATH = "/Game/Levels/URS_SoccerField"
REPORT_PATH = ROOT / "Saved/Diagnostics/indoor_production.json"

OUTDOOR_COMPONENT_TYPES = (
    unreal.DirectionalLightComponent,
    unreal.SkyLightComponent,
    unreal.SkyAtmosphereComponent,
    unreal.VolumetricCloudComponent,
    unreal.ExponentialHeightFogComponent,
)


def is_outdoor_actor(actor: unreal.Actor) -> bool:
    return any(
        actor.get_components_by_class(component_type)
        for component_type in OUTDOOR_COMPONENT_TYPES
    )


def main() -> None:
    if not unreal.EditorLoadingAndSavingUtils.load_map(LEVEL_PATH):
        raise RuntimeError(f"could not load {LEVEL_PATH}")

    editor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    outdoor_actors = [
        actor for actor in editor.get_all_level_actors() if is_outdoor_actor(actor)
    ]
    removed = [
        {
            "label": actor.get_actor_label(),
            "class": actor.get_class().get_name(),
            "path": actor.get_path_name(),
            "components": sorted(
                {
                    component.get_class().get_name()
                    for component_type in OUTDOOR_COMPONENT_TYPES
                    for component in actor.get_components_by_class(component_type)
                }
            ),
        }
        for actor in outdoor_actors
    ]

    for actor in outdoor_actors:
        if not editor.destroy_actor(actor):
            raise RuntimeError(f"could not remove outdoor actor {actor.get_path_name()}")

    remaining = [
        actor.get_path_name()
        for actor in editor.get_all_level_actors()
        if is_outdoor_actor(actor)
    ]
    if remaining:
        raise RuntimeError(f"outdoor actors remain after cleanup: {remaining}")

    if not unreal.EditorLoadingAndSavingUtils.save_current_level():
        raise RuntimeError(f"could not save {LEVEL_PATH}")

    REPORT_PATH.parent.mkdir(parents=True, exist_ok=True)
    REPORT_PATH.write_text(
        json.dumps(
            {
                "level": LEVEL_PATH,
                "removed_actor_count": len(removed),
                "removed_actors": removed,
                "remaining_outdoor_actor_count": 0,
            },
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )
    unreal.log(
        "[configure_indoor_production] removed "
        f"{len(removed)} outdoor actors and saved {LEVEL_PATH}"
    )


main()
