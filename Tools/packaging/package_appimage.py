#!/usr/bin/env python3
"""Package the URSoccerLab simulator into a Linux AppImage.

The AppImage contains the cooked hall, ball and generic runtime loaders
(TCP transport, MuJoCo, nDisplay, glTFRuntime). Robot packages, field textures
and scene configuration are external filesystem inputs. It excludes UnrealEditor,
py_example and Tools. Vulkan/GPU drivers come from the host (AMD or NVIDIA);
they are intentionally NOT bundled.

Phases (each resumable; run with no subcommand to do all)::

    python Tools/packaging/package_appimage.py cook    # UAT BuildCookRun
    python Tools/packaging/package_appimage.py appdir  # assemble build/AppDir
    python Tools/packaging/package_appimage.py image   # appimagetool -> .AppImage

Cook output, AppDir, and the final AppImage all land under the gitignored
``build/`` directory.
"""
from __future__ import annotations

import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path

# NOTE: must NOT be named "build" (lowercase) — it collides with UE's Build/
# directory in UBT's DirectoryItem.Scan (duplicate-key error). See
# Tools/packaging/README.md.
ROOT = Path(__file__).resolve().parents[2]
PROJECT = ROOT / "URSoccerLab.uproject"
BUILD = ROOT / "dist"
STAGED = ROOT / "Saved" / "StagedBuilds" / "Linux"
APPDIR = BUILD / "AppDir"
APPIMAGE = BUILD / "URSoccerLab.AppImage"
ENGINE = Path(
    os.environ.get(
        "URS_UE",
        str(Path.home() / "software/Unreal_Engine_5.7.4"),
    )
)
RUN_UAT = ENGINE / "Engine/Build/BatchFiles/RunUAT.sh"

# Third-party runtime libs that must ship (copied from third_party/install if UAT
# didn't stage them). {soname: source glob relative to Plugins/.../install}.
THIRD_PARTY = {
    "libmujoco.so": "MuJoCo/lib/libmujoco.so",
    "libzmq.so": "libzmq/lib/libzmq.so",
    "lib_coacd.so": "CoACD/lib/lib_coacd.so",
}
INSTALL_ROOT = ROOT / "Plugins/UnrealRoboticsLab/third_party/install"

# Host-provided libs we must NOT bundle (external Vulkan / GPU drivers).
EXTERNAL_PATTERNS = ("libGL.", "libEGL.", "libglapi", "libnvidia", "libdrm")


def log(msg: str) -> None:
    print(f"[pkg] {msg}", flush=True)


def run(cmd: list[str], **kw) -> int:
    log("$ " + " ".join(str(c) for c in cmd))
    return subprocess.run([str(c) for c in cmd], **kw).returncode


# --------------------------------------------------------------------------- #
def phase_cook() -> int:
    if not RUN_UAT.is_file():
        raise FileNotFoundError(f"RunUAT.sh not found at {RUN_UAT} (set URS_UE)")
    BUILD.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, DOTNET_ROLL_FORWARD="Major")
    return run(
        [
            str(RUN_UAT),
            "BuildCookRun",
            f"-project={PROJECT}",
            "-noP4",
            "-platform=Linux",
            "-clientconfig=Development",
            "-cook",
            "-stage",
            "-pak",
            "-package",
            "-build",
        ],
        cwd=ROOT,
        env=env,
    )


# --------------------------------------------------------------------------- #
APPRUN = r"""#!/usr/bin/env sh
# URSoccerLab AppImage launcher.
# Bundles the UE game + robot/field content + runtime libs (MuJoCo, ZMQ, CoACD).
# Vulkan + GPU driver are provided by the HOST (AMD or NVIDIA); not bundled.
set -eu
HERE="$(dirname "$(readlink -f "$0")")"

# --- Scene config: either an explicit -URSSceneConfig=<path>, or the first
# positional argument treated as the scene JSON (./URSoccerLab.AppImage scene.json).
# $SCENE holds the absolute path for the nDisplay auto-setup below.
SCENE=""
for arg in "$@"; do
  case "$arg" in
    -URSSceneConfig=*) SCENE="${arg#-URSSceneConfig=}" ;;
  esac
done
if [ -z "$SCENE" ]; then
  if [ "$#" -gt 0 ] && [ -f "$1" ]; then
    case "$1" in
      /*) SCENE="$1" ;;
      *) SCENE="$(readlink -f "$1" 2>/dev/null || echo "$1")" ;;
    esac
    [ -z "$SCENE" ] && SCENE="$1"
    shift
    set -- "-URSSceneConfig=$SCENE" "$@"
  else
    echo "ERROR: scene config is required." >&2
    echo "Usage: $0 <scene.json> [additional UE args]" >&2
    echo "       (pass -URSSceneCapture to disable the nDisplay atlas)" >&2
    exit 1
  fi
fi

# --- nDisplay atlas auto-setup (default on) -------------------------------
# Generate a tightly-packed 640x480 atlas from the scene and enable the
# high-throughput nDisplay backend, so `AppRun <scene.json>` just works.
#   * Opt out with -URSSceneCapture / -URSNoNDisplay.
#   * An explicit -dc_cluster / -dc_cfg is respected as-is.
#   * If the scene can't be parsed (or awk is absent), we silently fall back
#     to per-UMjCamera SceneCapture, which is always functional.
force_sc=0; explicit_cluster=0
for arg in "$@"; do
  case "$arg" in
    -URSSceneCapture|-URSNoNDisplay) force_sc=1 ;;
    -dc_cluster|-dc_cluster=1|-dc_cluster=0|-dc_cfg=*) explicit_cluster=1 ;;
  esac
done
if [ "$force_sc" -eq 0 ] && [ "$explicit_cluster" -eq 0 ] && [ -n "$SCENE" ] && [ -f "$SCENE" ]; then
  _robots=$(awk '
    /"robots"[[:space:]]*:/ { in_r = 1 }
    /"objects"[[:space:]]*:/ { in_r = 0 }
    in_r && /"actor_id"[[:space:]]*:/ { c++ }
    END { print c + 0 }
  ' "$SCENE" 2>/dev/null || echo 0)
  if [ "${_robots:-0}" -gt 0 ]; then
    if grep -q '"mode"[[:space:]]*:[[:space:]]*"rgbd"' "$SCENE" 2>/dev/null; then
      _per=1
      _leftcam=$(awk -F'"' '/"left_camera"[[:space:]]*:/ {print $(NF-1)}' "$SCENE" 2>/dev/null)
      [ -z "$_leftcam" ] && _leftcam=left_eye
    else
      _per=2; _leftcam=""
    fi
    _views=$((_robots * _per))
    _cols=$(awk -v n="$_views" 'BEGIN{s=sqrt(n*4/3);c=int(s);if(s>c)c++;if(c<1)c=1;print c}')
    _rows=$(awk -v n="$_views" -v c="$_cols" 'BEGIN{r=int(n/c);if(n>c*r)r++;print r}')
    _W=$((_cols * 640)); _H=$((_rows * 480))
    _cfg="${TMPDIR:-/tmp}/urs_auto_${_views}_$$.ndisplay"
    {
      echo '{'
      echo '  "nDisplay": {'
      echo "    \"description\": \"URS auto atlas ($_views cameras)\","
      echo '    "version": "5.00",'
      echo '    "assetPath": "",'
      echo '    "misc": { "bFollowLocalPlayerCamera": false, "bExitOnEsc": true, "bOverrideViewportsFromExternalConfig": true, "bOverrideTransformsFromExternalConfig": true },'
      echo '    "scene": {'
      echo '      "xforms": {},'
      echo '      "cameras": { "DefaultViewPoint": { "interpupillaryDistance": 6.4, "swapEyes": false, "stereoOffset": "none", "parentId": "", "location": {"x":0,"y":0,"z":0}, "rotation": {"pitch":0,"yaw":0,"roll":0} } },'
      echo '      "screens": {}'
      echo '    },'
      echo '    "cluster": {'
      echo '      "primaryNode": { "id": "node_0", "ports": {"ClusterSync":41001,"ClusterEventsJson":41003,"ClusterEventsBinary":41004} },'
      echo '      "sync": { "renderSyncPolicy": {"type":"none","parameters":{}}, "inputSyncPolicy": {"type":"ReplicatePrimary","parameters":{}} },'
      echo '      "network": { "ConnectRetriesAmount":"10","ConnectRetryDelay":"100","GameStartBarrierTimeout":"30000","FrameStartBarrierTimeout":"30000","FrameEndBarrierTimeout":"30000","RenderSyncBarrierTimeout":"30000" },'
      echo '      "nodes": {'
      echo '        "node_0": {'
      echo '          "host": "127.0.0.1", "sound": false, "fullScreen": false,'
      echo "          \"window\": {\"x\":0,\"y\":0,\"w\":$_W,\"h\":$_H},"
      echo '          "postprocess": {},'
      echo '          "viewports": {'
      _i=0
      while [ "$_i" -lt "$_views" ]; do
        [ "$_i" -gt 0 ] && echo ','
        _c=$((_i % _cols)); _r=$((_i / _cols))
        _x=$((_c * 640)); _y=$((_r * 480))
        printf '            "camera_%02d": { "camera": "DefaultViewPoint", "bufferRatio": 1, "gPUIndex": -1, "allowCrossGPUTransfer": false, "isShared": false, "region": {"x":%d,"y":%d,"w":640,"h":480}, "projectionPolicy": {"type":"camera","parameters":{}} }' "$_i" "$_x" "$_y"
        _i=$((_i + 1))
      done
      echo ''
      echo '          },'
      echo '          "outputRemap": { "bEnable": false, "dataSource": "mesh", "staticMeshAsset": "", "externalFile": "" }'
      echo '        }'
      echo '      }'
      echo '    },'
      echo '    "customParameters": {},'
      echo '    "diagnostics": { "simulateLag": false, "minLagTime": 0.01, "maxLagTime": 0.3 }'
      echo '  }'
      echo '}'
    } > "$_cfg"
    if [ -s "$_cfg" ]; then
      set -- "$@" -dc_cluster -dc_dev_mono -dc_cfg="$_cfg" -URSNDisplayCameras -URSNDisplayCameraCount="$_views" -ForceRes -ResX="$_W" -ResY="$_H"
      [ -n "$_leftcam" ] && set -- "$@" "-URSNDisplayCameraName=$_leftcam"
    fi
  fi
fi

# --- nDisplay node resolution: inject -dc_node=node_0 whenever -dc_cluster is
# present. nDisplay refuses to auto-match a 127.0.0.1/localhost host (UE's
# GetResolvedNodeId deliberately skips loopback), so without an explicit node
# the game exits "Couldn't resolve node ID" -> KillImmediately.
have_dc_cluster=0; have_dc_node=0
for arg in "$@"; do
  case "$arg" in
    -dc_cluster|-dc_cluster=1|-dc_cluster=0) have_dc_cluster=1 ;;
    -dc_node=*) have_dc_node=1 ;;
  esac
done
if [ "$have_dc_cluster" -eq 1 ] && [ "$have_dc_node" -eq 0 ]; then
  set -- "$@" "-dc_node=node_0"
fi

# Gather all bundled lib dirs (bundled runtime first, then game + engine + plugins).
LIBS="$HERE/usr/lib:$HERE/URSoccerLab/Binaries/Linux:$HERE/Engine/Binaries/Linux"
for d in \
  "$HERE"/Engine/Plugins/*/*/Binaries/Linux \
  "$HERE"/Engine/Plugins/*/Binaries/Linux \
  "$HERE"/Plugins/*/*/Binaries/Linux \
  "$HERE"/Plugins/*/Binaries/Linux ; do
  [ -d "$d" ] && LIBS="$LIBS:$d"
done
export LD_LIBRARY_PATH="$LIBS:${LD_LIBRARY_PATH:-}"

# Redirect the writable Saved/ tree.
SAVE="${URS_SAVE_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/URSoccerLab}"
mkdir -p "$SAVE"

# Launch with baked runtime flags.
chmod +x "$HERE/URSoccerLab/Binaries/Linux/URSoccerLab" 2>/dev/null || true
exec "$HERE/URSoccerLab/Binaries/Linux/URSoccerLab" URSoccerLab \
  -saved="$SAVE" \
  -RenderOffscreen \
  -NoSound \
  -ExecCmds="DisableAllScreenMessages" \
  "$@"
"""


def _is_external(libname: str) -> bool:
    return any(pat in libname for pat in EXTERNAL_PATTERNS)


def _strip_external(tree: Path) -> int:
    removed = 0
    for p in tree.rglob("*.so*"):
        if p.is_file() and _is_external(p.name):
            p.unlink()
            removed += 1
    return removed


def _stage_third_party(appdir: Path) -> None:
    # Find the UnrealRoboticsLab plugin's Binaries/Linux dir in the staged tree.
    bin_dirs = list(appdir.glob("Engine/Plugins/**/UnrealRoboticsLab*/Binaries/Linux"))
    bin_dirs += list(appdir.glob("Plugins/**/UnrealRoboticsLab*/Binaries/Linux"))
    target = bin_dirs[0] if bin_dirs else appdir / "Engine/Binaries/Linux"
    target.mkdir(parents=True, exist_ok=True)
    for soname, rel in THIRD_PARTY.items():
        already = list(appdir.rglob(soname))
        if already:
            continue
        src = INSTALL_ROOT / rel
        if src.is_file():
            shutil.copy2(src, target / soname)
            log(f"staged third-party {soname} -> {target.relative_to(appdir)}")
        else:
            log(f"WARNING: third-party {soname} missing on disk ({src})")


# C++ runtime libs to bundle (for portability: libmujoco needs CXXABI_1.3.13
# = GCC 11+; bundling the build-host's libstdc++ makes the AppImage
# self-contained instead of depending on a new-enough host libstdc++).
BUNDLED_RUNTIME = ("libstdc++.so.6", "libgcc_s.so.1")


def _bundle_runtime(appdir: Path) -> None:
    libdir = appdir / "usr" / "lib"
    libdir.mkdir(parents=True, exist_ok=True)
    for soname in BUNDLED_RUNTIME:
        # Resolve via ldconfig to get the real file (may be a versioned symlink).
        import subprocess as _sp
        try:
            out = _sp.run(["ldconfig", "-p"], capture_output=True, text=True, check=True).stdout
        except Exception:
            out = ""
        path = None
        for line in out.splitlines():
            if line.rstrip().endswith(soname) and "=>" in line:
                path = line.split("=>", 1)[1].strip().split()[0]
                break
        if path and Path(path).is_file():
            shutil.copy2(path, libdir / soname)
            # Drop the SONAME symlink target's versioned name too if distinct.
            log(f"bundled {soname} -> usr/lib (from {path})")
        else:
            log(f"WARNING: {soname} not found via ldconfig; not bundled")


def _write_png(path: Path, w: int, h: int, rgb: tuple) -> None:
    import zlib, struct
    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))
    def chunk(t: bytes, d: bytes) -> bytes:
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw)))
        f.write(chunk(b"IEND", b""))


DESKTOP_ENTRY = """[Desktop Entry]
Type=Application
Name=URSoccerLab
Exec=AppRun
Icon=ursoccerlab
Categories=Development;Game;
Terminal=false
Comment=MuJoCo + Unreal Engine robot soccer simulator
"""


def _write_desktop_and_icon(appdir: Path) -> None:
    (appdir / "ursoccerlab.desktop").write_text(DESKTOP_ENTRY)
    # Simple solid icon (soccer-field green). appimagetool requires a real icon file.
    icon = appdir / "ursoccerlab.png"
    _write_png(icon, 256, 256, (40, 160, 80))
    # .DirIcon should be a square PNG; reuse the same image.
    shutil.copy2(icon, appdir / ".DirIcon")
    log("wrote ursoccerlab.desktop + icon")


def phase_appdir() -> int:
    if not STAGED.is_dir():
        raise FileNotFoundError(
            f"staged build not found at {STAGED}; run the 'cook' phase first"
        )
    if APPDIR.exists():
        shutil.rmtree(APPDIR)
    log(f"copying staged tree -> {APPDIR.relative_to(ROOT)}")
    shutil.copytree(STAGED, APPDIR, symlinks=True)

    apprun = APPDIR / "AppRun"
    apprun.write_text(APPRUN)
    apprun.chmod(apprun.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    _stage_third_party(APPDIR)
    _bundle_runtime(APPDIR)
    _write_desktop_and_icon(APPDIR)
    removed = _strip_external(APPDIR)
    log(f"removed {removed} external (vulkan/gpu) libs from AppDir")

    exe = APPDIR / "URSoccerLab" / "Binaries" / "Linux" / "URSoccerLab"
    log(f"AppDir ready; exe={exe.relative_to(ROOT)} exists={exe.is_file()}")
    return 0 if exe.is_file() else 1


# --------------------------------------------------------------------------- #
def phase_image() -> int:
    if not (APPDIR / "AppRun").is_file():
        raise FileNotFoundError(f"AppDir not assembled at {APPDIR}; run 'appdir' first")
    tool = shutil.which("appimagetool")
    if not tool:
        raise RuntimeError(
            "appimagetool not found in PATH. On Arch: pacman -S appimagetool."
        )
    APPIMAGE.parent.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, ARCH="x86_64")
    return run([tool, str(APPDIR), str(APPIMAGE)], cwd=ROOT, env=env)


PHASES = {"cook": phase_cook, "appdir": phase_appdir, "image": phase_image}


def main() -> int:
    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    if which == "all":
        for name, fn in PHASES.items():
            log(f"===== phase: {name} =====")
            rc = fn()
            if rc != 0:
                log(f"phase {name} failed (rc={rc})")
                return rc
        log(f"done -> {APPIMAGE.relative_to(ROOT)}")
        return 0
    if which in PHASES:
        return PHASES[which]()
    print(__doc__)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
