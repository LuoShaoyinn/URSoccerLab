# URSoccerLab Viewer

A small native desktop client for watching URSoccerLab through a floating guest
camera, with an optional admin panel for actor poses. This `main-cli` branch is a
standalone C/C++ project. The simulator and its documentation remain on the
[main branch](https://github.com/LuoShaoyinn/URSoccerLab/tree/main).

The viewer uses SDL2, Dear ImGui and FFmpeg. It receives streamed images rather
than rendering the scene locally. Python, Unreal Engine and MuJoCo are not needed
to run the viewer. This branch also owns the reusable C/C++ connector libraries
and the [Python connector wheel](python/README.md).

## Build

### Linux

Install the development dependencies on Ubuntu, then build from the repository
root:

```bash
sudo apt install build-essential cmake pkg-config libsdl2-dev \
    libavcodec-dev libavutil-dev libswscale-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build -j
```

CMake downloads pinned Dear ImGui and cJSON sources with SHA256 checks. SDL2 and
FFmpeg come from the build environment. FFmpeg needs AV1, H.264 and HEVC decoders
for the corresponding server codecs; libdav1d is preferred for AV1.

### Windows

Build in an MSYS2 **UCRT64** terminal:

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-gcc \
    mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja \
    mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-SDL2 \
    mingw-w64-ucrt-x86_64-ffmpeg
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build
```

The result is a native `build/urs-viewer.exe`. Outside that terminal, dependent
DLLs must be next to the executable or on PATH. Windows compilation and packaging
have not been validated yet.

## Start the simulator and connect

Run the separately distributed simulator AppImage with a scene JSON that enables
`guest_inspector`. Start the viewer in another terminal:

```bash
./build/urs-viewer --host 127.0.0.1
```

On Windows use `build/urs-viewer.exe`. Replace localhost with the simulator's IP
for a remote connection; the server can run on Linux while the viewer runs on
Windows.

Click **Connect guest** using port **12000**. Video starts at a periodic keyframe.
The scene JSON determines guest resolution, frame rate and capacity; the client
does not negotiate them.

| Control | Action |
| --- | --- |
| Click the video | Capture the mouse |
| Mouse | Look around |
| WASD | Move forward/back/left/right |
| Space / left Ctrl | Move up/down |
| Left Shift | Move faster |
| Esc or losing window focus | Release the mouse |

Use the speed slider or **Reset camera** in the panel. Camera positions stay
within +/-99 metres; flight does not collide with hall geometry.

### Optional admin panel

Click **Connect admin** on port **11000**. Enter an actor ID from your scene, such
as `robot_rp0` or `ball`.

- **Get pose** fills the position and xyzw quaternion fields.
- **Set pose** applies those fields, leaving joint positions unspecified.
- **Reset actor** restores its initial pose.
- **Lock pose** holds the specified pose; **Unlock pose** releases it.

Replies use short status messages rather than raw JSON. Actor IDs are entered
manually. The panel does not discover actors, authenticate users, pause physics
or edit scene configuration.

For an admin panel without a guest view:

```bash
./build/urs-viewer --host 127.0.0.1 --admin-only
```

## Distribution status

This is a source preview, not a published portable package. The local Linux
MinSizeRel executable is approximately **678 KiB**, excluding shared dependencies.
A standalone distribution must also supply SDL/FFmpeg libraries and their required
license notices. Linux builds inherit their build environment's glibc and library
requirements; the local build is not an Ubuntu 22.04-compatible AppImage.

## Development

The project uses C99 and C++17, with a conventional root CMake build:

```text
CMakeLists.txt
include/               # C protocol and public C++ connector headers
src/                   # Protocol, transport, decoder, replies and GUI
tests/                 # Native/headless checks and video fixtures
python/                # Reusable Python package, wheel metadata and tests
cmake/                 # Installed CMake package configuration
```

The C library has no GUI, socket, JSON-parser or decoder dependency. C++ connection
workers own sockets and video decoding; they publish owned latest images. SDL
textures and UI remain on the main thread. Pending camera commands are replaced
by the newest pose; admin commands use a bounded queue. Reconnect manually after
a disconnect.

AV1/H.264/H.265 decoders reset on epoch, codec or sequence changes and wait for a
keyframe. Padded frames are cropped to visible dimensions. The GUI also accepts
raw BGRA and JPEG diagnostic images. It displays guest RGB only: it is not a
robot-eye/depth viewer. No server wire-format change is required.

### C protocol library

Include `urs_protocol.h` and link `urs_protocol`:

| API | Purpose |
| --- | --- |
| `urs_feed` | Incremental framing for partial/coalesced TCP reads |
| `urs_frame_header` | Outgoing big-endian length/type prefix |
| `urs_parse_images` | Version-2 RGB/depth metadata and payload views |
| `urs_parse_video` | Video epoch, coded dimensions, config and packet views |
| `urs_camera_json` | Validated guest pose command |
| `urs_admin_json` | Validated, escaped admin command |

Initialize `urs_parser` to zero and release it with `urs_parser_destroy`. Callback
payloads are borrowed and valid only during the callback. Malformed input or a
nonzero callback result returns -1; discard the parser/connection. Frames are
limited to 32 MiB. Image/video fields are little-endian. JSON formatting requires
the default C numeric locale.

To build only this library, without fetching GUI dependencies:

```bash
cmake -S . -B build -DURS_BUILD_VIEWER=OFF -DURS_BUILD_CONNECTOR=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

All native targets share `build/`; there is no separate protocol build directory.
Re-enable both options to restore the full viewer build.

### C++ connector

`URSoccerLab::client` provides the asynchronous `Client` API used by the viewer:
TCP connections, commands, replies and latest decoded RGB images. Public headers
live in `include/ursoccerlab/`. It currently provides the guest/admin workflow;
robot-specific high-level methods and stereo/depth splitting remain in Python.
The C protocol API is available for custom robot clients.

Build the connector without SDL/ImGui:

```bash
cmake -S . -B build -DURS_BUILD_CONNECTOR=ON -DURS_BUILD_VIEWER=OFF
cmake --build build
```

Install the C/C++ libraries and headers (plus the viewer when enabled) with:

```bash
cmake --install build --prefix /your/install/path
```

Consumers can then use the installed package:

```cmake
find_package(URSoccerLabClient CONFIG REQUIRED)
target_link_libraries(my_client PRIVATE URSoccerLab::client)
```

```cpp
#include <ursoccerlab/client.hpp>
Client guest;
guest.connect("127.0.0.1", 12000, true);
```

Point `CMAKE_PREFIX_PATH` at the install prefix. Installation does not collect
third-party shared libraries into a portable bundle.

### Python wheel

```bash
uv build --wheel --out-dir build/python-wheel python
python -m pip install build/python-wheel/ursoccerlab_client-0.1.1-py3-none-any.whl
```

Import `RobotClient`, `AdminClient` and `InspectorClient` from `ursoccerlab`.
See [Python API and development instructions](python/README.md).

### Validation

```bash
ctest --test-dir build --output-on-failure
```

The tests cover framing, metadata, command formatting, readable JSON replies and
three SDL/ImGui render iterations using SDL's dummy video driver. Dummy-video
initialization does not validate interactive mouse capture or real presentation.

Optional real AV1/H.264/H.265 decoder fixtures use Python only for testing:

```bash
python3 -m venv build/fixture-venv
build/fixture-venv/bin/python -m pip install -r tests/requirements.txt
build/fixture-venv/bin/python tests/interop.py build/client_probe
```

These fixtures exercise fragmented TCP delivery, camera JSON and visible-image
cropping. On Windows use the fixture environment's `Scripts/python.exe` and
`build/client_probe.exe`. They do not replace a test against the running simulator.

Original code is Apache-2.0. See [LICENSE](LICENSE), [NOTICE](NOTICE) and
[third-party notices](THIRD_PARTY_NOTICES.md).
