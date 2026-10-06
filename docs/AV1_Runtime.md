# AV1 streams on the existing TCP ports

Robot commands, state, RGB and depth keep the same per-robot TCP connection
(10000 + index). Guests share their inspector listener (12000 by default).
No RTSP server or new media port is required. This implementation remains TCP;
UDP packetization and recovery are future transport work.

## Scene configuration

Example scenes select AV1 explicitly. Files omitting robot `vision.rgb.compression`
retain the previous JPEG default. Guests default to AV1 at 30 Hz.

```json
{
  "vision": {
    "mode": "stereo_rgb",
    "rgb": {
      "rate_hz": 30,
      "compression": "av1",
      "bitrate_kbps": 2000,
      "keyframe_interval_s": 2,
      "vulkan_device": ""
    }
  },
  "guest_inspector": {
    "enabled": true,
    "port": 12000,
    "max_guests": 4,
    "width": 640,
    "height": 480,
    "rate_hz": 30,
    "fov_degrees": 90,
    "compression": "av1",
    "bitrate_kbps": 2000,
    "keyframe_interval_s": 2,
    "vulkan_device": ""
  }
}
```

These are startup settings. Resolution and capacity determine the generated
nDisplay atlas; restart the simulator to change them. Use `run_scene.py` or
`run_with_sim.py` to generate the matching layout. Guest dimensions must be
even, width 64–1920 and height 64–1080; capacity is 1–4; rate is 1–120 Hz;
FOV is 10–150 degrees. Bitrate is 64–100000 kbps and keyframe interval 0.1–60 s.
`camera_freq`, when positive, overrides the robot RGB rate as before.

`vulkan_device` selects an FFmpeg Vulkan device (GPU name substring or device
index); empty chooses FFmpeg's default device. Its driver must support Vulkan
Video AV1 encoding. AV1 setup failures are explicit errors in the runtime log;
there is no CPU or vendor-API fallback. JPEG/raw can be selected for diagnostics.

## Encoding and delivery

The simulator uses third-party FFmpeg `libavcodec` (`av1_vulkan`), `libavutil`
and `libswscale`. One persistent codec per robot encodes stereo as a single
1280x480 side-by-side frame; RGBD encodes only the left RGB eye. Every guest
has a separate persistent codec. Encoding runs on worker threads with bounded
in-flight jobs. RGB pixels are currently read back to CPU, converted to NV12,
and uploaded to the encoder; direct GPU texture sharing is not implemented.

A maximum GOP of `round(rate_hz * keyframe_interval_s)` encoded frames is used,
with no B-frames and ultra-low-latency tuning. If capture cannot sustain the
configured rate, the wall-clock interval between keyframes is longer.

New connections receive state immediately but skip video until a keyframe.
Codec initialization accompanies video packets. Replaced unsent AV1 packets,
sequence gaps, and encoder recreation return only the affected connection to
waiting for a keyframe. Partially written TCP messages always finish. No join,
pose update, or recovery request forces a keyframe. Network handoffs and pending
messages remain bounded; JPEG/raw retain their latest-image behavior.

Depth uses independent type-2 messages and its existing raw float32 metres or
raw/zlib uint16 millimetres encoding. It is not passed through lossy AV1.
Depth capture remains separate from nDisplay RGB; their timestamps are sampled
at consumption and do not establish exact RGB/depth exposure synchronization.

## Wire format and Python

Outer TCP framing and image message v2 stay unchanged. RGB codec ID **3** is
AV1. Image flags bit 0 indicates a keyframe. AV1 data starts with:

```
version:u8=1, layout:u8 (0 single / 1 side-by-side), epoch:LE64,
coded_width:LE16, coded_height:LE16, codec_config_length:LE32, second_eye_name_length:u8, second_eye_name:utf8,
codec_config:bytes, encoded_AV1_packet:bytes
```

The image width is the visible packed width. Vulkan may pad the coded surface to
the GPU alignment; the receiver validates the coded dimensions and crops to the
visible dimensions before splitting stereo. AV1 packet order and sequence IDs are
per encoder; a new epoch identifies codec recreation. State JSON remains
independent. The updated Python package uses **PyAV** (FFmpeg bindings) to keep
one decoder per stream, detect discontinuities, and split stereo into named
left/right camera dictionaries. `camera_to_rgb()` returns decoded RGB arrays,
so existing examples keep their camera API. Older clients do not decode codec 3.

## Build and checks

Install FFmpeg development libraries with an `av1_vulkan` encoder. The build
uses `/usr/include/{libavcodec,libavutil,libswscale}` and `/usr/lib`, or set
`URS_FFMPEG_ROOT` to a matching installation prefix. Headers are isolated into
`Intermediate/ThirdParty/FFmpeg` to preserve Unreal's compiler sysroot.
Run `uv sync --project py_example` for the pinned PyAV dependency.

```sh
py_example/.venv/bin/python Tools/runtime/test_av1.py
py_example/.venv/bin/python -m unittest discover -s py_example/tests
```

The rendered test exercises stereo, RGBD, moving guest cameras, non-default
guest settings, multiple robot readers and late joins. Native automation tests
configuration round-trip/rejection and per-connection AV1 dependency recovery.
No AppImage has been cooked or updated. Future packaging must bundle FFmpeg
runtime libraries and their dependencies, preserve their license notices, and
validate them against the target distribution's driver/runtime environment.
