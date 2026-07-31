# nDisplay camera atlas renderer

URSoccerLab's production RGB backend renders URLab MuJoCo cameras into one
nDisplay atlas and disables their duplicate `SceneCaptureComponent2D`
updates. A four-camera configuration remains useful for profiling.

The adapter belongs to URSoccerLab rather than URLab. URLab owns generic
robot-camera transforms and capture APIs; nDisplay is a heavyweight,
application-specific presentation backend and should remain optional.

## Run

```bash
"$HOME/Unreal_Engine_5.7.4/Engine/Binaries/Linux/UnrealEditor" \
  "$PWD/URSoccerLab.uproject" /Game/Levels/URS_SoccerField \
  -game -RenderOffscreen -ForceRes -ResX=1280 -ResY=960 \
  -dc_cluster -dc_dev_mono \
  -dc_cfg="$PWD/Config/ndisplay/four_cameras.ndisplay" -dc_node=node_0 \
  -URSNDisplayCameras -URSNDisplayCameraCount=4 \
  -URSSceneConfig="$PWD/Config/examples/walker_and_observer.json" \
  -URSMotionBlur=0 -URSCameraStats \
  -ExecCmds="MjCamera.AutoReadback 0"
```

`-ForceRes` is required in offscreen mode. Without it, UE retains its default
1280x720 backbuffer and crops the lower 240 pixels of the atlas.

Add `-URSNDisplayScreenshot` to save the composited atlas as
`Saved/Screenshots/urs_ndisplay_four_cameras.png`.

## Initial result

On the Radeon RX 7900 XTX, with four 640x480 views, motion blur and readback
disabled:

- Four independent scene captures, with a 64x64 dispatch viewport: about
  39.7 complete render frames/s.
- Four nDisplay camera-policy viewports in one atlas: about 45.5 complete
  atlas frames/s.

This is approximately a 15% render-rate improvement. It is useful, but it is
not Isaac Sim-style single-view-family batching: UE 5.7 marks nDisplay's
`RenderFamilyGroup` viewport merge mode as not implemented. nDisplay still
renders the four viewports independently and composites them into one output.

The production binder converts the composited atlas to BGRA8, queues one
bounded asynchronous GPU readback, and slices the completed atlas by viewport
rectangle. The TCP transport packages those named slices using the normal
versioned RGB message. It never waits for a GPU fence.

Launch a normal production scene with:

```bash
py_example/.venv/bin/python Tools/runtime/run_scene.py \
  --scene-config Config/examples/six_robots_stereo_rgb.json
```

The launcher and `benchmark_match_vision.py` generate the required atlas under
`Saved/Generated/NDisplay` from the robot count and vision mode. The benchmark
offers `--scene-capture` only for a legacy comparison.

In stereo-RGB mode, both eyes are nDisplay viewports. In RGBD mode, the left
RGB eye is an nDisplay viewport and the right depth sensor remains an
independently scheduled depth-only SceneCapture. This avoids rendering a
second lit RGB view solely to obtain depth.

## Temporal upscaling experiment

The atlas also works with UE 5.7's built-in Temporal Super Resolution (TSR).
For example, append the following settings to `-ExecCmds`:

```text
r.AntiAliasingMethod 4,r.ScreenPercentage 75
```

The RX 7900 XTX results for the static four-camera atlas were:

| Upscaler | Internal resolution per 640x480 view | Atlas rate | MAE vs native | PSNR vs native |
| --- | ---: | ---: | ---: | ---: |
| Native | 640x480 (100%) | ~45.5 FPS | - | - |
| TSR 75% | 480x360 | ~47-48 FPS | 0.0130 | 30.36 dB |
| TSR 67% | ~429x322 | ~48.4 FPS | 0.0133 | 30.37 dB |

The image difference is small, but reducing the shaded pixel count produces
only a modest speedup and reaches a plateau by 75%. This indicates that Lumen,
hardware ray tracing, and per-view rendering overhead dominate this particular
workload. Use 75% as the current quality/performance default; 67% gives no
meaningful additional throughput.

AMD's FSR Upscaling 4.0.3a package officially includes UE 5.7 and would fall
back to analytical FSR 3 on an RX 7900 XTX. It cannot be tested in this Linux
Vulkan configuration: the distributed UE 5.7 plugin contains Win64/D3D12
binaries only, and AMD removed its generic RHI backend. Testing that plugin
therefore requires the Windows/D3D12 build of this project. Do not enable the
plugin in the Linux project configuration.

## Ten robots and twenty cameras

`Config/examples/ten_robots_twenty_cameras.json` places ten robots on the
field. `Config/ndisplay/twenty_cameras.ndisplay` lays their twenty 640x480
cameras out as a 5x4, 3200x1920 atlas. Run it by replacing the scene, nDisplay
configuration, resolution, and camera count in the command above:

```text
-ForceRes -ResX=3200 -ResY=1920
-dc_cfg=.../Config/ndisplay/twenty_cameras.ndisplay
-URSNDisplayCameraCount=20
-URSSceneConfig=.../Config/examples/ten_robots_twenty_cameras.json
```

With hardware ray tracing and Lumen enabled on the RX 7900 XTX:

| Mode | Complete 20-camera atlas rate |
| --- | ---: |
| Native 640x480 per camera | ~1.32-1.36 FPS |
| TSR 75% per dimension | ~1.29-1.32 FPS |

TSR 75% had an MAE of 0.0097 and PSNR of 35.73 dB against the native atlas,
but no performance benefit. All twenty views were live and correctly bound.
This workload therefore misses the 30 Hz target by approximately 23x.

The scaling is much worse than camera-count-only extrapolation. Subsequent
GPU profiling showed that this is not primarily caused by Lumen, hardware ray
tracing, or nDisplay composition itself. The scene's shadow configuration is
the source of the nonlinear collapse; see the profile below.

This run also exposed a separate transport scalability issue. URLab's legacy
per-camera ZMQ publisher exhausts its ten-port bind retry window when creating
twenty cameras. It does not affect the atlas benchmark because automatic
camera readback and publishing are disabled, but it must be fixed or disabled
for a production twenty-camera transport.

### Bottleneck isolation

Follow-up runs separated scene, lighting, resolution, and viewport-count
costs:

| Robots | nDisplay views | Per-view output | Lighting | Atlas rate |
| ---: | ---: | ---: | --- | ---: |
| 2 | 4 | 640x480 | Lumen HWRT | ~45.5 FPS |
| 10 | 4 | 640x480 | Lumen HWRT | ~38.2 FPS |
| 10 | 20 | 640x480 | Lumen HWRT | ~1.33 FPS |
| 10 | 20 | 640x480 | Lumen software tracing | ~1.56 FPS |
| 10 | 20 | 640x480 | Lumen disabled | ~1.5-1.6 FPS |
| 10 | 20 | 640x480 output, 0.5 buffer ratio | Lumen HWRT | ~1.35 FPS |
| 10 | 20 | 320x240 | Lumen HWRT | ~1.35 FPS |

Increasing the robot count from two to ten reduces the four-view result by
only about 16%. Disabling Lumen hardware ray tracing improves the twenty-view
result by about 17%. Disabling Lumen completely, changing TSR, halving
nDisplay's buffer ratio, and physically quartering the output pixel count do
not materially change the twenty-view rate.

### CPU/GPU profile

Matched Unreal CSV profiles were captured after renderer initialization with
GPU CSV stats enabled. The figures below are medians over steady-state frames:
the final 200 frames of the four-view run and final 40 frames of the
twenty-view run.

| Metric | 4 views | 20 views | Scale |
| --- | ---: | ---: | ---: |
| Frame time | 26.4 ms | 760.4 ms | 28.8x |
| GPU time | 25.0 ms | 740.4 ms | 29.7x |
| Render thread | 16.6 ms | 672.0 ms | 40.4x |
| RHI thread | 21.4 ms | 719.9 ms | 33.6x |
| Draw calls | 1,586 | 7,233 | 4.6x |
| Primitives drawn | 170,930 | 788,426 | 4.6x |
| RDG passes | 2,438 | 11,940 | 4.9x |

Draw calls, primitives, and render-graph passes scale almost linearly with the
fivefold view-count increase. The GPU work does not:

| GPU category | 4 views | 20 views | Share of 20-view GPU |
| --- | ---: | ---: | ---: |
| Volumetric cloud shadow | 2.6 ms | 489.5 ms | 66.1% |
| Shadow depths | 7.4 ms | 149.2 ms | 20.2% |
| Ray-tracing scene | 2.0 ms | 14.5 ms | 2.0% |
| Lights | 2.6 ms | 15.0 ms | 2.0% |
| Lumen screen-probe gather | 1.8 ms | 13.0 ms | 1.7% |
| Lumen scene lighting | 1.4 ms | 9.9 ms | 1.3% |
| TSR | 0.7 ms | 5.9 ms | 0.8% |

The game thread spends 757 ms waiting, and the render thread spends 565 ms in
an event wait, because the workload is GPU-bound. nDisplay's measured
composition passes (`RenderFrame`, cross-GPU transfer, deferred-resource
update, and warp/blend) total less than 0.2 ms. They are not the bottleneck.

Disabling only `r.VolumetricCloud.ShadowMap` in a confirmation run, while
retaining Lumen and hardware ray tracing, changed:

| Metric | Cloud shadows on | Cloud shadows off |
| --- | ---: | ---: |
| Frame time | 760.4 ms (1.32 FPS) | 256.9 ms (3.89 FPS) |
| GPU time | 740.4 ms | 249.2 ms |
| Volumetric cloud shadow | 489.5 ms | 0 ms |
| Shadow depths | 149.2 ms | 148.7 ms |

This confirms the first bottleneck and leaves ordinary shadow-depth rendering
as the next target. The scene contains many shadow-casting lamp lights; their
shadow work is repeated for twenty independent views. For the next experiment,
disable cloud shadow maps for robot observations and selectively disable
dynamic shadow casting on local lamps (or retain it only on the nearest few).
Profile that result before deciding whether nDisplay must be replaced by a
custom multiview renderer.

Enabling hardware ray-traced direct shadows with
`r.RayTracing.Shadows=1` was also tested on the same twenty-view workload,
with volumetric-cloud shadow maps disabled in both cases:

| Metric | Virtual Shadow Maps | Ray-traced shadows |
| --- | ---: | ---: |
| Frame time | 256.9 ms (3.89 FPS) | 235.3 ms (4.25 FPS) |
| GPU time | 249.2 ms | 227.6 ms |
| Shadow depths | 148.7 ms | 0 ms |
| Lights | 14.1 ms | 56.0 ms |
| Shadow denoiser | 0 ms | 53.4 ms |
| Draw calls | 7,213 | 2,988 |

Ray tracing removes the expensive shadow-depth pass, but tracing and denoising
the shadows from all lights consumes most of the saving. It is approximately
8% faster overall, not a solution to the twenty-camera throughput target.
It also raises the render-target-pool peak by about 0.8 GB. Keep this result as
an available quality mode; reducing the number of shadowed local lights
remains the more important optimization.

MegaLights was then enabled for the same workload with its default
half-resolution, four-samples-per-pixel hardware-RT path:

```text
r.VolumetricCloud.ShadowMap 0
r.RayTracing.Shadows 0
r.MegaLights.EnableForProject 1
r.MegaLights.HardwareRayTracing 1
```

| Metric | Virtual Shadow Maps | RT shadows | MegaLights |
| --- | ---: | ---: | ---: |
| Frame time | 256.9 ms | 235.3 ms | **169.2 ms** |
| Complete atlas rate | 3.89 FPS | 4.25 FPS | **5.91 FPS** |
| GPU time | 249.2 ms | 227.6 ms | **161.1 ms** |
| Shadow depths | 148.7 ms | 0 ms | 39.6 ms |
| Conventional lights | 14.1 ms | 56.0 ms | 1.0 ms |
| Conventional shadow denoiser | 0 ms | 53.4 ms | 0 ms |
| MegaLights | 0 ms | 0 ms | 17.1 ms |
| Draw calls | 7,213 | 2,988 | **2,628** |

MegaLights is the best tested direct-lighting path, improving frame rate by
52% over VSM and 39% over conventional ray-traced shadows. It efficiently
handles the indoor point lights, but does not support the directional light;
the remaining 39.6 ms shadow-depth cost is therefore outside MegaLights and
should be isolated separately. The visual atlas is saved as
`Saved/Screenshots/twenty_megalights_hwrt.png`.

Disabling conventional dynamic shadows with
`ShowFlag.DynamicShadows=0` removed that residual shadow-depth pass while
leaving the MegaLights pass active:

| Metric | MegaLights | MegaLights, conventional shadows off |
| --- | ---: | ---: |
| Frame time | 169.2 ms (5.91 FPS) | **134.5 ms (7.44 FPS)** |
| GPU time | 161.1 ms | **128.4 ms** |
| Shadow depths | 39.6 ms | **0 ms** |
| MegaLights | 17.1 ms | 17.5 ms |

This show flag is useful for isolation but is broader than the desired
production change. The level's directional-light component should have shadow
casting disabled instead, leaving any deliberately non-MegaLights local
shadows unaffected.

The CSV GPU categories account for essentially the full GPU interval: their
medians total 160.8 ms against 161.1 ms `GPUTime` with conventional shadows
enabled, and 128.5 ms against 128.4 ms with them disabled. There is no hidden
pass larger than `ShadowDepths`; the remainder is the sum of MegaLights,
Lumen, RT-scene construction, translucency, Nanite, TSR, post-processing,
atmosphere, and numerous sub-millisecond passes.

The profile also reports a render-target-pool peak near 20.0 GB for twenty
views versus 9.5 GB for four views. This is close enough to the RX 7900 XTX's
24 GB capacity to make the twenty-view design sensitive to view-state history
and shadow allocations, even though it is not yet an out-of-memory condition.

The partial-view binder disables every URLab `SceneCaptureComponent2D`, not
only the cameras selected for nDisplay. This prevents unselected cameras from
silently rendering and contaminating camera-count measurements.

### Indoor-only RGB and depth lower bound

The outdoor stack was disabled to model a lamp-only indoor deployment:

```text
r.VolumetricCloud=0
ShowFlag.Atmosphere=0
ShowFlag.Fog=0
ShowFlag.VolumetricFog=0
ShowFlag.SkyLighting=0
ShowFlag.DirectionalLights=0
```

MegaLights HWRT and Lumen HWRT remained enabled for RGB. Medians below are
over the final 60 frames on the RX 7900 XTX:

| Workload | Frame time | GPU time | Rate | RT pool peak |
| --- | ---: | ---: | ---: | ---: |
| 20 RGB views | 125.9 ms | 118.5 ms | 7.94 FPS | 20.03 GB |
| 10 RGB views, one `left_eye` per robot | 34.3 ms | 30.6 ms | 29.2 FPS | 13.45 GB |
| 10 unlit/depth-like views, one `left_eye` per robot | 24.5 ms | 21.2 ms | 40.9 FPS | 12.89 GB |

The ten-view selector uses
`-URSNDisplayCameraName=left_eye`; without that filter, requesting the first
ten sorted cameras selects both eyes from only five robots.

The twenty-view slowdown is nonlinear. Draw calls (928 to 1,852) and RDG
passes (4,245 to 8,466) nearly double, but the render-target pool grows close
to the 24 GB card's practical allocation ceiling and several view-local passes
lose cache/locality:

| GPU category | 10 RGB | 20 RGB | Scale |
| --- | ---: | ---: | ---: |
| MegaLights | 2.85 ms | 16.33 ms | 5.7x |
| Lumen screen-probe gather | 4.81 ms | 17.62 ms | 3.7x |
| Lumen scene lighting | 3.55 ms | 11.62 ms | 3.3x |
| Translucency lighting | 0.49 ms | 10.55 ms | 21.6x |
| Ray-tracing scene | 5.54 ms | 15.13 ms | 2.7x |

This is an nDisplay/view-state resource cliff, not a simple twice-the-pixel
hardware limit. A ten-view RGB-D mode should reuse each RGB view's existing
depth attachment instead of adding another SceneCapture. The unlit benchmark
is conservative rather than a true depth-only renderer: it still measured
7.8 ms of shadow-depth work and 5.7 ms of ray-tracing-scene construction.

A first Lumen quality experiment used screen-probe downsample factor 24,
reflection downsample factor 2, and disabled translucency lighting volumes.
It measured 28.5 FPS versus 29.2 FPS at defaults, so no performance gain is
claimed from that preset.

Two lifecycle defects found during profiling have since been fixed:

- endpoint metadata and command state are protected while the physics callback
  and game thread access them;
- TCP state publishing reads URLab's coherent render snapshot instead of live
  `mjData`.

The earlier independent-SceneCapture benchmarks shut down cleanly while
MuJoCo continued at 0.983x and 0.9997x wall time respectively; the production
atlas results below supersede their camera-throughput figures.

### Production atlas delivery

The production readback and TCP path was validated on the RX 7900 XTX with
the tracked sun/atmosphere, volumetric clouds disabled, Lumen HWRT, MegaLights
HWRT, JPEG quality 85, and 640x480 sensors:

| 3v3 mode | Delivered rate per sensor | Minimum physics/wall ratio |
| --- | ---: | ---: |
| 12 RGB views | 15.5 RGB/s | 0.9999 |
| 6 RGBD pairs | 24.2 RGB/s, 5.1 depth/s | 0.9997 |

All messages were complete and had zero sequence gaps. The 12-RGB rate
matched the raw 12-view render cadence, showing that atlas readback, JPEG, and
TCP no longer reduce render throughput. The remaining 12-view limit is the
production render workload itself.
