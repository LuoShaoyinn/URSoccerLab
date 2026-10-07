# Guest cameras

Guest cameras are available in the packaged AppImage and the source runtime.
Start with a scene JSON, then connect the Python inspector receiver.

## Guest sessions

Use one TCP listener on port **12000**, alongside robot ports 10000 + index and
admin 11000. Each accepted connection owns one floating camera, with a stable
session ID. Several guests can connect to the same port with independent views.
There is no MuJoCo body, collision, actuator, robot state subscription, or admin
capability. Closing a connection releases its camera slot. Firewall rules can
expose the inspector listener while keeping robot/admin ports private.

The inspector parser accepts only `set_camera`. Reject motor, gain, actor pose,
reset, and other commands before they can reach any core/admin queue. Client API
restrictions alone do not enforce guest permissions. Validate the inspector port
against all robot/admin listeners. Camera admission and resource limits are
server-owned; guests cannot request arbitrary resolution, FPS, or codecs.

## Configure guest cameras

Add this section to the scene JSON:

```json
"encoder": {"codec": "av1", "backend": "auto", "fallback_codec": "h264"},
"guest_inspector": {
  "enabled": true,
  "port": 12000,
  "max_guests": 4,
  "width": 640,
  "height": 480,
  "rate_hz": 30,
  "fov_degrees": 90,
  "bitrate_kbps": 2000,
  "keyframe_interval_s": 2
}
```

These values are the defaults. Restart after changing them. Width/height must
be even, width 64–1920, height 64–1080, capacity 1–4, FPS 1–120 and FOV 10–150
degrees. The port must not overlap robot/admin listeners. Disabling guests avoids
reserving their atlas views. A new AV1 guest waits for a periodic keyframe;
changing pose does not force a keyframe. [AV1 streaming](AV1_Runtime.md) describes
the encoding and delivery settings.

## V1 protocol

Reuse TCP framing: `[BE u32 length including type][u8 type][payload]`.
Client JSON (type 0) is:

```json
{"version":1,"command":"set_camera","args":{"translation_m":[-4,0,2],"rotation_quat_xyzw":[0,0,0,1]}}
```

Coordinates use MuJoCo metres, +X forward, +Y left, +Z up; quaternions are xyzw.
The camera's local +X is its viewing direction and local +Z is up. The server
converts into Unreal coordinates using the existing world conversion helpers.
Validate numeric finite values, nonzero quaternion, and +/-100 metre bounds on
each position axis;
normalize orientation. A request changes only the sender's camera. The first
valid pose enables capture; no implicit default view is streamed before it.

Server JSON replies are `{"version":1,"ok":true,"command":"set_camera"}` or
`{"version":1,"ok":false,"command":"set_camera","error":"reason"}`.
An acknowledgement means the game thread applied the pose, not that a matching
image has already arrived. Allow only one pending pose update per session and
at most 30 requests per second.
Excess updates receive an immediate busy error, which can precede the pending
update acknowledgement; clients should wait for acknowledgement before sending
another update.
Malformed outer framing closes that connection. Invalid commands produce an
error without affecting other sessions.

Server RGB (type 1) reuses the existing v2 image payload: one image named
`inspector`, sequence, sim_time, dimensions, AV1/H.264/H.265 packets, JPEG or raw BGRA8 bytes. No robot
state or depth messages. Simulation time is sampled at frame consumption, with
the same synchronization limits as robot cameras. Sequences are per session;
reconnecting creates a new session. JSON/RGB can interleave. The server preset is
640x480, at most 30 Hz, the shared encoder at 2000 kbps, and four guest
sessions. Encoding is globally capped at four jobs, including jobs from recently
disconnected sessions. Configure startup limits using the `guest_inspector` scene JSON section (see [AV1 runtime](AV1_Runtime.md)). The AppImage accepts only the JSON path; configure the port and codec in JSON.
Clients cannot raise resource limits.

## Developer implementation boundaries

- `FInspectorProtocol` decodes typed camera poses and produces status replies.
  Robot/admin parsers and queues do not participate in guest commands.
- `IURSInspectorNetwork` is a replaceable transport contract. Its TCP worker owns
  sockets, framing, connection IDs, admission, and backpressure. Camera lifecycle
  and pose events cross bounded queues to the game thread.
- `UURSInspectorCameraComponent` assigns each session a reserved nDisplay camera
  slot. Guests use the robot camera policy, post-processing, shared atlas GPU
  readback, and `FImageEncoder`; no separate SceneCapture/render target exists.
  The generated atlas reserves `max_guests` viewports at the configured width and height. These render at the
  engine frame rate; streaming follows `guest_inspector.rate_hz` (default 30 Hz). Reserved views increase render
  cost even without connected guests. Older custom layouts without `guest_00`
  through `guest_03` continue to support robots but cannot admit all four guests.
- Encoders own pixels and mailbox handles, never UObjects. Session IDs and camera
  generations discard results for closed or obsolete cameras. Latest pending
  frames replace only unsent video; partially sent TCP frames always finish.
- Disconnect and shutdown release guest slots. Scene rebuilds stop sessions and reset
  generations. Each guest must reconnect and submit a fresh camera pose.

The separation allows a future UDP inspector transport to share typed camera
requests and encoded frames. UDP still needs its own session identity, image
fragmentation/reassembly, loss handling, and reliable control strategy.

## Record a guest view

```sh
cd py_example
uv run python examples/inspector/receive.py --host 127.0.0.1 --port 12000 \
  --position -4 0 2 --quaternion 0 0 0 1 --duration 10 \
  --fps 30 --video ../artifacts/outputs/inspector.mp4
```

`InspectorClient` exposes only camera pose updates and status/RGB polling. The
script streams frames into an MP4 and saves the first frame as a PNG. Output FPS
is playback timing, not a request to change server capture rate. Start the simulator first with `./URSoccerLab.AppImage scene.json`.
See [Getting started](Getting_Started.md) for a complete configuration.

## Validation

```sh
py_example/.venv/bin/python Tools/runtime/test_inspector.py
py_example/.venv/bin/python -m unittest discover -s py_example/tests
```

The rendered test launches the source runtime with nDisplay for JPEG and raw
streaming. It saves distinct camera views, a changed pose view, receiver MP4/PNG,
logs, and rate measurements in `artifacts/tests/inspector/{jpeg,raw}/`. Native
automation under `URSoccerLab.Inspector` validates strict numeric types, normalized
quaternions, bounds, versioning, and rejection of robot/admin commands. A real
socket native test also checks pose mailboxes, disconnects, and adapter restart
without a renderer or physics loop. The rendered test includes robot reset with
guests present; a full scene rebuild is covered by the lifecycle/generation
implementation rather than a rendered rebuild test.

Implementation files: `Inspector/URSInspectorProtocol` contains pure application
protocol parsing; `URSInspectorNetwork` implements the socket-only adapter;
`URSInspectorCameraComponent` owns Unreal capture and encoder handoff;
`URSInspectorTransportComponent` coordinates bounded typed events on the game
thread. Scene rebuilds stop sessions and reset the camera generation, preventing
old encoder completions from attaching to new cameras.
