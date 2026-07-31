# Dynamic object source convention

Each non-robot dynamic object is maintained as one authoritative MJCF file
and an optional sibling `meshes/` directory:

```text
Assets/Objects/<object-type>/
├── <object-type>.xml
└── meshes/
    └── <visual-name>.glb
```

Physics uses simple MJCF collision geometry. Unreal-only GLB visuals are
attached with the same empty-frame convention used by robots:

```xml
<frame name="visual__soccer_ball"/>
```

This resolves to `meshes/soccer_ball.glb`. The visual must be exported at its
intended metric size and centered at its local origin. Dynamic objects are
listed in the scene configuration's `objects` array; unlike robots, they do
not create motor-command or camera TCP endpoints.
