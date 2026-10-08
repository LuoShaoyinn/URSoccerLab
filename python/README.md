# URSoccerLab Python connector

Install the supplied wheel in a Python 3.12 environment:

```bash
python -m pip install ursoccerlab_client-0.1.1-py3-none-any.whl
```

The package imports as `ursoccerlab`. The wheel contains Python code; pip installs
PyAV, NumPy and image/video helpers separately. It works on Windows and Linux
where compatible dependency wheels are available. No simulator assets are bundled.

Start the simulator AppImage separately with a scene JSON. The simulator and
[example programs](https://github.com/LuoShaoyinn/URSoccerLab/tree/main/py_example/examples)
remain on `main`; reusable connectors and wheel builds live on `main-cli`.

## Robot control, state and cameras

```python
from ursoccerlab import RobotClient

robot = RobotClient('127.0.0.1', 10000)
try:
    robot.send_command({'head_pitch_joint_servo': 0.1})
    for kind, data in robot.recv():
        print(kind, type(data))
finally:
    robot.close()
```

Robot ports start at 10000 in scene order. A connection carries commands, state,
RGB and optional independent lossless depth messages. Receive repeatedly in your
application loop. RGB streams use AV1/H.264/H.265; PyAV decodes them. Joining and
recovery wait for periodic keyframes. Stereo returns separate left/right images;
RGBD keeps left RGB and separate depth. Diagnostic JPEG/raw are also supported.

Use `ursoccerlab.media.camera_to_rgb(image)` for RGB arrays and
`depth_to_meters(image)` for depth. Controller settings are available through
`robot.set_controller_params(kp=..., kv=..., damping=..., actuator_mode='position')`.
The default actuator mode is torque. Gain presets `PI_PLUS`, `MOS9` and
`ursoccerlab.gains.detect_gains` remain available.

## Admin commands

```python
from ursoccerlab import AdminClient

admin = AdminClient('127.0.0.1', 11000)
try:
    pose = admin.get_pose('robot_rp0')
    admin.set_pose('robot_rp0', translation_m=[0, 0, 0.55])
    admin.reset('robot_rp0')
finally:
    admin.close()
```

`lock_pose` / `unlock_pose` are also available. Actor IDs must match the scene.

## Guest camera

```python
from ursoccerlab import InspectorClient
from ursoccerlab.media import camera_to_rgb

with InspectorClient('127.0.0.1', 12000) as guest:
    guest.set_camera([-4, 0, 2], [0, 0, 0, 1])
    for kind, data in guest.recv():
        if kind == 'rgb':
            rgb = camera_to_rgb(data[0])
```

Poll `recv()` repeatedly. Camera poses use MuJoCo-world metres and xyzw
quaternions: +X forward, +Z up. The server JSON sets frame rate and resolution.
Guests have no robot or admin commands.

## Build and validate

From the client branch root:

```bash
uv build --wheel --out-dir build/python-wheel python
uv sync --project python --frozen
uv run --project python python -m unittest discover -s python/tests
```

The output is `build/python-wheel/ursoccerlab_client-0.1.1-py3-none-any.whl`. For video fixture
tests, PyAV must contain the software encoders used by those tests. This is separate
from the server's hardware-encoder policy. Optional ML extras retain the original
example dependencies; they are not needed for basic connection or decoding.
