# Scene asset convention

The authoritative visual scene is maintained directly in Unreal:

```text
Content/
├── Levels/
│   └── URS_SoccerField.umap
└── URSoccerLab/Scenes/SoccerField/
    ├── Environment/   # building meshes, materials, textures, import metadata
    ├── Field/         # pitch and goal meshes, materials, textures
    ├── Lighting/      # scene-specific material assets
    └── Physics/       # Unreal-baked MuJoCo collision actor
```

Edit the level and its visual assets through Unreal Editor and commit the
resulting `.umap` and `.uasset` files. The original environment or field GLB is
not a maintained source artifact after import.

MuJoCo scene physics has a separate, intentionally minimal source:

```text
Assets/Scenes/SoccerField/physics/field_physics.xml
```

It contains the flat playing-plane collision model only. After changing it,
run `Tools/editor/bake_field_physics.py` to refresh the baked Unreal actor.

Generated import staging directories and `*_ue.xml` files must not be committed.
