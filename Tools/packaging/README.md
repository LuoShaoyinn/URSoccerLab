# URSoccerLab AppImage Packaging

Package the URSoccerLab simulator into a portable Linux AppImage. The AppImage
contains **only** the cooked game binary + assets (hall, generic field/goal primitives, ball, pi_plus +
mos9 robots) + the C++ runtime libs it needs (MuJoCo, ZMQ, CoACD, libstdc++). It
excludes UnrealEditor, `py_example/`, and `Tools/`. The user supplies the scene
config, its required external field image, and Python clients externally.
The pitch image and former goal meshes are not cooked into the simulator.
Both goal poses and dimensions must be supplied in scene JSON. Vulkan + GPU drivers come from the host
(AMD or NVIDIA); they are intentionally NOT bundled.

## Usage

```bash
# Start the simulator (Terminal 1) — positional scene JSON:
./dist/URSoccerLab.AppImage py_example/examples/standing/scene.json

# ...or the explicit flag form:
./dist/URSoccerLab.AppImage -URSSceneConfig=$PWD/Config/URS_scene.json

# Run a Python client (Terminal 2)
cd py_example && uv run python examples/standing/standing.py
```

TCP ports: `robot_rp0` = 10000, `robot_rp1` = 10001, admin = 11000.
If the host's FUSE is unavailable, append `--appimage-extract-and-run`.

## Output

`dist/URSoccerLab.AppImage` (~665 MB, single file, executable).

## Portability

The cooked binary is produced inside the **Ubuntu 20.04 systemd-nspawn
container** (`URSoccerLab` machine) using the engine's bundled
`clang-20.1.8-rockylinux8` toolchain, so it targets glibc ≥ 2.28. The C++ runtime
(`libstdc++.so.6`, `libgcc_s.so.1`) from the build container is bundled in
`AppDir/usr/lib` and loaded first by `AppRun`, so the AppImage is self-contained
for the C++ ABI (libmujoco needs `CXXABI_1.3.13` = GCC 11+). No GPU/Vulkan
libraries are bundled — the host provides them, which is what makes the same
AppImage run on both AMD (RADV/Mesa) and NVIDIA (proprietary) stacks.

## Container one-time setup

Enter the container with `sudo start-URSoccerLab.sh` (NOPASSWD is scoped to this
wrapper). Both the project and the engine are bind-mounted under `/root/`.

```bash
export DEBIAN_FRONTEND=noninteractive
# focal main+updates+security + universe
M=https://mirrors.tuna.tsinghua.edu.cn/ubuntu
cat > /etc/apt/sources.list <<LIST
deb $M focal main restricted universe multiverse
deb $M focal-updates main restricted universe multiverse
deb $M focal-security main restricted universe multiverse
LIST
apt-get update -qq

# Runtime libs for the headless cook (UnrealEditor-Cmd) + the toolchain binary.
apt-get install -y --no-install-recommends \
  build-essential binutils file ca-certificates wget git xz-utils \
  libnss3 libnspr4 libxkbcommon0 libgbm1 libdrm2 libcups2 libxkbfile1 \
  libxshmfence1 libatspi2.0-0 libatk1.0-0 libatk-bridge2.0-0 libpangocairo-1.0-0 \
  libcairo2 libpango-1.0-0 libvulkan1 libgl1-mesa-glx libx11-6 libxcursor1 \
  libxrandr2 libxi6 libxinerama1 libxxf86vm1 libxss1 libxtst6 \
  libxrender1 libxext6 libxfixes3 libxdamage1 libxcomposite1 \
  libpulse0 libasound2 libfontconfig1 libnuma1 libdbus-1-3 libcurl4 \
  libharfbuzz0b libssl1.1 libstdc++6

# Newer libstdc++ (CXXABI_1.3.13) so libmujoco can dlopen during the cook.
add-apt-repository -y ppa:ubuntu-toolchain-r/test && apt-get update -qq
apt-get install -y --no-install-recommends libstdc++6

# Match host git behaviour (UBT parses `git status`; a Cyrillic-named skybox
# asset breaks UBT's path parser when git quotes non-ASCII paths).
git config --system core.quotepath false
```

Notes:
- **No compiler install is needed.** UE 5.7.4 bundles
  `clang-20.1.8-rockylinux8` at
  `Engine/Extras/ThirdPartyNotUE/SDKs/HostLinux/Linux_x64/v26_clang-20.1.8-rockylinux8`;
  UBT discovers it automatically. (`LinuxToolChain.cs` requires clang 20.x.)
- **No third-party rebuild is needed.** `libmujoco/libzmq/lib_coacd` already
  target GLIBC ≤ 2.29; the bundled libstdc++ covers their CXXABI needs.
- **UnrealEditor-Cmd refuses to run as root**, so the cook runs as a non-root
  user `urs` (uid 1000 == host `luoshaoyinn`, so files stay host-owned).

## Pipeline

All phases run **inside the container** via the packager
(`Tools/packaging/package_appimage.py`), each resumable:

```bash
# as urs (uid 1000):
runuser -u urs -- bash -lc '
  cd /root/URSoccerLab
  export URS_UE=/root/Unreal_Engine_5.7.4 DOTNET_ROLL_FORWARD=Major HOME=/home/urs
  python3 Tools/packaging/package_appimage.py cook     # UAT BuildCookRun (~20 min)
  python3 Tools/packaging/package_appimage.py appdir   # assemble dist/AppDir + bundle runtime
  python3 Tools/packaging/package_appimage.py image    # appimagetool -> dist/URSoccerLab.AppImage
'
```

- `cook` runs `BuildCookRun -cook -stage -pak -package -build`. The staged build
  lands in `Saved/StagedBuilds/Linux/` (read by the `appdir` phase).
- `appdir` copies the staged tree to `dist/AppDir`, writes `AppRun`, stages any
  missing third-party `.so`s, **bundles the build container's `libstdc++.so.6`
  + `libgcc_s.so.1` into `usr/lib`**, strips any stray bundled GPU/Vulkan libs,
  and adds a `.desktop` + icon.
- `image` runs `appimagetool`. `appimagetool` is **not** in the Ubuntu repos, so
  fetch the continuous AppImage and extract it once (no FUSE required):

  ```bash
  wget https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage -O /tmp/appimagetool
  chmod +x /tmp/appimagetool && cd /tmp && ./appimagetool --appimage-extract
  rm -rf /opt/appimagetool && mv /tmp/squashfs-root /opt/appimagetool
  ln -sf /opt/appimagetool/AppRun /usr/local/bin/appimagetool
  ```

> Do NOT use a workspace directory named `build` (lowercase) — it collides with
> UE's `Build/` directory and causes a UBT `DirectoryItem.Scan` duplicate-key
> error. The packager uses `dist/`.

## AppRun behaviour

- Accepts `./URSoccerLab.AppImage <scene.json>` (positional, resolved to an
  absolute path) **or** `-URSSceneConfig=<path>`. One of them is required.
- **Auto-enables nDisplay**: a bare `./URSoccerLab.AppImage <scene.json>` parses
  the scene, generates a tightly-packed 640×480 atlas to `$TMPDIR`, and injects
  every nDisplay flag (`-dc_cluster -dc_dev_mono -dc_cfg=... -URSNDisplayCameras
  -URSNDisplayCameraCount=N -ForceRes -ResX -ResY`, plus `-dc_node=node_0`).
  View count = robots × (2 for `stereo_rgb`, 1 for `rgbd`); `rgbd` also gets
  `-URSNDisplayCameraName=left_eye`. Pass **`-URSSceneCapture`** (or
  `-URSNoNDisplay`) to force the per-`UMjCamera` fallback instead. An explicit
  `-dc_cluster`/`-dc_cfg` from the caller is left untouched, and any parse
  failure silently falls back to SceneCapture — so the bare command always works.
- Bakes `-RenderOffscreen -NoSound -ExecCmds="DisableAllScreenMessages"`.
- Prepends `usr/lib` (bundled C++ runtime) then all game/engine/plugin lib dirs
  to `LD_LIBRARY_PATH`.
- Redirects the writable `Saved/` tree to `${URS_SAVE_DIR:-~/.local/share/URSoccerLab}`.
- Forwards all additional args to the game.

## nDisplay (high-throughput atlas) backend

The default `./URSoccerLab.AppImage <scene.json>` runs the production nDisplay
camera atlas (the AppRun generates the atlas from the scene — no extra flags).
Alternatives:

```bash
# Turnkey from a scene (generates atlas + launches, same as the bare command):
uv run --project py_example python Tools/runtime/run_scene.py \
  --appimage --scene-config Config/examples/six_robots_stereo_rgb.json

# Force the lower-throughput SceneCapture fallback:
./dist/URSoccerLab.AppImage Config/examples/six_robots_stereo_rgb.json -URSSceneCapture
```

Why `-dc_node=node_0` is needed: nDisplay's `GetResolvedNodeId` deliberately
refuses to match `127.0.0.1`/`localhost` (the generated node host), so without an
explicit node the game exits `Couldn't resolve node ID`. The AppRun injects it.

Verified: 6 robots / 12 cameras bind into one 2560×1440 atlas and stream stereo
RGB over TCP from the bare `AppImage <scene>` invocation.

## Known issues

- **FUSE**: if the host's FUSE setup prevents mounting (silent exit, no output),
  use `--appimage-extract-and-run` (~3 s extraction overhead).
