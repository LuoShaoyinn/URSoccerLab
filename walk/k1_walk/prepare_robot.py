#!/usr/bin/env python3
"""Create the K1 walking robot package from the external Booster K1 package."""
import argparse
import shutil
import xml.etree.ElementTree as ET
from pathlib import Path

# Rotor inertias from MOS-Brain K1_22dof.xml, in policy joint names.
ARMATURE = {'AAHead_yaw': 0.002, 'Head_pitch': 0.002, 'ALeft_Shoulder_Pitch': 0.001, 'Left_Shoulder_Roll': 0.001, 'Left_Elbow_Pitch': 0.001, 'Left_Elbow_Yaw': 0.001, 'ARight_Shoulder_Pitch': 0.001, 'Right_Shoulder_Roll': 0.001, 'Right_Elbow_Pitch': 0.001, 'Right_Elbow_Yaw': 0.001, 'Left_Hip_Pitch': 0.0478125, 'Left_Hip_Roll': 0.0339552, 'Left_Hip_Yaw': 0.0282528, 'Left_Knee_Pitch': 0.095625, 'Left_Ankle_Pitch': 0.0565, 'Left_Ankle_Roll': 0.0565, 'Right_Hip_Pitch': 0.0478125, 'Right_Hip_Roll': 0.0339552, 'Right_Hip_Yaw': 0.0282528, 'Right_Knee_Pitch': 0.095625, 'Right_Ankle_Pitch': 0.0565, 'Right_Ankle_Roll': 0.0565}


def main():
    root = Path(__file__).resolve().parents[2] / "external" / "robots"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=root / "booster_k1")
    parser.add_argument("--output", type=Path, default=root / "booster_k1_armature")
    args = parser.parse_args()
    if args.output.exists():
        raise SystemExit(f"Output already exists: {args.output}; choose a new path")
    if not (args.source / "model.xml").is_file():
        raise SystemExit(f"Missing robot model: {args.source / 'model.xml'}")
    tree = ET.parse(args.source / "model.xml")
    joints = {node.get("name"): node for node in tree.findall(".//joint")}
    missing = ARMATURE.keys() - joints.keys()
    if missing:
        raise SystemExit(f"Missing K1 joints: {sorted(missing)}")
    shutil.copytree(args.source, args.output)
    for name, armature in ARMATURE.items():
        joints[name].set("armature", str(armature))
    tree.write(args.output / "model.xml", encoding="unicode")
    print(f"Created {args.output}")

if __name__ == "__main__":
    main()
