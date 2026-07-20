#!/usr/bin/env python3
"""Generate local MOS-Brain MJCF copies with a robot-mounted test camera.

The source assets live under refs/ and are intentionally not copied into git.
This script creates local generated XML files that still reference the original
mesh directories, but add one camera mounted to the selected camera body,
looking outward along +X:

    <camera name="urlab_origin_camera" pos="0.06 0 0.04" xyaxes="0 -1 0 0 0 1" fovy="90" resolution="640 480"/>
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
        "camera_body": "head_pitch_link",
        "camera_pos": "0.06 0 0.04",
        "camera_xyaxes": "0 -1 0 0 0 1",
        "fixed_base": True,
        "meshdir": "meshes",
    },
    "k1": {
        "source": "robots/k1/K1_22dof.xml",
        "root_body": "Trunk",
        "camera_body": "Head_2",
        "camera_pos": "0.04 0 0.02",
        "camera_xyaxes": "0 -1 0 0 0 1",
        "fixed_base": False,
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


def remove_mesh_geoms(body: ET.Element, mesh_names: set[str]) -> None:
    for geom in list(body.findall("geom")):
        if geom.get("mesh") in mesh_names:
            body.remove(geom)


def ensure_child_body(parent: ET.Element, name: str, attributes: dict[str, str]) -> ET.Element:
    for body in parent.findall("body"):
        if body.get("name") == name:
            return body

    body = ET.Element("body", {"name": name, **attributes})
    parent.append(body)
    return body


def ensure_single_joint(body: ET.Element, name: str, attributes: dict[str, str]) -> None:
    for joint in list(body.findall("joint")):
        if joint.get("name") == name:
            body.remove(joint)
    body.insert(0, ET.Element("joint", {"name": name, **attributes}))


def ensure_single_geom(body: ET.Element, attributes: dict[str, str]) -> None:
    mesh_name = attributes.get("mesh")
    for geom in list(body.findall("geom")):
        if mesh_name and geom.get("mesh") == mesh_name:
            body.remove(geom)
    body.append(ET.Element("geom", attributes))


def ensure_motor(actuator: ET.Element, name: str, joint: str, ctrlrange: str) -> None:
    for motor in list(actuator.findall("motor")):
        if motor.get("name") == name or motor.get("joint") == joint:
            actuator.remove(motor)
    actuator.insert(
        0,
        ET.Element(
            "motor",
            {
                "name": name,
                "joint": joint,
                "gear": "1",
                "ctrllimited": "true",
                "ctrlrange": ctrlrange,
            },
        ),
    )


def remove_freejoints(body: ET.Element) -> None:
    for joint in list(body.findall("joint")):
        if joint.get("type") == "free":
            body.remove(joint)


def configure_pi_plus_head(root_body: ET.Element, root: ET.Element) -> None:
    remove_mesh_geoms(root_body, {"head_yaw_link", "head_pitch_link"})

    head_yaw = ensure_child_body(root_body, "head_yaw_link", {"pos": "0 0.00025 0.1981"})
    ensure_single_joint(
        head_yaw,
        "head_yaw_joint",
        {
            "pos": "0 0 0",
            "axis": "0 0 1",
            "range": "-1.39 1.39",
            "actuatorfrcrange": "-3 3",
            "damping": "0.02",
            "class": "arm_motor",
        },
    )
    ensure_single_geom(
        head_yaw,
        {
            "type": "mesh",
            "contype": "0",
            "conaffinity": "0",
            "group": "1",
            "density": "0",
            "rgba": "1 1 1 1",
            "mesh": "head_yaw_link",
        },
    )
    ensure_single_geom(head_yaw, {"type": "mesh", "rgba": "1 1 1 1", "mesh": "head_yaw_link"})

    head_pitch = ensure_child_body(head_yaw, "head_pitch_link", {"pos": "0 0 0.0255"})
    ensure_single_joint(
        head_pitch,
        "head_pitch_joint",
        {
            "pos": "0 0 0",
            "axis": "0 1 0",
            "range": "-0.34 1.57",
            "actuatorfrcrange": "-3 3",
            "damping": "0.02",
            "class": "arm_motor",
        },
    )
    ensure_single_geom(
        head_pitch,
        {
            "type": "mesh",
            "contype": "0",
            "conaffinity": "0",
            "group": "1",
            "density": "0",
            "rgba": "0.75294 0.75294 0.75294 1",
            "mesh": "head_pitch_link",
        },
    )
    ensure_single_geom(
        head_pitch,
        {"type": "mesh", "rgba": "0.75294 0.75294 0.75294 1", "mesh": "head_pitch_link"},
    )

    actuator = root.find("actuator")
    if actuator is None:
        actuator = ET.SubElement(root, "actuator")
    ensure_motor(actuator, "head_yaw_joint", "head_yaw_joint", "-3 3")
    ensure_motor(actuator, "head_pitch_joint", "head_pitch_joint", "-3 3")


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
    if spec.get("fixed_base", False):
        remove_freejoints(root_body)
    if robot == "pi_plus":
        configure_pi_plus_head(root_body, root)

    camera_body = find_named_body(worldbody, spec["camera_body"])

    for body in worldbody.iter("body"):
        for existing in list(body.findall("camera")):
            if existing.get("name") == "urlab_origin_camera":
                body.remove(existing)

    for existing in list(camera_body.findall("camera")):
        if existing.get("name") == "urlab_origin_camera":
            camera_body.remove(existing)

    camera = ET.Element(
        "camera",
        {
            "name": "urlab_origin_camera",
            "pos": spec["camera_pos"],
            "xyaxes": spec["camera_xyaxes"],
            "fovy": f"{fovy:g}",
            "resolution": f"{width} {height}",
        },
    )
    camera_body.insert(0, camera)

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
