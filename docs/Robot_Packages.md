# External robot packages (urs_robot_v1)

The application ships a generic articulation loader, not robot models. Robot
MJCF, GLB geometry, textures and package manifests are filesystem inputs outside
the cook. There are no built-in robot types or fallback models.

A scene explicitly maps aliases to manifests, with paths relative to scene.json:

```json
"robot_types": {
  "pi_plus": "../external/robots/pi_plus/robot.json"
},
"robots": [{"actor_id":"robot_rp0", "type":"pi_plus"}]
```

The alias must match the manifest id. Package paths are relative to robot.json;
MJCF mesh/texture paths follow the model compiler's meshdir/texturedir/assetdir.
Absolute paths also work. Missing packages/assets fail startup.

```text
external/robots/pi_plus/
├── robot.json
├── model.xml
└── meshes/*.glb
```

```json
{
  "version": "urs_robot_v1",
  "id": "pi_plus",
  "model": "model.xml",
  "default_base_height_m": 0.3762,
  "bindings": {
    "base_body": "base_link",
    "head_body": "head_pitch_link",
    "left_camera": "left_eye",
    "right_camera": "right_eye"
  }
}
```

All bindings are validated against the compiled physics model. Original body,
joint, actuator and sensor names remain intact; every instance receives its
actor-id prefix. Camera bindings map to the scene's existing left/right camera
channel names. Robot and guest streams keep the shared nDisplay/AV1 pipeline.
Bound cameras must belong to the declared root body and use fixed perspective
projection at 640x480 in this version.

## Explicit visuals in MJCF

```xml
<mujoco>
  <compiler angle="radian" meshdir="meshes" inertiafromgeom="false"/>
  <asset>
    <mesh name="head_visual" file="head.glb" scale="1 1 1"/>
  </asset>
  <worldbody>
    <body name="head_link">
      <inertial pos="0 0 0" mass="0.2" diaginertia="0.001 0.001 0.001"/>
      <geom name="head_collision" type="box" size="0.04 0.05 0.03"/>
      <geom name="head_visual_geom" type="mesh" mesh="head_visual"
            pos="0 0 0" quat="1 0 0 0"
            contype="0" conaffinity="0" mass="0"/>
    </body>
  </worldbody>
</mujoco>
```

The loader selects `.glb` mesh assets and their referencing geoms for Unreal.
It removes them from the MuJoCo input without changing the source XML. Temporary
sites let MuJoCo resolve each visual's body-relative pose through nested frames
and geom defaults; they are removed before attaching the final physics spec.
Includes are expanded with cycle checks. Existing MJCF physics definitions
(joints, inertia, collision geoms, contacts, actuators, sensors, etc.) are
compiled directly, rather than reconstructed from Unreal meshes.

Checks:

- Every joint-owning body needs an explicit `<inertial>`. Invalid inertia is
  additionally checked by MuJoCo's compiler. `inertiafromgeom=true` is rejected;
  the physics copy uses `inertiafromgeom=false`.
- GLB geoms must resolve to `contype=0` and `conaffinity=0`, including defaults.
  A nonzero explicit/inherited mass is rejected. Omitted mass is safe because
  visual geoms are removed and inertia inference is disabled.
- Fixed bodies with geoms but no explicit inertia warn that they contribute
  no mass.
- Collision comes exclusively from remaining MJCF geoms. A model without any
  collision-enabled geom produces a startup warning.
- Missing GLBs, invalid bindings and invalid/nonpositive mesh scales fail.
  GLB mesh `refpos`/`refquat` and visual `fromto` are unsupported; use geom pose.
- Physics references to removed visual geoms fail compilation rather than being
  silently redirected. Names beginning `__urs_visual_` are reserved.

The package XML requires this loader before standard MuJoCo can open it, because
MuJoCo does not accept GLB mesh assets directly. Primitive geoms and MuJoCo's
supported mesh formats remain valid for collision.

GLBs use metres and their glTF Y-up axes. The loader matches the previous
Interchange importer: GLB X -> Unreal X, GLB Y -> Unreal Z, GLB Z -> Unreal Y.
In MuJoCo terms this is GLB X -> X, Y -> Z, Z -> -Y. Geom pose/scale is applied
in the MuJoCo body frame. Pose quaternions in MJCF are wxyz; scene JSON remains
xyzw. GLB internal node transforms and PBR materials are loaded by glTFRuntime.
Visual meshes are shared by robot instances of the same loaded package.
UV-less meshes with normals receive a finite orthonormal tangent basis; authored
UVs and tangents are preserved. GLBs without explicit materials use glTF
default PBR values rather than Unreal's WorldGrid material.

The original Pi Plus GLBs omitted the editor-only powder-coated-metal override.
`Tools/runtime/restore_pi_plus_pbr.py` migrates that finish into the external
package using shared PNGs and standard GLB PBR material references. It packs
roughness/metallic channels and converts DirectX normals to glTF normals. Existing
UVs are retained; meshes without UVs keep constant UV0, matching the old material
override, with explicit finite tangents. Height was not wired into the old
material and is not added here. All generated robot textures remain outside Git
and the application cook.

## Distribution and validation

`external/` is Git-ignored. pi_plus and mos9 have been migrated locally into
`external/robots/`; previous source and cooked assets are preserved as local
backups in `legacy_source/` and `legacy_unreal/`. Fresh checkouts need separately
supplied robot packages. These backups are not runtime inputs.

The project's robot Content directory is removed and marked NeverCook. Only
loader code and generic glTFRuntime material templates belong in the application.
The Docker packager cooks the generic loader and materials; robot files remain
external inputs. See [Getting started](Getting_Started.md) for packaged startup.

The third-party runtime loader is vendored from
https://github.com/rdeioris/glTFRuntime at
`cef7be26f803b1728508218957ea9d1baf01400d`, under its MIT license.

Run native `URSoccerLab` automation for configuration and physics validation.
With the two external packages installed, run:

```bash
python Tools/runtime/test_external_robots.py
python Tools/runtime/test_av1.py
```

The first test drives deterministic locked poses with moving head joints for a
render fixture, and records both robot cameras and a guest view under
`artifacts/tests/external-robots/`. The second checks stereo, RGBD, periodic
keyframes and late connections. Neither cooks the application.

### Rendering and calibration

All robot visuals and hall lights use lighting channel zero. Hall illumination
uses separate emissive lamp meshes with Lumen surface-cache coverage. Robot
camera placement and field/robot material settings remain package-specific.
The normalized Booster K1 cameras and servo gains are provisional and need
calibration against the real robot; loading/rendering validation does not prove
stable locomotion.
