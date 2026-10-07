# URSoccerLab guide for agents

This file applies to the whole repository. Read the user workflow before changing
the implementation. Follow the current task's instructions and inspect the
worktree before relying on earlier build results.

## User workflow

URSoccerLab combines MuJoCo physics with Unreal Engine robot and guest camera
rendering. Users run the AppImage and connect through the Python client package.
They do not need Unreal Engine or Docker installed.

### Installation and startup

- The packaged baseline is Linux x86-64, Ubuntu 22.04 / glibc 2.35.
- Video streaming requires a supported hardware encoder; the shared JSON policy selects native NVENC/QSV/VAAPI or Vulkan, with optional H.264/H.265 fallback.
  There is currently no automatic software encoder fallback.
- The Python client wheel requires Python 3.12 and installs as `ursoccerlab`.
- Supply a scene JSON, external robot packages, and external field textures.
  These resources are distributed separately and are not bundled in the AppImage.

```bash
chmod +x URSoccerLab.AppImage
./URSoccerLab.AppImage scene.json
python -m pip install ursoccerlab_client-0.1.1-py3-none-any.whl
```

The AppImage accepts exactly one argument: the scene JSON file. Configure scene,
physics, lights and camera effects in JSON. Extra Unreal arguments are rejected.
When FUSE is unavailable, set `APPIMAGE_EXTRACT_AND_RUN=1` before launching.
Restart the simulator after editing the JSON; do not promise automatic file reload.

### Assets and configuration

- Every spawned robot requires an external package manifest in `robot_types`.
- Field dimensions and its external base-colour map are required. Additional PBR
  maps are optional. The image is mapped over the configured field and borders.
- Goal configuration requires dimensions and exactly two explicit poses.
- The hall, lamps, generic field/goal geometry, and default ball mesh and textures
  are bundled. Ball physics and optional external PBR texture overrides use JSON.
- Guest camera resolution, frame rate and capacity are configured in
  `guest_inspector`; guests send their camera pose, rather than negotiating these
  settings through a handshake.

### Connections

Import `RobotClient`, `AdminClient` and `InspectorClient` from `ursoccerlab`.
Robot ports start at TCP 10000, following robot order in JSON. Administration
uses 11000 and guest cameras use 12000 by default. A robot connection carries
control, state, RGB and optional depth data.

Stereo AV1 contains both eyes side by side. RGBD uses left-eye AV1 and a separate
lossless depth message. Clients may wait for a periodic keyframe when joining;
connection and recovery events do not force keyframes.

For complete instructions, use [Getting started](docs/Getting_Started.md),
[scene configuration](docs/URSoccerLab_Scene_Building_Api.md),
[Python clients](py_example/README.md), and [the guide index](docs/README.md).

## Development workflow

### Repository layout and ownership

- `Source/URSoccerLab/`: scene configuration, physics integration, camera capture,
  protocol, transport, inspector and runtime coordination.
- `Plugins/UnrealRoboticsLab/`: pinned Git submodule providing MuJoCo integration.
  Inspect its revision and dirty state before editing; its commits and parent
  submodule pointer are separate changes.
- `Plugins/glTFRuntime/`: runtime GLB loader. External robot visuals do not require
  cooking, and runtime-loaded meshes do not use Nanite.
- `Content/`: required Unreal hall, ball, level, physics and generic loader assets.
  `Assets/`: maintained ball and ground-physics source files. These directories
  remain necessary for source builds.
- `external/`: Git-ignored robot packages and field/ball textures. Do not commit
  or automatically upload these resources, or restore retired built-in robots/maps.
- `py_example/`: reusable Python client, examples and client tests.
- `Tools/`: editor utilities, runtime checks, robot conversion and packaging.
- `artifacts/` and `dist/`: ignored generated outputs. Preserve requested renders,
  release artifacts and checksums when cleaning temporary files.

Track intentional `.uasset`/`.umap` changes with Git LFS. Before removing an Unreal
asset, check its referencers and runtime string loads. Remove an obsolete directory
completely after its data has moved out; do not leave placeholder directories or
scripts pointing to removed assets. Inspect ignored files before deleting them.

### Implementation boundaries

Keep MuJoCo physics, Unreal capture, encoding, protocol and socket transport
separate. Exchange owned frames, commands and snapshots through the existing
buffers/mailboxes. Socket and encoder workers must not manipulate UObjects or
access MuJoCo state directly. See [runtime architecture](docs/Runtime_Architecture.md).

Keep robot and guest cameras on the shared nDisplay pipeline. Preserve independent
state/camera schedules and separate lossless depth delivery. Update the Python
receiver and [protocol documentation](docs/URSoccerLab_TCP_Runtime.md) with wire
changes. Shared `encoder` settings default to AV1, automatic hardware backend selection and H.264 fallback. Keep robot and guest codec/backend selection identical.

The hall uses lighting channel 0 and emissive lamps with Lumen. Keep hardware ray
tracing enabled. Offline Path Tracing compilation and the unused neural denoiser
are disabled. Large hall/skybox textures are capped at 1024; preserve their source
images. Keep the default ball skin and ZeroMQ library bundled. ZeroMQ remains a
plugin dependency, although URSoccerLab disables its camera broadcasts.

### Build and validation

Use Unreal Engine 5.7.4, Git LFS assets and the pinned submodule. Run commands from
the repository root. The Docker wrapper also handles a Podman Docker shim.

```bash
export URS_UE=/path/to/Unreal_Engine_5.7.4
python3 Tools/packaging/build_docker.py build
python3 Tools/editor/validate_baked_assets.py
python3 Tools/packaging/test_launcher.py
uv sync --project py_example
uv run --project py_example python -m unittest discover -s py_example/tests
```

The `build` phase compiles Development game/editor targets without cooking or
replacing the AppImage. Standalone asset validation checks files and MJCF; running
it inside Unreal also checks asset loading. AV1 fixture tests require a PyAV build
with the software encoder used by the fixture; this is separate from the server's
hardware encoders. Choose checks relevant to the change and report limits.

An editor build or NullRHI startup does not prove packaged camera rendering.
External robot rendering needs a GPU. For camera changes, render and inspect
frames using the actual robot/guest pipeline, including a settled middle frame
when investigating temporal artifacts.

### Packaging and publication

Keep source validation and packaging distinct. Respect requests to test before
cooking or to leave the AppImage unchanged. When packaging is part of the task:

```bash
python3 Tools/packaging/build_docker.py all
uv build --wheel --out-dir dist py_example
uv run --project py_example python Tools/packaging/smoke_appimage.py --shipping --builtin-ball
```

The AppImage uses Shipping configuration, compressed cooked content, and an
Ubuntu 22.04 builder. It excludes debug symbols and optional Vulkan diagnostic
layers. Shipping disables Unreal engine logs and the development console.
Use the smoke test without `--builtin-ball` to check external ball PBR instead.
For release validation, also install the wheel in a fresh environment and test
through that installed package. Inspect the rendered default ball before claiming
its textures are present.

Check bundled ELF glibc requirements, final sizes and SHA256 checksums. Update the
ignored release validation record and checksums whenever an artifact changes.
Do not bundle GPU drivers or raise the compatibility baseline accidentally.
See [packaging instructions](Tools/packaging/README.md) for phases and dependencies.

Keep documentation user-oriented and describe implemented behavior. Commit/push,
merge and publish according to the task's requested branch and scope; preserve
unrelated changes and external resources. Report what changed, what was tested,
and which build or artifact those results cover.
