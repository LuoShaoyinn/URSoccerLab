#!/usr/bin/env python3
"""Generate local MOS-Brain MJCF copies with a root-body test camera.

The source assets live under refs/ and are intentionally not copied into git.
This script creates local generated XML files that still reference the original
mesh directories, but add one camera at the root robot body origin:

    <camera name="urlab_origin_camera" pos="0 0 0" fovy="90" resolution="640 480"/>
"""

from __future__ import annotations

import argparse
import os
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ASSETS = ROOT / "refs/mos-brain/simulation/mujoco/assets"
DEFAULT_OUT = ROOT / "Assets/MosBrainCameraTest"


ROBOTS = {
    "pi_plus": {
        "source": "robots/pi_plus/pi_plus.xml",
        "root_body": "base_link",
        "meshdir": "meshes",
    },
    "k1": {
        "source": "robots/k1/K1_22dof.xml",
        "root_body": "Trunk",
        "meshdir": "meshes",
    },
}


def indent(elem: ET.Element, level: int = 0) -> None:
    whitespace = "\n" + level * "  "
    child_whitespace = "\n" + (level + 1) * "  "
    if len(elem):
        if not elem.text or not elem.text.strip():
            elem.text = child_whitespace
        for child in elem:
            indent(child, level + 1)
        if not elem.tail or not elem.tail.strip():
            elem.tail = whitespace
    else:
        if level and (not elem.tail or not elem.tail.strip()):
            elem.tail = whitespace


def find_named_body(worldbody: ET.Element, name: str) -> ET.Element:
    for body in worldbody.iter("body"):
        if body.get("name") == name:
            return body
    raise RuntimeError(f"root body {name!r} not found")


def rel_posix_path(path: Path, start: Path) -> str:
    return Path(os.path.relpath(path.resolve(), start.resolve())).as_posix()


def generate_robot(robot: str, assets: Path, out_root: Path, width: int, height: int, fovy: float) -> Path:
    spec = ROBOTS[robot]
    src = assets / spec["source"]
    robot_out = out_root / robot
    robot_out.mkdir(parents=True, exist_ok=True)

    tree = ET.parse(src)
    root = tree.getroot()

    compiler = root.find("compiler")
    if compiler is None:
        compiler = ET.SubElement(root, "compiler")
    mesh_source = src.parent / spec["meshdir"]
    compiler.set("meshdir", rel_posix_path(mesh_source, robot_out))

    worldbody = root.find("worldbody")
    if worldbody is None:
        raise RuntimeError(f"{src} has no worldbody")
    root_body = find_named_body(worldbody, spec["root_body"])

    for existing in list(root_body.findall("camera")):
        if existing.get("name") == "urlab_origin_camera":
            root_body.remove(existing)

    camera = ET.Element(
        "camera",
        {
            "name": "urlab_origin_camera",
            "pos": "0 0 0",
            "fovy": f"{fovy:g}",
            "resolution": f"{width} {height}",
        },
    )
    root_body.insert(0, camera)

    indent(root)
    out_xml = robot_out / f"{robot}_urlab_origin_camera.xml"
    tree.write(out_xml, encoding="utf-8", xml_declaration=False)
    return out_xml


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--assets", type=Path, default=DEFAULT_ASSETS)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--robot", choices=sorted(ROBOTS), action="append")
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=480)
    parser.add_argument("--fovy", type=float, default=90.0)
    args = parser.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    robots = args.robot or sorted(ROBOTS)
    for robot in robots:
        out_xml = generate_robot(robot, args.assets, args.out, args.width, args.height, args.fovy)
        print(out_xml.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
