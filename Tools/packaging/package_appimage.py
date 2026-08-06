#!/usr/bin/env python3
"""Package the URSoccerLab simulator into a Linux AppImage.

The AppImage contains ONLY the cooked simulator + robots (field, robot/ball
assets, runtime C++: TCP transport, MuJoCo, nDisplay binder). It excludes the
UnrealEditor, py_example, and Tools. External (user-provided at runtime): the
scene config (``-URSSceneConfig=``), the nDisplay config (``-dc_cfg=``), and
Python clients. Vulkan/GPU drivers come from the host (AMD or NVIDIA); they are
intentionally NOT bundled.

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

ROOT = Path(__file__).resolve().parents[2]
PROJECT = ROOT / "URSoccerLab.uproject"
BUILD = ROOT / "build"
PACKAGED = BUILD / "packaged"
STAGED = PACKAGED / "LinuxNoEditor" / "URSoccerLab"
APPDIR = BUILD / "AppDir"
APPIMAGE = BUILD / "URSoccerLab-Linux-x86_64.AppImage"
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
EXTERNAL_PATTERNS = ("libvulkan", "libGL.", "libEGL.", "libglapi", "libnvidia", "libdrm")


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
            "-archive",
            f"-archdirectory={PACKAGED}",
        ],
        cwd=ROOT,
        env=env,
    )


# --------------------------------------------------------------------------- #
APPRUN = """#!/usr/bin/env sh
# URSoccerLab AppImage launcher.
# Bundles the UE game + robot/field content + runtime libs (MuJoCo, ZMQ, CoACD).
# Vulkan + GPU driver are provided by the HOST (AMD or NVIDIA); not bundled.
set -e
HERE="$(dirname "$(readlink -f "$0")")"

# Gather all bundled lib dirs (engine + every plugin's Binaries/Linux).
LIBS="$HERE/Engine/Binaries/Linux:$HERE/Binaries/Linux"
for d in \\
  "$HERE"/Engine/Plugins/*/*/Binaries/Linux \\
  "$HERE"/Engine/Plugins/*/Binaries/Linux \\
  "$HERE"/Plugins/*/*/Binaries/Linux \\
  "$HERE"/Plugins/*/Binaries/Linux ; do
  [ -d "$d" ] && LIBS="$LIBS:$d"
done
export LD_LIBRARY_PATH="$LIBS:$LD_LIBRARY_PATH"

# Redirect the writable Saved/ tree (logs, generated nDisplay configs) to user data.
SAVE="${URS_SAVE_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/URSoccerLab}"
mkdir -p "$SAVE"

# Forward every user arg (e.g. -URSSceneConfig=... -dc_cluster -dc_cfg=...).
exec "$HERE/Binaries/Linux/URSoccerLab" -saved="$SAVE" "$@"
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
    removed = _strip_external(APPDIR)
    log(f"removed {removed} external (vulkan/gpu) libs from AppDir")

    exe = APPDIR / "Binaries/Linux/URSoccerLab"
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
