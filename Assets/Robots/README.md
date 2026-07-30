# Robot source convention

Each robot is maintained as one authoritative MJCF file and one `meshes/`
directory:

```text
Assets/Robots/<robot-type>/
├── <robot-type>.xml
└── meshes/
    ├── base_link.glb
    └── ...
```

URDF files and generated `_ue.xml` files are not part of the robot source.
The MJCF contains all physics, kinematic, naming, actuator, sensor, camera, and
visual-placement metadata. GLB files contain Unreal-only render geometry and
materials.

## Visual frames

MuJoCo does not support GLB mesh assets. Visuals are therefore declared with
standard, empty MJCF frames:

```xml
<body name="l_calf_link">
  <inertial .../>
  <joint name="l_calf_joint" .../>

  <frame
    name="visual__l_calf_link"
    pos="0 0 0"
    quat="1 0 0 0"/>

  <geom
    name="l_calf_collision"
    type="box"
    pos="0 0.00465 -0.011029"
    size="0.01495 0.01375 0.0309705"/>
</body>
```

The `visual__<name>` frame resolves to:

```text
meshes/<name>.glb
```

The frame must be placed under the body that owns the visual. Its MJCF
`pos` and orientation attributes are the visual's body-relative transform.
The frame must not contain child MJCF elements. Export GLBs at their intended
scale; arbitrary per-visual scale is intentionally unsupported.

Multiple visuals can be attached to one body by using distinct names:

```xml
<frame name="visual__base_link" pos="0 0 0"/>
<frame name="visual__base_link_cover" pos="0 0 0.12"/>
```

```text
meshes/base_link.glb
meshes/base_link_cover.glb
```

During editor import, URLab creates a transformed `UMjFrame` plus a
collision-disabled `UStaticMeshComponent`. MuJoCo dissolves the empty frame
during model compilation and never reads the GLB.

## Collision and naming

Collision geometry must be declared directly in MJCF using simplified
primitives such as boxes, capsules, cylinders, spheres, and planes. Do not use
the GLB visual as a MuJoCo collision mesh.

Body, joint, actuator, sensor, and camera names are runtime protocol
identifiers. Changing them requires updating controllers and scene
configuration. Unreal asset names may follow Unreal naming conventions, but
MJCF identifiers must be preserved exactly.

## Import

Import or rebuild the baked Unreal robot assets from the authoritative MJCF:

```bash
UnrealEditor URSoccerLab.uproject \
  -ExecutePythonScript=Tools/editor/import_robot.py
```

The importer reads the original XML directly and writes baked assets below
`/Game/URSoccerLab/Robots/`. It does not generate a rewritten XML file.
