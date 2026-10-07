# URSoccerLab

URSoccerLab simulates robot soccer with MuJoCo physics and Unreal Engine camera
rendering. Run the simulator from an AppImage, then connect a Python client to
control a robot or watch through a guest camera. Use the shipped AppImage and
Python wheel from your own working directory. No repository checkout, Unreal
Engine installation or Docker installation is needed to run them.

## User guide

Start with [Getting started](docs/Getting_Started.md), then choose a topic below.
The complete [guide index](docs/README.md) links to user and developer references.
All documentation is Markdown and can be read directly on GitHub.

| Topic | Guides |
| --- | --- |
| Overview and installation | [First scene and external resources](docs/Getting_Started.md) |
| Concepts and configuration | [Coordinates and clocks](docs/Concepts.md) · [Scene JSON](docs/URSoccerLab_Scene_Building_Api.md) |
| Physical entities | [Field map](docs/Field_Assets.md) · [Goalposts](docs/URSoccerLab_Scene_Building_Api.md#goalposts) · [Ball physics](docs/URSoccerLab_Scene_Building_Api.md#ball-overrides) |
| Loading assets | [Robot packages](docs/Robot_Packages.md) · [Booster conversion](Tools/robots/README.md) · [Field PBR](docs/Field_PBR.md) · [Ball PBR](docs/URSoccerLab_Scene_Building_Api.md#external-ball-pbr-maps) |
| Robot control and sensors | [Python clients and examples](py_example/README.md) · [Guest cameras](docs/Guest_Cameras.md) |
| Rendering and streaming | [Lighting and camera effects](docs/Rendering.md) · [AV1 video and depth](docs/AV1_Runtime.md) |
| Troubleshooting | [Startup and logs](docs/Getting_Started.md#troubleshooting) |
| Developers | [Architecture](docs/Runtime_Architecture.md) · [TCP protocol](docs/URSoccerLab_TCP_Runtime.md) · [Tools](Tools/README.md) · [Docker packaging](Tools/packaging/README.md) |

## Get the distribution files

Use the supplied `URSoccerLab.AppImage` and
`ursoccerlab_client-0.1.0-py3-none-any.whl`. Robot packages and field textures are
separate resource archives. The wheel provides the Python API; `py_example/`
is a reference for client usage and example programs.

## Start the simulator

You need Linux x86-64, a Vulkan-capable GPU with current drivers, and the
`URSoccerLab.AppImage`. For AV1 camera streaming, the GPU and its Vulkan driver
must support AV1 video encoding. The Docker packaging baseline is
Ubuntu 22.04 (glibc 2.35); older Linux distributions are not validated.

You also need a scene JSON, external robot packages, and a field image. These
assets are supplied separately; robot models and field maps are not inside
the AppImage. The ball's default mesh and skin remain built in; optional external
PBR maps override its skin. A typical installation is:

```text
match/
├── URSoccerLab.AppImage
├── ursoccerlab_client-0.1.0-py3-none-any.whl
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

Resource archives are supplied separately:
`robots.7z` contains Pi Plus, MOS9 and Booster K1 packages; `fields.7z` contains
the example field image and grass PBR maps; `ball.7z` contains the original ball
GLB/MJCF, extracted PBR textures and sample checkerboard maps for testing.
The ball archive also includes its attribution and license links.
Legacy backups are excluded. Extract them from your match
directory with `7z x robots.7z` (and likewise for the other archives). They restore
paths under `external/`; point your scene JSON at those paths instead of the
`assets/` paths in the example. These resources are not stored in Git.

**The simulator accepts exactly one argument: the scene JSON file.** Set all
scene, physics, lighting and camera options in JSON. Extra Unreal flags and the
former `-URSSceneConfig=...` form are rejected. Rendering is offscreen; use a
client to receive camera images. Stop the simulator with Ctrl+C.

If FUSE is unavailable, use the AppImage runtime's extraction environment setting:

```bash
APPIMAGE_EXTRACT_AND_RUN=1 ./URSoccerLab.AppImage scene.json
```

Runtime data are under `~/.local/share/URSoccerLab/`, or beneath
`$XDG_DATA_HOME/URSoccerLab/` when that variable is set. Shipping builds omit
Unreal engine logs.

## Connect a client

Use Python 3.12 and install the shipped wheel into your own virtual environment:

```bash
python3.12 -m venv .venv
source .venv/bin/activate
python -m pip install ./ursoccerlab_client-0.1.0-py3-none-any.whl
```

Import the installed package from your own Python program:

```python
from ursoccerlab import RobotClient, AdminClient, InspectorClient
from ursoccerlab.media import camera_to_rgb, depth_to_meters
```

See [Python API usage and examples](py_example/README.md) for robot control,
camera receiving and guest recording. Browse the [example programs](py_example/examples)
as reference code and adapt them to your application. Match your scene's robot
types and actuator names; walking examples also require separate controllers and
policy weights. Pip installs the wheel's dependencies separately.

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

## Development

For changes to the simulator or client library, see [the development guide](AGENTS.md#development-workflow)
and [Docker packaging](Tools/packaging/README.md). Source development requires
Unreal Engine 5.7.4, Git LFS and the pinned submodule. Supply external robot and
field resources separately. [Runtime architecture](docs/Runtime_Architecture.md)
and [inspector protocol](docs/Guest_Cameras.md) describe the implementation.
