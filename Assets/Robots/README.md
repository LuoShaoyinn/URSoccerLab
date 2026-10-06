# External robot packages

Actual robot MJCF, meshes and materials are no longer tracked or cooked here.
Supply packages separately under `external/robots/` (Git-ignored), and reference
manifests through the scene's required `robot_types` declarations.

See [Robot package format](../../docs/Robot_Packages.md) for the manifest, GLB
mesh/geom convention, explicit inertia/collision checks and runtime tests.

Previous source files and cooked assets have been retained locally under
`external/robots/legacy_source/` and `external/robots/legacy_unreal/`.
