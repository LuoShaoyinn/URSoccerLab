# URSoccerLab

URSoccerLab simulates robot soccer with MuJoCo physics and Unreal Engine camera
rendering. Run the simulator from an AppImage, then connect a Python client to
control a robot or watch through a guest camera. Unreal Engine and Docker are
not needed to run the packaged simulator.

## Start the simulator

You need Linux x86-64, a Vulkan-capable GPU with current drivers, and the
`URSoccerLab.AppImage`. For AV1 camera streaming, the GPU and its Vulkan driver
must support AV1 video encoding. The current Docker packaging baseline is
Ubuntu 24.04; older Linux distributions are not yet validated.

You also need a scene JSON, robot packages, and field assets. They are separate
from the AppImage; this branch includes the default field bundle in the Git
checkout, while robot models and optional ball PBR maps are supplied separately.
The ball's default mesh and skin remain built in. A typical installation is:

```text
match/
├── URSoccerLab.AppImage
├── scene.json
└── assets/
    ├── field/albedo.png
    └── robots/booster_k1/
        ├── robot.json
        ├── model.xml
        └── meshes/
```

Follow [Getting started](docs/Getting_Started.md) to create `scene.json`, then:

```bash
chmod +x URSoccerLab.AppImage
./URSoccerLab.AppImage scene.json
```

Local resource archives are generated separately under `dist/resources/`:
`robots.7z` contains Pi Plus, MOS9 and Booster K1 packages; `fields.7z` contains
the example field image and grass PBR maps; `ball.7z` contains the original ball
GLB/MJCF, extracted PBR textures and sample checkerboard maps for testing.
The ball archive also includes its attribution and license links.
Legacy backups are excluded. Extract them from your match
directory with `7z x robots.7z` (and likewise for the other archives). They restore
paths under `external/`; point your scene JSON at those paths instead of the
`assets/` paths in the example. The archives are build outputs and are not in Git.

**The simulator accepts exactly one argument: the scene JSON file.** Set all
scene, physics, lighting and camera options in JSON. Extra Unreal flags and the
former `-URSSceneConfig=...` form are rejected. Rendering is offscreen; use a
client to receive camera images. Stop the simulator with Ctrl+C.

If FUSE is unavailable, use the AppImage runtime's extraction environment setting:

```bash
APPIMAGE_EXTRACT_AND_RUN=1 ./URSoccerLab.AppImage scene.json
```

Logs and runtime data are under `~/.local/share/URSoccerLab/`, or beneath
`$XDG_DATA_HOME/URSoccerLab/` when that variable is set.

## Connect a client

The Python client is distributed separately under `py_example/`. From this
repository, install its dependencies and run an example:

```bash
uv sync --project py_example
uv run --project py_example python py_example/examples/standing/standing.py
```

Use a scene matching the example's robot count and type. Walking examples require
their own robot package and controller. See [Python clients](py_example/README.md)
for robot control, camera receiving and guest recording.

Default TCP ports are:

| Client | Port |
| --- | --- |
| First robot | 10000 |
| Further robots, in JSON order | 10001, 10002, … |
| Administration: reset, pose and scene operations | 11000 |
| Guest floating cameras | 12000 |

Robot connections carry control, state and camera data. Guests receive camera
images and control their own camera pose; they cannot administer the match.
Only expose the ports needed by your clients.

## Configure your match

[Getting started](docs/Getting_Started.md) includes a complete scene example.
The [scene reference](docs/URSoccerLab_Scene_Building_Api.md) documents physics,
PBR textures, lighting, motion blur and film grain. The hall stays fixed; field
size, field image, both goals, ball settings and robots come from your JSON.
Relative asset paths resolve from the JSON file's directory. Change the JSON or
external assets and restart the simulator; no rebake is needed for those inputs.

[Robot packages](docs/Robot_Packages.md) describes the external MJCF/GLB format.
[Booster conversion](Tools/robots/README.md) explains how to normalize the K1
archive. Camera calibration and controller tuning remain package-specific.

## Use the turf assets

This branch provides two ways to use the work:

1. **Complete turf field:** use the generated field image with its white lines,
   the repeating PBR detail maps, and optionally the 3D grass overlay. The
   default scene already combines all of them.
2. **Grass model only:** add the 3D grass GLB over a field image/material that
   your project already uses. Keep your existing field appearance and physics.

Both options are visual only. MuJoCo contacts remain on the flat `field_ground`
plane and use the scene's configured friction.

### Get the branch and field files

After checking out this branch (or the branch containing this change), download
only its field LFS assets:

```bash
git lfs install
git lfs pull --include="Assets/Scenes/SoccerField/visual/**,Assets/Scenes/SoccerField/physics/field_physics.xml,external/field/**"
```

For a fresh clone, skip automatic downloads first, then pull the same field
files. This avoids failing the checkout on unrelated LFS objects such as the
ball or Unreal environment assets:

```bash
GIT_LFS_SKIP_SMUDGE=1 git clone \
  --branch features/simulation_lawn \
  https://github.com/MIng-Diamond/URSoccerLab-features-Lawn.git URSoccerLab
cd URSoccerLab
git lfs install
git lfs pull --include="Assets/Scenes/SoccerField/visual/**,Assets/Scenes/SoccerField/physics/field_physics.xml,external/field/**"
```

The grass GLB is about 409 MiB. The field image and PBR maps are also stored in
Git LFS. The filtered pull materializes the field assets; a source build still
needs the Unreal project assets and submodules required by the checkout.

### Option 1: use the complete prepared field

`Config/URS_scene.json` is the ready-to-use example. Its `field.visual` block
points to `external/field/example.png` (grass base color with the white field
markings), the normal/roughness/AO detail maps, and
`Assets/Scenes/SoccerField/visual/grass_blades.glb`. The geometry leaves the
white marking paths clear. Keep the matching default dimensions: 9 x 6 m of
playing area with 0.8 m X borders and 0.9 m Y borders. When using a copied scene
JSON, paths are relative to that JSON file, so preserve the bundle layout or
adjust the paths.

Run the scene with a packaged build that includes this branch's runtime loader:

```bash
./dist/URSoccerLab.AppImage Config/URS_scene.json
```

An AppImage built from an older branch may render the field image but will not
load the `grass_mesh` overlay. Build the updated source target as described
below before previewing the 3D fibers.

### Option 2: add only the grass model to an existing field

Copy `Assets/Scenes/SoccerField/visual/grass_blades.glb` into the teammate's
asset bundle, then add its path to the existing `field.visual` object. Keep the
existing `base_color_map`, any normal/roughness/AO settings, and the complete
`field.physics` block unchanged. For example, in a scene JSON stored at the
project root:

```json
"visual": {
  "base_color_map": "assets/my_existing_field.png",
  "grass_mesh": "assets/grass_blades.glb"
}
```

Do not use the GLB as `base_color_map`: it is geometry, not a texture. The
included mesh has white-line clearance laid out for a 9 x 6 m pitch and the
default borders above. If the teammate's pitch dimensions or line layout are
different, regenerate the GLB with matching dimensions and borders so the
standard soccer-line clearances line up. The generator currently creates this
standard line plan; adapt its line definitions first if the field uses a custom
marking layout:

```bash
python3 Tools/field/generate_grass_assets.py \
  --length-m 9 --width-m 6 --border-x-m 0.8 --border-y-m 0.9 \
  --out-dir /tmp/field-assets \
  --grass-mesh-out /path/to/grass_blades.glb
```

The generator also writes a field image and PBR maps under `/tmp/field-assets`;
the grass-only workflow can ignore those outputs. `grass_mesh` is a
non-colliding visual overlay and does not replace the field image, markings, or
MuJoCo contact surface.

### Build and visualize the 3D field

The runtime loader is part of this branch. For a source preview, install the
Python client dependencies, build the updated editor target, and start either
the complete example or the teammate's edited scene. The source build requires
Unreal Engine 5.7.4 and the FFmpeg development headers and libraries used by the
project:

```bash
uv sync --project py_example
export URS_UE_ROOT=/path/to/Unreal_Engine_5.7.4
export URS_UE="$URS_UE_ROOT/Engine/Binaries/Linux/UnrealEditor"
"$URS_UE_ROOT/Engine/Build/BatchFiles/Linux/Build.sh" \
  URSoccerLabEditor Linux Development "$PWD/URSoccerLab.uproject"
uv run --project py_example python Tools/runtime/run_scene.py \
  --scene-config Config/URS_scene.json
```

In a second terminal, capture a full-field view through the guest inspector:

```bash
uv run --project py_example python Tools/field/capture_field_view.py \
  --out artifacts/outputs/field_preview.png
```

Open `artifacts/outputs/field_preview.png`. The scene must be running, and its
`guest_inspector.enabled` setting must be true. To use the packaged runtime,
build the AppImage from this branch as described in
[AppImage packaging](Tools/packaging/README.md), launch it with the selected
scene JSON, then run the capture command.

## Build from source

Developers need Unreal Engine 5.7.4 and Git LFS:

```bash
git lfs install
git submodule update --init --recursive
git lfs pull
```

The application ships generic runtime loaders, not robot assets. Supply external
packages before launching a source checkout. See [Docker packaging](Tools/packaging/README.md)
for building the AppImage. [Runtime architecture](docs/Runtime_Architecture.md)
and [inspector protocol](docs/Inspector_Plan.md) describe the implementation.
