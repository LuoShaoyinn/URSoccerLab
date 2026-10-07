# Coordinates, clocks and configuration

## World and camera frames

Scene positions use metres in the MuJoCo world: +X forward toward the opposing
goal, +Y left and +Z up. JSON quaternions use `[x, y, z, w]`; MJCF quaternions
use `w x y z`. The runtime converts to Unreal centimetres and flips handedness.
Do not pre-flip Y in a scene or client pose.

The field is centred at world zero. Its complete image covers the playing area
and both borders: image left/right map to -X/+X, image top/bottom to +Y/-Y.
Goal origins are ground-level centres of their openings; their width lies along
local Y and height along Z. Field resizing does not reposition goals automatically.

Guest poses use local +X viewing direction and +Z up. Robot camera optical axes
are defined in MJCF; see [camera integration](URLab_Builtin_Behavior.md).

## Configuration and external resources

The packaged simulator accepts exactly one scene JSON path. Paths inside that
file resolve relative to its directory; paths inside `robot.json` resolve relative
to the robot package. Absolute paths are accepted.

The hall and default ball mesh/skin are bundled. Robot packages, the field map
and optional field/ball PBR overrides remain external. A scene must specify the
field and both goal poses. Each spawned robot requires an external package.
Ball overrides are optional. See [scene reference](URSoccerLab_Scene_Building_Api.md).

Edit JSON or assets and restart to load changes. There is no file watcher.
Dimensions, contact parameters and MJCF compile at scene startup; texture changes
need no cook. Changes to application code or the bundled hall require a new build.
Admin pose/reset operations and guest camera movements act on the running scene.

## Independent clocks

MuJoCo stepping, network state publication and camera capture have independent
rates. State normally publishes at 60 Hz; supplied scenes configure RGB and guests
at 30 Hz. Render or encode load can lower delivered FPS without blocking physics.
Bounded queues discard unavailable or stale camera work rather than building latency.

`sim_time` on camera messages is sampled at consumption. It does not establish
exact exposure synchronization with state or separately captured depth. AV1
clients may wait for the next periodic keyframe before seeing their first image.
See [AV1 streaming](AV1_Runtime.md) and [runtime architecture](Runtime_Architecture.md).
