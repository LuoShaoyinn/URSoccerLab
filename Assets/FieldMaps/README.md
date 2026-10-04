# External field maps

`example.png` is a simple 9 x 6 metre pitch with 0.8 metre X borders and
0.9 metre Y borders. It is an external example input, not a cooked Unreal asset.
The AppImage does not bundle this directory. Distribute your scene JSON and
image together, preserving the relative `field.visual.base_color_map` path.

The complete image covers the pitch plus borders. Image left/right correspond
to MuJoCo -X/+X; image top/bottom correspond to MuJoCo +Y/-Y. The center pixel
maps to world (0, 0). Images are stretched over the specified physical extent;
markings are not detected or generated automatically. Prefer an image aspect
ratio matching `(length_m + 2*border_x_m)/(width_m + 2*border_y_m)`.
