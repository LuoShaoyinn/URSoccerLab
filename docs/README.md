# User guide

URSoccerLab combines real-time MuJoCo physics with Unreal Engine camera rendering.
Start with a packaged simulator and a scene JSON, then connect robot or guest
clients. The guides below describe the current application; developer references
are grouped separately.

## Overview and getting started

- [Project overview and quick start](../README.md)
- [Installation, external resources and your first scene](Getting_Started.md)
- [Python client setup and examples](../py_example/README.md)

## Concepts and configuration

- [Coordinates, clocks and configuration lifecycle](Concepts.md)
- [Scene JSON reference](URSoccerLab_Scene_Building_Api.md)
- [Field size and bird-view map](Field_Assets.md)
- [Field PBR maps and ground contact physics](Field_PBR.md)
- [Ball size, mass and contact settings](URSoccerLab_Scene_Building_Api.md#ball-overrides)
- [Goalpost dimensions and poses](URSoccerLab_Scene_Building_Api.md#goalposts)

## Loading robots and textures

- [External robot package format, collision and inertia checks](Robot_Packages.md)
- [Normalize a Booster K1 package](../Tools/robots/README.md)
- [External ball PBR maps](URSoccerLab_Scene_Building_Api.md#external-ball-pbr-maps)

## Robot control and cameras

- [Robot commands, state and controller gains](../py_example/README.md#robotclient--motor-commands-state-and-camera)
- [Admin pose, reset and locking](URSoccerLab_TCP_Runtime.md#admin-rpc)
- [Guest floating cameras and recording](Guest_Cameras.md)
- [Rendering, lighting and camera effects](Rendering.md)
- [Video encoding, keyframes and depth](AV1_Runtime.md)

## Troubleshooting

- [Startup, missing resources, GPU encoding and logs](Getting_Started.md#troubleshooting)
- [Camera calibration and package rendering](Robot_Packages.md#rendering-and-calibration)

## Developers

- [Runtime architecture and thread boundaries](Runtime_Architecture.md)
- [TCP message and image protocol](URSoccerLab_TCP_Runtime.md)
- [URLab coordinates and camera integration](URLab_Builtin_Behavior.md)
- [Editor tools and runtime diagnostics](../Tools/README.md)
- [Docker build and AppImage packaging](../Tools/packaging/README.md)
- [Baked dynamic object convention](../Assets/Objects/README.md)
- [Historical experiments](experiments/README.md)
