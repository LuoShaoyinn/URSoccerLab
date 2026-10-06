# Booster K1 normalization

Convert the supplied archive into the existing external robot format:

```bash
uv pip install --python py_example/.venv/bin/python numpy trimesh mujoco
py_example/.venv/bin/python Tools/robots/normalize_booster.py \
  ../Booster_K1_URDF_PBR.zip --output external/robots/booster_k1
```

The output directory must be new. Source files are not modified. Generated
MJCF, STL, GLB, robot.json and normalization.json stay under ignored external/.
No editor imports, cook or AppImage rebuild are needed to create the package.

The converter preserves original link/joint names, full inertia tensors,
collision mesh scales, joint limits and effort limits. It compiles a physics-only
copy with MuJoCo, without balanceinertia; invalid authored inertia fails instead
of being silently changed. Mesh collision uses MuJoCo's convex mesh handling.
The two ankle-cross links have inertia but no source collision; this is recorded
in normalization.json rather than inventing collision geometry.

GLB nodes are grouped by original link name and exported without assembled-pose
transforms. Local geometry and original PBR materials stay intact. Source STL
and visual bounds in a common frame must agree within 3 mm; the supplied archive
agrees within 0.008 mm. The GLB is a posed static scene, not a skin. Its standing
pose and root height are not used as physics offsets. The default base height is
computed from the lowest mesh collision vertex at zero joint angles (~0.551924 m).

Stereo cameras are provisional at Head_2 local (0.1, +/-0.03, 0.08) metres,
640x480, 60-degree vertical FOV. The source contains no camera calibration.
Position servos use provisional kp=50, kv=1 with original URDF effort limits.
These are simulation defaults, not manufacturer motor specifications or a
validated standing controller. All 22 original joint names are retained,
including AAHead_yaw and Head_pitch.

Add the manifest to your scene:

```json
"robot_types": {
  "booster_k1": "../external/robots/booster_k1/robot.json"
},
"robots": [
  {"actor_id": "robot_rp0", "type": "booster_k1"}
]
```

Paths above assume scene.json lives in Config/. Explicit translation_m overrides
the package height. Robots use the existing network, nDisplay and encoding path.
