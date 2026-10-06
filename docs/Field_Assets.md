# External field maps

`external/field/example.png` is a simple 9 x 6 metre pitch with 0.8 metre X borders and
0.9 metre Y borders. It is an external example input, not a cooked Unreal asset.
The `external/` directory is Git-ignored and the AppImage does not bundle it. Distribute your scene JSON and
image together, preserving the relative `field.visual.base_color_map` path.

The complete image covers the pitch plus borders. Image left/right correspond
to MuJoCo -X/+X; image top/bottom correspond to MuJoCo +Y/-Y. The center pixel
maps to world (0, 0). Images are stretched over the specified physical extent;
markings are not detected or generated automatically. Prefer an image aspect
ratio matching `(length_m + 2*border_x_m)/(width_m + 2*border_y_m)`.

Grass PBR source maps are in `external/field/grass1-ue/`. Four unused, previously
imported Unreal texture assets are preserved in `external/field/legacy_unreal/`
as a local backup; `.uasset` files are not runtime image inputs. They have no
asset referencers and were removed from Content to keep them out of future cooks.
