# Synthetic turf field assets

Generate the default visual field and turf maps from the project root:

```bash
python3 Tools/field/generate_grass_assets.py
```

The generator writes a 9 x 6 m pitch with 0.8 m X borders and 0.9 m Y borders,
matching `Config/URS_scene.json`. The 5427 x 3994 base-color image covers the
pitch and borders. Its markings are 5 cm white inlays; the green surface
repeats the tracked, photo-like turf tile with restrained mowing bands and
field-scale color mottling. The default output uses deterministic seed
`20261007`.

## Choose what to deploy

The field bundle supports two use cases:

- **Complete prepared turf field:** use `external/field/example.png` for the
  complete ground appearance and painted white lines, add the optional normal,
  roughness and AO maps from `external/field/grass1-ue/`, and use the 3D blade
  mesh at `Assets/Scenes/SoccerField/visual/grass_blades.glb`. The default
  `Config/URS_scene.json` already references this combination.
- **Grass model only:** copy `grass_blades.glb` into the target project's asset
  bundle and add its path as `field.visual.grass_mesh`. Keep that project's
  existing `field.visual.base_color_map`, other material maps, and `field.physics`
  values. The GLB is geometry, not a ground texture, and does not contain the
  white lines.

For either mode, materialize the field's LFS assets after checking out this
branch:

```bash
git lfs install
git lfs pull --include="Assets/Scenes/SoccerField/visual/**,Assets/Scenes/SoccerField/physics/field_physics.xml,external/field/**"
```

If cloning a branch where unrelated LFS objects are unavailable, use
`GIT_LFS_SKIP_SMUDGE=1` for `git clone`, then run the filtered `git lfs pull`
above. That obtains the grass/field bundle without downloading the ball or
unrelated Unreal environment assets. A source build still needs the Unreal
project's own level, plugin and other required content assets.

The standard GLB leaves clearance along white markings for a 9 x 6 m pitch
with 0.8 m X and 0.9 m Y borders. The runtime scales the mesh to the dimensions
in the scene JSON, but that does not move its cutouts. If the existing field
uses different dimensions, regenerate with matching `--length-m`, `--width-m`,
`--border-x-m`, and `--border-y-m` values. Those options retain the standard
soccer-marking pattern; edit the generator's line definitions first if the
field uses a custom marking layout.

`external/field/grass1-ue/` contains seamless 0.5 m normal, roughness, and AO
tiles with 180,000 fiber strokes per square metre. `grass1-height.png` is an
authoring source only; runtime displacement is not supported. The
`Assets/Scenes/SoccerField/visual/grass_blades.glb` adds 180,000 individual 3D
turf fibers per square metre. Each low triangular fiber has a 1.8–2.4 mm root
width, 5–11 mm tip height, and 18–25 mm ground-plane lean. The jittered root
spacing is about 2.36 mm; overlapping fibers close the pile while keeping the
blades low. Fiber colors sample the same tracked albedo tile as the field
underlay, and field markings are kept clear for the white inlay beneath.
Signed-short positions and unindexed triangles keep the dense mesh at about
409 MiB. The mesh is visual-only and has no collision; `.gitattributes` stores
it through Git LFS. The pitch image and PBR maps under `external/field/` are
also tracked with Git LFS so the standard scenes work after a branch checkout
and `git lfs pull`.

The 3D mesh and PBR texture maps are visual assets. The Unreal runtime loads the
mesh as a non-colliding overlay and applies the maps to the field surface.
MuJoCo still uses the flat `field_ground` contact plane and configured friction;
grass appearance does not change robot or ball contact dynamics. The runtime
requires a build that includes the field-visual support already present in this
checkout.

Change dimensions with `--length-m`, `--width-m`, `--border-x-m`, and
`--border-y-m`; use those same values in the scene JSON. The generator sizes
the grass overlay and texture borders from those arguments. `--out-dir`
selects a different map location, `--pixels-per-m` controls base-map resolution
(default 512, maximum image axis 8192 px), `--turf-albedo-tile` selects the
seamless source tile, and `--seed` changes the procedural pattern. Generated
maps are under `external/field/` and are included in this feature branch. For a
custom scene outside this repository, copy the field bundle with the scene JSON
and preserve or update the relative `field.visual` paths.

Image left/right correspond to MuJoCo -X/+X; image top/bottom correspond to
+Y/-Y. The center pixel maps to world (0, 0). Keep the image aspect ratio
aligned with `(length_m + 2*border_x_m) / (width_m + 2*border_y_m)`.
