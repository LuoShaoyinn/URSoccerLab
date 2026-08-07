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

The `package_appimage.py` packager handles AppDir assembly, third-party lib
staging, external-lib stripping, and AppRun generation in one step:

```bash
# Point at the staged build and run the appdir + image phases
python Tools/packaging/package_appimage.py appdir
python Tools/packaging/package_appimage.py image
```

The packager reads the staged build from `build/packaged/LinuxNoEditor/URSoccerLab/`
(or `ArchivedBuilds/Linux/` if cooked via the container). Output:
`dist/URSoccerLab-Linux-x86_64.AppImage`.

The generated AppRun:
- **Requires** `-URSSceneConfig=<path>` (exits with an error if absent).
- Bakes in `-RenderOffscreen -NoSound -ExecCmds="MjCamera.AutoReadback 0,DisableAllScreenMessages"`.
- Sets up `LD_LIBRARY_PATH` for all bundled plugin libs.
- Redirects writable `Saved/` to `~/.local/share/URSoccerLab/`.
- Forwards all additional user args after the baked ones.

### 4. Usage

```bash
# Start the simulator (Terminal 1)
./dist/URSoccerLab-Linux-x86_64.AppImage \
  -URSSceneConfig=$PWD/py_example/examples/standing/scene.json

# Run a Python client (Terminal 2)
cd py_example
uv run python examples/standing/standing.py --port 10000 10001 --duration 5
```

TCP ports: `robot_rp0` = 10000, `robot_rp1` = 10001, admin = 11000.
If FUSE is unavailable on the host, append `--appimage-extract-and-run`.

## Known issues

- **nDisplay render manager**: the nDisplay camera binder does not initialise
  in standalone packaged binaries (`GetRenderMgr()` returns null). The
  simulator falls back to per-actor MjCamera readback, which works correctly
  for all robots and cameras. nDisplay flags (`-dc_cluster`, `-dc_cfg`, etc.)
  are not needed and not baked into the AppRun.
- **`write_video`** gracefully skips empty frame lists (warns instead of
  crashing), so examples that save observer video don't abort if no frames
  arrive.
- **FUSE**: if the host's FUSE setup prevents the AppImage from mounting
  (silent exit, no output), use `--appimage-extract-and-run` as a fallback.
  This extracts the squashfs to a temp directory first (~3 s overhead).
