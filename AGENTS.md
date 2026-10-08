# Native client guide for agents

This branch (`main-cli`) is client-only. The `main` branch owns the Unreal/MuJoCo
simulator, example programs, resources and AppImage packaging. This branch owns
the C/C++ connector SDK and Python wheel source under `python/`. Do not restore simulator
source, Unreal assets or a submodule here, and do not merge client-only deletions
into the simulator branch unless explicitly requested.

## User workflow

Users run a separately distributed simulator AppImage with scene JSON, then use
this viewer on Windows or Linux. Guest port defaults to 12000; optional admin port
is 11000. Guests use mouse-look/WASD flight. The admin panel edits numeric pose
fields. Do not display changing raw JSON replies in the GUI. Camera resolution
and frame rate remain server-configured.

## Implementation

- Keep the C99 library in `include/urs_protocol.h` and `src/protocol.c` independent
  of socket, GUI, JSON-parser and decoder libraries.
- Use C++17, SDL2, Dear ImGui and FFmpeg for the desktop application. cJSON parses
  replies into stable status and pose data.
- Socket and video workers publish owned latest frames. SDL textures/UI stay on
  the main thread. Preserve partial sends and keyframe recovery after epoch,
  codec or sequence changes. Successful camera acknowledgements must not change
  GUI text at the pose-update rate.
- Keep POSIX/Winsock code paths. Windows instructions are provisional until a
  native Windows build is tested. Native executables use shared dependencies;
  do not promise a self-contained sub-1-MB download.
- Preserve the existing server wire format; compare against main if needed.

## Validation

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The fixture script and optional Python dependencies are described in
`README.md`; Python package tests are under `python/tests`. Keep all native
build targets in one `build/` tree. Dummy-video initialization is not interactive desktop
validation. Report which platform and connection behavior were tested.

`artifacts/`, `dist/`, `external/` and local simulator backups are ignored. Preserve
existing AppImages, resources, scenes and requested renders. Do not delete ignored
material merely to make the branch appear clean. No simulator repack is needed
for client changes. Commit, push and release only within the requested scope.
