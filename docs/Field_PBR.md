# External field PBR maps and MuJoCo contacts

```json
"field": {
  "length_m": 9,
  "width_m": 6,
  "border_x_m": 0.8,
  "border_y_m": 0.9,
  "visual": {
    "base_color_map": "textures/field.png",
    "normal_map": "textures/grass_normal.png",
    "normal_format": "directx",
    "roughness_map": "textures/grass_roughness.png",
    "metallic_map": "textures/grass_metallic.png",
    "ao_map": "textures/grass_ao.png",
    "detail_tile_size_m": 0.5,
    "normal_strength": 1.0,
    "roughness": 0.8,
    "metallic": 0.0
  },
  "physics": {
    "friction": [1.0, 0.005, 0.0001],
    "condim": 3,
    "solref": [0.02, 1.0],
    "solimp": [0.9, 0.95, 0.001, 0.5, 2]
  }
}
```

`field`, positive dimensions, and `visual.base_color_map` are mandatory. This
replaces `field.map_image`; old configs receive a migration error. All other
visual keys and the `physics` object are optional. Defaults are shown above,
except no detail maps are supplied by default. Border defaults remain unchanged.

## Files and sampling

All paths are external files relative to the scene JSON directory, or absolute
paths. Moving the JSON requires moving its textures with it or adjusting paths.
The loader creates transient textures, never imports files into Content, and
never writes them into an AppImage. Any explicitly supplied unreadable/invalid
map aborts startup, including optional maps. Omit optional map keys to use their
neutral defaults; empty strings are rejected. PNG is recommended for data maps;
base color also accepts JPEG. Images must be single 2D images at most 8192x8192.

| Input | Meaning | Sampling |
|---|---|---|
| `base_color_map` | RGB bird-view field with markings and borders | sRGB; UV0 covers the entire surface once |
| `normal_map` | Tangent-space RGB normal vectors | Linear; DirectX by default, `opengl` flips green |
| `roughness_map` | 0 smooth, 1 rough, from red channel | Linear; replaces scalar `roughness` when present |
| `metallic_map` | 0 dielectric, 1 metal, from red channel | Linear; replaces scalar `metallic` when present |
| `ao_map` | 0 occluded, 1 unoccluded, from red channel | Linear; neutral default 1 |

Normal/roughness/metallic/AO are repeating detail maps sharing UV0 and a physical
repeat size of `detail_tile_size_m`. Their tiling follows field plus border
extents, so resizing does not stretch the detail. Scalars `roughness` and
`metallic` are fallback values in [0,1]; normal strength is [0,10], and repeat
size must be positive and finite. Absent normals are flat. No height/displacement
map is supported: geometry stays the original static Nanite surface.

Separate roughness and metallic maps are resampled into one transient G/B-packed
texture for the existing glTF shader; they can have different resolutions. Every
runtime texture gets a full CPU-generated mip chain. Data-map gamma is ignored;
base-color mip filtering is gamma-correct. Runtime textures currently use
uncompressed BGRA8, remain resident, and are not streamed from disk.

## Physics

`physics` applies to the hall's existing `field_ground` MuJoCo geom before model
compilation. Standalone worlds without that plane get a single runtime plane.
Visual maps do not affect collision geometry or friction. The plane remains
infinite; field dimensions do not create boundary walls or change match rules.

- `friction`: exactly three finite, nonnegative coefficients: sliding, torsional,
  rolling. Default matches the previous ground.
- `condim`: one of 1, 3, 4, 6. This is a geom setting; the contact dimension also
  depends on the other contacting geom.
- `solref`: two finite values, either both positive (time constant/damping ratio)
  or both nonpositive (direct stiffness/damping).
- `solimp`: exactly five finite values: d0, dwidth, width, midpoint, power. The
  first two and midpoint must be between 0 and 1, width positive, power >= 1.

Contact parameters combine with the ball/robot geom settings according to
[MuJoCo's contact rules](https://mujoco.readthedocs.io/en/stable/modeling.html#contact-parameters).
No contact priority override is added here. Parameters configure the ground
geom, rather than forcing identical coefficients for all generated pairs.

Use the existing scene apply/recompile flow to change physics on reload; editing
component values alone does not update a running compiled model. Loading new
maps needs no asset rebake. The updated runtime code must be built/cooked into a
future application release once; subsequent map changes stay external. This
change does not cook or update the AppImage.

## Verification

`Tools/runtime/test_field_pbr.py` generates synthetic external detail maps and
compares the field against a base-color-only scene. It records both robot and
inspector cameras plus a moving inspector video through the existing nDisplay
and AV1 path. Outputs live under `artifacts/tests/field-pbr-render/`. Native
automation under `URSoccerLab.Scene` covers JSON round-trip/rejection, texture
colour spaces, mip chains, channel packing, reload, and compiled ground contacts.
