#!/usr/bin/env python3
"""Package the URSoccerLab simulator into a Linux AppImage.

The AppImage contains the cooked hall, ball and generic runtime loaders
(TCP transport, MuJoCo, nDisplay, glTFRuntime). Robot packages, field textures
and scene configuration are external filesystem inputs. It excludes UnrealEditor,
py_example and Tools. GPU drivers come from the host (AMD or NVIDIA); the generic Vulkan loader is bundled.

Phases (each resumable; run with no subcommand to do all)::

    python Tools/packaging/package_appimage.py cook    # UAT BuildCookRun
    python Tools/packaging/package_appimage.py appdir  # assemble dist/AppDir
    python Tools/packaging/package_appimage.py image   # appimagetool -> .AppImage

Cook output, AppDir, and the final AppImage all land under the gitignored
``dist/`` directory.
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
EXTERNAL_PATTERNS = ("libvulkan_", "libGL.", "libEGL.", "libglapi", "libnvidia", "libdrm")


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
    # Some checkouts redirect diagnostics to ignored artifact directories.
    (ROOT / "Saved/MaterialStats").resolve().mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, DOTNET_ROLL_FORWARD="Major")
    return run(
        [
            str(RUN_UAT),
            "BuildCookRun",
            f"-project={PROJECT}",
            "-noP4",
            "-platform=Linux",
            "-clientconfig=Shipping",
            "-cook",
            "-stage",
            "-pak",
            "-compressed",
            "-package",
            "-build",
        ],
        cwd=ROOT,
        env=env,
    )


# --------------------------------------------------------------------------- #
APPRUN = (Path(__file__).with_name("AppRun")).read_text()


def _is_external(libname: str) -> bool:
    return any(pat in libname for pat in EXTERNAL_PATTERNS)


def _strip_external(tree: Path) -> int:
    removed = 0
    for p in tree.rglob("*.so*"):
        if p.is_file() and _is_external(p.name):
            p.unlink()
            removed += 1
    return removed


def _strip_release_diagnostics(tree: Path) -> None:
    """Keep build symbols in staging, but omit them and optional Vulkan layers.

    Vulkan rendering uses the host driver; these packaged layers are developer
    instrumentation, not the Vulkan loader or the application's encoder.
    """
    removed_bytes = 0
    removed_files = 0
    for p in tree.rglob("*"):
        if not p.is_file():
            continue
        if p.suffix in (".debug", ".sym") or (
            p.name.startswith("libVkLayer_") and ".so" in p.name
        ):
            removed_bytes += p.stat().st_size
            removed_files += 1
            p.unlink()
    log(f"omitted {removed_files} diagnostic files ({removed_bytes / 1024**2:.1f} MiB)")


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


def _bundle_vulkan_loader(appdir: Path) -> None:
    """Use Unreal's portable generic loader for both rendering and FFmpeg video."""
    import ctypes
    source = ENGINE / "Engine/Binaries/ThirdParty/Vulkan/Linux/libvulkan.so"
    loader = ctypes.CDLL(str(source))
    version = ctypes.c_uint32()
    if loader.vkEnumerateInstanceVersion(ctypes.byref(version)) != 0:
        raise RuntimeError("Cannot query bundled Vulkan loader version")
    major, minor, patch = version.value >> 22, (version.value >> 12) & 1023, version.value & 4095
    libdir = appdir / "usr/lib"; libdir.mkdir(parents=True, exist_ok=True)
    name = f"libvulkan.so.{major}.{minor}.{patch}"
    shutil.copy2(source, libdir / name)
    for alias in ("libvulkan.so", "libvulkan.so.1"):
        path = libdir / alias; path.unlink(missing_ok=True); path.symlink_to(name)
    fallback = appdir / "Engine/Binaries/ThirdParty/Vulkan/Linux/libvulkan.so"
    fallback.parent.mkdir(parents=True, exist_ok=True)
    fallback.unlink(missing_ok=True)
    fallback.symlink_to(os.path.relpath(libdir / name, fallback.parent))
    licenses = appdir / "usr/share/ursoccerlab"
    licenses.mkdir(parents=True, exist_ok=True)
    shutil.copy2(Path(__file__).with_name("Vulkan-Loader-LICENSE.txt"), licenses / "Vulkan-Loader-LICENSE.txt")
    log(f"bundled generic Vulkan loader {major}.{minor}.{patch}; GPU drivers remain external")


def _bundle_media_and_launcher(appdir: Path) -> None:
    """Ship minimal FFmpeg and jq dependencies, never the host GPU driver."""
    import re
    ffmpeg = Path(os.environ.get("URS_FFMPEG_ROOT", "/usr"))
    libraries = [ffmpeg / "lib" / (name + ".so") for name in ("libavcodec", "libavutil", "libswscale")]
    jq = shutil.which("jq")
    if not jq:
        raise RuntimeError("jq is required in the packaging container")
    bindir = appdir / "usr/bin"; bindir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(jq, bindir / "jq")
    share = appdir / "usr/share/ursoccerlab"; share.mkdir(parents=True, exist_ok=True)
    shutil.copy2(Path(__file__).with_name("atlas.jq"), share / "atlas.jq")
    libdir = appdir / "usr/lib"; libdir.mkdir(parents=True, exist_ok=True)
    excluded = ("libc.so", "libm.so", "libdl.so", "libpthread.so", "librt.so", "ld-linux", "libvulkan.so")
    for source in [Path(jq), *libraries]:
        if not source.is_file():
            raise FileNotFoundError(source)
        if source != Path(jq):
            real = source.resolve(); shutil.copy2(real, libdir / real.name)
            for alias in source.parent.glob(source.name + "*"):
                if alias.is_symlink():
                    dest = libdir / alias.name
                    dest.unlink(missing_ok=True); dest.symlink_to(real.name)
        result = subprocess.run(["ldd", str(source)], check=True, capture_output=True, text=True)
        if "not found" in result.stdout:
            raise RuntimeError(result.stdout)
        for name, path in re.findall(r"(\S+) => (/\S+)", result.stdout):
            if not name.startswith(excluded) and not _is_external(name):
                shutil.copy2(path, libdir / name)
    licenses = ffmpeg / "share/licenses"
    if licenses.is_dir(): shutil.copytree(licenses, share / "ffmpeg-licenses", dirs_exist_ok=True)


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

    # UAT gives Shipping executables a configuration suffix. Keep the public
    # launcher path identical across configurations without shipping two games.
    binaries = APPDIR / "URSoccerLab/Binaries/Linux"
    shipping = binaries / "URSoccerLab-Linux-Shipping"
    is_shipping = shipping.is_file()
    if is_shipping:
        shipping.replace(binaries / "URSoccerLab")
    (APPDIR / "usr/share/ursoccerlab").mkdir(parents=True, exist_ok=True)
    (APPDIR / "usr/share/ursoccerlab/build-config").write_text(
        "Shipping\n" if is_shipping else "Development\n"
    )

    apprun = APPDIR / "AppRun"
    apprun.write_text(APPRUN)
    apprun.chmod(apprun.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    _stage_third_party(APPDIR)
    _bundle_runtime(APPDIR)
    _bundle_media_and_launcher(APPDIR)
    _bundle_vulkan_loader(APPDIR)
    _write_desktop_and_icon(APPDIR)
    removed = _strip_external(APPDIR)
    log(f"removed {removed} external (vulkan/gpu) libs from AppDir")
    _strip_release_diagnostics(APPDIR)

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
    temporary = APPIMAGE.with_suffix(".AppImage.tmp")
    rc = run([tool, str(APPDIR), str(temporary)], cwd=ROOT, env=env)
    if rc == 0:
        temporary.replace(APPIMAGE)
    return rc


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
