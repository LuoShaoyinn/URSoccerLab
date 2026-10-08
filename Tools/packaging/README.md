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

The script builds `ursoccerlab-packager:22.04`, then builds both Unreal targets,
cooks/stages the application and assembles `dist/URSoccerLab.AppImage` entirely
inside the container. It supports Docker's Podman shim with isolated user storage
when that shim is installed. Build caches persist between invocations.

The builder uses Ubuntu 22.04. A separate stage compiles minimal shared FFmpeg
8.1.3 with native NVENC/QSV/VAAPI and Vulkan video encoding; GCC, NASM and development packages are
excluded from the final builder stage. Unreal uses the engine's bundled Clang
toolchain and sysroot. CoACD's OpenMP runtime is installed explicitly.

Vulkan headers are pinned to 1.4.321. The bundled generic libva 2.24.1 dispatcher searches the host’s standard DRI driver directories; oneVPL 2.16.0 provides the Intel dispatcher. Neither includes a GPU driver. Ubuntu 22.04 glslang supplies FFmpeg's GLSL initialization/compiler support,
linked statically with SPIR-V tools and their license notices. CPU pixels are
converted to NV12 and supplied to the selected hardware encoder. Native driver APIs are loaded dynamically. The generic Vulkan loader is bundled from Unreal; host GPU drivers are not bundled.
The packaged ELF libraries require at most glibc 2.35. Startup and MuJoCo were
checked in Ubuntu 22.04 with NullRHI and no external robot rendering. GPU AV1
stereo/RGBD and guest streaming are checked separately on the current RADV host;
this does not establish compatibility with every Ubuntu 22.04 GPU/driver.
External robot rendering requires a GPU; NullRHI cannot initialize its meshes.

## Package contents

The AppImage contains the cooked hall and lamps, ball mesh, generic field/goal
geometry, runtime robot loader and required libraries. Robot packages, field images, optional ball texture overrides, scene JSON,
Python clients and developer tools are external. The default ball skin remains
bundled alongside its mesh. The
launcher bundles jq for real JSON parsing; no host Python installation is required.

Release assembly omits `.debug`/`.sym` files and optional Vulkan diagnostic layers.
The packaged game uses Shipping configuration and compressed cooked content.
Large hall textures and the window skybox are capped at a 1024-pixel maximum
dimension. Offline Path Tracing shaders are excluded; Lumen hardware ray tracing
remains enabled. The default ball textures and ZeroMQ library remain bundled.
The unused neural path-tracing denoiser is disabled; cameras retain Lumen and
hardware ray tracing. Shipping omits Unreal's development console and engine
logs, while JSON configuration and TCP control remain available.
The symbols remain in the staged build for debugging. Rendering uses the host Vulkan driver; encoding uses the selected native or Vulkan backend; the release retains the engine and encoder libraries.

FFmpeg, libstdc++, libgcc and launcher dependencies are staged under `usr/lib`.
The launcher prefers a newer host libstdc++/libgcc when available, so modern GPU
drivers are not constrained by the bundled C++ ABI; otherwise it uses the bundle.
The FFmpeg LGPL license is included. FFmpeg source is available at
https://ffmpeg.org/releases/ffmpeg-8.1.3.tar.xz; its exact configure/build command
is in [Dockerfile](Dockerfile). Bundled library licenses remain applicable.

## Python client distribution

Reusable connectors and wheel builds are maintained on `main-cli`, not in this
simulator branch. From a checkout of `main-cli`:

```bash
uv build --wheel --out-dir build/python-wheel python
```

Install the supplied `ursoccerlab_client-0.1.1-py3-none-any.whl` into the Python 3.12
environment used for runtime checks. The wheel contains no simulator assets or
example programs. See [client installation](https://github.com/LuoShaoyinn/URSoccerLab/blob/main-cli/python/README.md).

## Compile source without packaging

```bash
python3 Tools/packaging/build_docker.py build
```

This builds both `URSoccerLab` and `URSoccerLabEditor` Development targets in
the container. It does not cook, stage or replace the AppImage. The engine and
repository are mounted from the host, so compiled outputs remain in the checkout.

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
Video requires a supported NVENC, QSV, VAAPI or Vulkan hardware encoder. The generic Vulkan loader is bundled; GPU drivers remain host-provided. The simulator is
headless and images are received through client connections.
