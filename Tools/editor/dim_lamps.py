#!/usr/bin/env python3
"""Scale the intensity of every auto-generated emissive-lamp point light in the
production field level.

The lamps were created by convert_emissive_lamps.py (each tagged
``URS_AutoEmissiveLamp``). This tool multiplies each lamp's intensity by
``DIM_FACTOR`` and saves the level. Run again with a different ``DIM_FACTOR``
to re-target; run convert_emissive_lamps.py to reset to the 280 lm default.

Run inside Unreal Editor against the built URSoccerLab project:

    UnrealEditor-Cmd URSoccerLab.uproject \
      -ExecutePythonScript="$PWD/Tools/editor/dim_lamps.py" \
      -NullRHI -unattended -nop4 -nosplash
"""

from __future__ import annotations

import unreal


LEVEL_PATH = "/Game/Levels/URS_SoccerField"
GENERATED_LABEL_PREFIX = "URS_AutoEmissiveLamp_"
GENERATED_TAG = "URS_AutoEmissiveLamp"
DIM_FACTOR = 0.4


def main() -> None:
    if not unreal.EditorLoadingAndSavingUtils.load_map(LEVEL_PATH):
        raise RuntimeError(f"could not load level {LEVEL_PATH}")

    editor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    touched: int = 0
    sample: list[tuple[str, float, float]] = []
    for actor in editor.get_all_level_actors():
        tags = {str(tag) for tag in actor.tags}
        if not (actor.get_actor_label().startswith(GENERATED_LABEL_PREFIX)
                or GENERATED_TAG in tags):
            continue
        components = actor.get_components_by_class(unreal.PointLightComponent)
        if not components:
            continue
        comp = components[0]
        try:
            before = float(comp.get_editor_property("intensity"))
        except Exception:
            before = 0.0
        after = before * DIM_FACTOR
        # set via the editor-property path so PostEditChange marks the owning
        # level package dirty; set_intensity() alone does not, and a non-dirty
        # level is silently skipped by save_current_level().
        comp.set_editor_property("intensity", after)
        touched += 1
        if len(sample) < 5:
            sample.append((actor.get_actor_label(), before, after))

    if touched == 0:
        unreal.log_warning(
            "[dim_lamps] no URS_AutoEmissiveLamp actors found; run "
            "convert_emissive_lamps.py first"
        )
        return
    unreal.EditorLoadingAndSavingUtils.save_current_level()
    for label, before, after in sample:
        unreal.log(f"[dim_lamps] {label}: {before:.1f} -> {after:.1f}")
    unreal.log(
        f"[dim_lamps] done: scaled {touched} lamp(s) by {DIM_FACTOR} and saved level"
    )


if __name__ == "__main__":
    main()
