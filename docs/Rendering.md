# Rendering, lighting and camera effects

Robot eyes and guest cameras use the shared nDisplay camera pipeline and atlas
readback. Robot resolution comes from package cameras (640×480 per eye in the
current package contract). Guest width, height, FOV and output rate are configured
in `guest_inspector`, not negotiated by a client. See [guest cameras](Guest_Cameras.md).
Streaming rate is a target; increasing the view count or resolution increases
rendering and encoding cost.

Source builds disable offline Path Tracing shader compilation. Lumen and hardware
ray tracing remain enabled for the robot and guest camera pipeline. This setting
requires a fresh cook to take effect in the AppImage.

## Hall texture resolution

The source hall's large textures and window skybox use a 1024-pixel maximum dimension.
Unreal retains the original source images and builds smaller runtime textures.
External field, ball and robot textures keep their own resolutions. The skybox
remains a visible unlit background, separate from hall illumination.

For source builds, [limit_hall_textures.py](../Tools/editor/limit_hall_textures.py)
audits these assets; `--apply` saves their maximum texture sizes. Changes need a
new cook to appear in an AppImage.

## Hall illumination

The fixed hall uses 21 visible emissive lamp meshes with Lumen. All lamps and
robot visuals use lighting channel 0. Configure absolute linear emission:

```json
"lighting": {
  "emissive_intensity": 10,
  "lamp_intensity_lumens": 0,
  "source_radius_cm": 60,
  "specular_scale": 0
}
```

`emissive_intensity` controls the lamp materials and their Lumen illumination;
it is an absolute Unreal linear colour value, not lumens or a relative multiplier.
`lamp_intensity_lumens` controls separate auxiliary point lights, which are off in
the default hall preset. Radius and specular scale apply to those auxiliary lights.

## Exposure, motion blur and film grain

Add these settings to the scene's `render` object:

```json
"render": {
  "enable": true,
  "auto_exposure": false,
  "exposure_compensation": 2.5,
  "motion_blur": true,
  "motion_blur_amount": 0.5,
  "motion_blur_max_percent": 5,
  "motion_blur_target_fps": 0,
  "film_grain": {
    "intensity": 0.1,
    "shadows": 1,
    "midtones": 1,
    "highlights": 0.25,
    "texel_size": 1
  }
}
```

Fixed exposure makes illumination comparisons reproducible; each +1 compensation
is one exposure stop. Motion blur is enabled explicitly. Target FPS zero follows
the configured camera output rate. Film grain is Unreal's built-in effect;
intensity zero disables it. There is no custom camera noise model. Both effects
apply to RGB before compression; metric depth remains separate.

See the [scene reference](URSoccerLab_Scene_Building_Api.md#illumination-and-camera-effects)
for defaults and ranges, [field PBR](Field_PBR.md) for ground materials, and
[ball PBR](URSoccerLab_Scene_Building_Api.md#external-ball-pbr-maps) for ball textures.
Restart the simulator after editing settings.
