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

You also need a scene JSON, external robot packages, and a field image. These
assets are supplied separately; robot models and texture images are not inside
the AppImage. A typical installation is:

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
the example field image and grass PBR maps; `ball.7z` contains sample checkerboard
PBR maps for testing. Legacy backups are excluded. Extract them from your match
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
