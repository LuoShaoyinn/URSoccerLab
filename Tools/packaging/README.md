# URSoccerLab AppImage Packaging

Package the URSoccerLab simulator into a portable Linux AppImage. The AppImage
contains **only** the cooked game binary + all assets (field, ball, pi_plus +
mos9 robots) + runtime libs (MuJoCo, ZMQ, CoACD). It excludes the UnrealEditor,
`py_example/`, and `Tools/` scripts. The user provides the scene config and
Python clients externally. Vulkan + GPU drivers come from the host (AMD or
NVIDIA); they are intentionally NOT bundled.

## Portability

The packaged game binary targets **glibc ≥ 2.29** → runs on Ubuntu 20.04+,
22.04, 24.04, Arch, Fedora. This requires the third-party libraries (libmujoco,
libzmq, lib_coacd) to be built against a compatible glibc — see
"Rebuild third-party libs" below.

## Prerequisites

### Host
- `appimagetool` (`pacman -S appimagetool` on Arch).
- The URSoccerLab project checked out.

### Ubuntu 20.04 container (systemd-nspawn)
UE 5.7.4's build tools (UBT) require glibc ≥ 2.28; Ubuntu 20.04 (glibc 2.31)
satisfies this. The prebuilt engine (Epic release, `++UE5+Release-5.7`) needs
only glibc ≥ 2.27 to run.

Container setup (one-time):
```bash
# In the ubuntu20 container:
apt update && apt install -y build-essential git cmake python3-pip \
  libnss3 libnspr4 libxkbcommon0 libgbm1 libdrm2 libcups2 libxkbfile1 \
  libxshmfence1 libatspi2.0-0 libatk1.0-0 libatk-bridge2.0-0 libpangocairo-1.0-0 \
  libcairo2 libpango-1.0-0 libvulkan1 libgl1-mesa-dev libx11-dev libxcursor-dev \
  libxrandr-dev libxi-dev libxinerama-dev libxxf86vm-dev libxss-dev libxtst-dev \
  libxrender-dev libxext-dev libxfixes-dev libxdamage-dev libxcomposite-dev \
  libpulse-dev libasound2-dev libxerces-c-dev libfontconfig1 \
  libssl1.1 libtinfo5 libncurses5 libnuma1 libdbus-1-3 libcurl4 \
  libopenal1 libharfbuzz0b
pip3 install cmake==3.31.4        # CoACD needs cmake ≥ 3.24
add-apt-repository ppa:ubuntu-toolchain-r/test && apt update && apt install -y gcc-11 g++-11  # MuJoCo needs C++20
```

## Pipeline

### 1. Rebuild third-party libs (glibc portability)

The bundled third-party libs under `Plugins/UnrealRoboticsLab/third_party/install/`
must be rebuilt inside the ubuntu20 container so they target glibc 2.31 (not
the host's glibc). The `build.sh` scripts handle this:

```bash
# In the ubuntu20 container, as a non-root user matching the host uid:
cd Plugins/UnrealRoboticsLab/third_party
for dep in libzmq CoACD; do
  rm -rf $dep/src/build
  ( cd $dep && bash build.sh ../install Release --no-submodule-sync )
done
rm -rf MuJoCo/src/build
( cd MuJoCo && CC=gcc-11 CXX=g++-11 bash build.sh ../install Release --no-submodule-sync )
```

Result: libmujoco.so → GLIBC_2.29, libzmq.so → GLIBC_2.17, lib_coacd.so → GLIBC_2.29.

### 2. Cook + package (in the container)

```bash
# In the ubuntu20 container:
URS_UE=/home/luoshaoyinn/software/Unreal_Engine_5.7.4
bash "$URS_UE/Engine/Build/BatchFiles/RunUAT.sh" BuildCookRun \
  -project=$PWD/URSoccerLab.uproject -noP4 \
  -platform=Linux -clientconfig=Development \
  -cook -stage -pak -package -build \
  -archive -archdirectory=$PWD/_pack/packaged
```

The staged build lands in `Saved/StagedBuilds/Linux/`. Key config requirements
(already set in the repo):
- `Config/DefaultEngine.ini`: `GameDefaultMap=/Game/Levels/URS_SoccerField`
  + `+MapsToCook=(FilePath="/Game/Levels/URS_SoccerField")`.
- `Config/DefaultGame.ini`: `+DirectoriesToAlwaysCook` for `URSoccerLab/Robots`
  and `URSoccerLab/Objects` (dynamic robot/object Blueprint spawn).

> **Note:** Do NOT use a workspace directory named `build` (lowercase) — it
> collides with UE's `Build/` directory in the project root, causing a UBT
> `DirectoryItem.Scan` duplicate-key error. Use `_pack/` or `dist/` instead.

### 3. Assemble the AppImage (on the host)

```bash
ST=Saved/StagedBuilds/Linux
# AppRun: redirects Saved/ to a writable user dir; passes all args through
cat > "$ST/AppRun" <<'EOF'
#!/usr/bin/env sh
HERE="$(dirname "$(readlink -f "$0")")"
USERDIR="${URS_USER_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/URSoccerLab}"
mkdir -p "$USERDIR"
chmod +x "$HERE/URSoccerLab/Binaries/Linux/URSoccerLab" 2>/dev/null
exec "$HERE/URSoccerLab/Binaries/Linux/URSoccerLab" URSoccerLab -UserDir="$USERDIR/" "$@"
EOF
chmod +x "$ST/AppRun"
# Minimal .desktop + icon for appimagetool metadata
echo -e '[Desktop Entry]\nType=Application\nName=URSoccerLab\nExec=URSoccerLab\nIcon=URSoccerLab\nTerminal=true' > "$ST/URSoccerLab.desktop"
ARCH=x86_64 appimagetool "$ST" dist/URSoccerLab-Linux-x86_64.AppImage
```

### 4. Usage

```bash
./dist/URSoccerLab-Linux-x86_64.AppImage \
  -URSSceneConfig=$PWD/scene.json \
  -dc_cluster -dc_dev_mono -dc_cfg=match_4_rgb.ndisplay -dc_node=node_0 \
  -URSNDisplayCameras -URSNDisplayCameraCount=4 \
  -RenderOffscreen -NoSound
# Then connect a Python client to TCP port 10000 (walker) / 10001 (observer)
```

## Known issues

- **Observer camera (robot_rp1) gap in packaged build**: the nDisplay camera
  binder's `TryBindCameras` disables all MjCamera SceneCaptures before
  confirming the full binding succeeds. If the binding doesn't complete (timing
  gap with the second robot's cameras), the observer's cameras are left
  disabled without nDisplay coverage. This does NOT affect editor-based
  launches (`run_scene.py`). Walker (robot_rp0) cameras always work.
- The `write_video` helper gracefully skips empty frame lists (warns instead
  of crashing), so examples that save observer video don't abort on this gap.
