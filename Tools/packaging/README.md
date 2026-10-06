# Build the AppImage with Docker

Users start the simulator with exactly one scene JSON file:

```bash
./URSoccerLab.AppImage scene.json
```

See [Getting started](../../docs/Getting_Started.md) for external assets and a
complete scene. The launcher rejects extra arguments and the former
`-URSSceneConfig=...` syntax. All runtime settings belong in JSON. Its internal
Unreal arguments are generated automatically, including robot and guest viewports.

## Build requirements

A Linux x86-64 host with Docker, Unreal Engine 5.7.4, Git LFS content and the
pinned URLab submodule. The engine is mounted into the container; it is not
redistributed in the builder image. Cooking runs as the host user's UID, because
UnrealEditor refuses root execution. Allow disk space for the engine, container,
cook cache and staging tree.

From the repository root:

```bash
export URS_UE=/path/to/Unreal_Engine_5.7.4
python3 Tools/packaging/build_docker.py
```

The script builds `ursoccerlab-packager:24.04`, then builds both Unreal targets,
cooks/stages the application and assembles `dist/URSoccerLab.AppImage` entirely
inside the container. It supports Docker's Podman shim with isolated user storage
when that shim is installed. Build caches persist between invocations.

The Ubuntu 24.04 builder compiles a minimal shared FFmpeg 8.0.1 with Vulkan AV1
encoding and bundles its runtime dependencies. Vulkan headers are pinned to
1.4.321. The host's GPU driver and Vulkan loader are not bundled. The current
packaging baseline is Ubuntu 24.04; older distributions and other GPU drivers
require separate validation.

## Package contents

The AppImage contains the cooked hall and lamps, ball mesh, generic field/goal
geometry, runtime robot loader and required libraries. Robot packages, field and
ball images, scene JSON, Python clients and developer tools are external. The
launcher bundles jq for real JSON parsing; no host Python installation is required.

FFmpeg, libstdc++, libgcc and launcher dependencies are staged under `usr/lib`.
The FFmpeg LGPL license is included. FFmpeg source is available at
https://ffmpeg.org/releases/ffmpeg-8.0.1.tar.xz; its exact configure/build command
is in [Dockerfile](Dockerfile). Bundled library licenses remain applicable.

## Resumable phases

The Docker wrapper also accepts a developer phase to resume a completed build:

```bash
python3 Tools/packaging/build_docker.py appdir
python3 Tools/packaging/build_docker.py image
```

Inside the builder, the packager has three developer phases:

```bash
python3 Tools/packaging/package_appimage.py cook
python3 Tools/packaging/package_appimage.py appdir
python3 Tools/packaging/package_appimage.py image
```

`cook` uses Unreal BuildCookRun. `appdir` reads `Saved/StagedBuilds/Linux`, copies
runtime libraries and writes the JSON-only launcher. `image` runs appimagetool.
The developer phase names are not simulator arguments. Do not name a workspace
output folder `build`: it conflicts with Unreal's `Build` directory; use `dist`.

To run without FUSE:

```bash
APPIMAGE_EXTRACT_AND_RUN=1 ./dist/URSoccerLab.AppImage scene.json
```

Logs and data live under `$XDG_DATA_HOME/URSoccerLab`, defaulting to
`~/.local/share/URSoccerLab`. Only the host GPU/Vulkan stack is needed to render;
AV1 additionally requires Vulkan AV1 video encode support. The simulator is
headless and images are received through client connections.
