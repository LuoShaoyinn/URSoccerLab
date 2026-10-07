# Video encoding on the existing TCP ports

Robot commands, state, RGB and depth keep the same per-robot TCP connection
(10000 + index). Guests share their inspector listener (12000 by default).
No RTSP server or new media port is required. This implementation remains TCP;
UDP packetization and recovery are future transport work.

Automatic backend selection and codec IDs 4/5 require an application and Python
wheel built from this source version. The published v0.1.0 release remains unchanged.

## Scene configuration

All robot and guest videos share one codec/backend policy. The defaults are AV1,
automatic hardware selection, and H.264 fallback. Frame rates, bitrates and
keyframe intervals remain per-stream settings.

```json
{
  "encoder": {
    "codec": "av1",
    "backend": "auto",
    "fallback_codec": "h264"
  },
  "vision": {
    "mode": "stereo_rgb",
    "rgb": {"rate_hz": 30, "bitrate_kbps": 2000, "keyframe_interval_s": 2}
  },
  "guest_inspector": {
    "enabled": true, "port": 12000, "max_guests": 4,
    "width": 640, "height": 480, "rate_hz": 30, "fov_degrees": 90,
    "bitrate_kbps": 2000, "keyframe_interval_s": 2
  }
}
```

| Shared setting | Values |
| --- | --- |
| `encoder.codec` | `av1` (default), `h264`, `h265`; `raw` and `jpeg` for diagnostics |
| `encoder.backend` | `auto` (default), `nvenc`, `qsv`, `vaapi`, `vulkan` |
| `encoder.fallback_codec` | `h264` (default), `h265`, or `null` to require the requested codec |
| `encoder.device` | Optional device for an explicit backend; omit with `auto` |

Automatic selection tries NVENC, QSV, VAAPI, then Vulkan for the requested codec.
It encodes and decodes moving calibration frames at the registered camera sizes
before accepting a backend. If every backend fails, it tries the fallback codec
in the same order. All videos then use that selected codec/backend. Selection is
performed once per scene, on an encoder worker; it does not switch during streaming.
An explicit backend confines both codec attempts to that backend. Errors and the
selected codec/backend are printed to stderr, including Shipping builds.

For NVENC, `device` is the GPU index as a string, such as `"0"`. For VAAPI/QSV it
is a DRM render-node path, such as `"/dev/dri/renderD128"`. Vulkan accepts its FFmpeg
device selector, such as `"0"`. Render-node numbering can change between machines.
There is no software fallback. GPU drivers and native encoder runtimes remain
host-provided. The NVENC build uses SDK 13 headers and requires NVIDIA Linux
driver 570 or newer ([SDK header requirements](https://github.com/FFmpeg/nv-codec-headers/blob/n13.0.19.0/README)).
VAAPI uses the host Mesa/iHD driver; QSV also requires its Intel GPU runtime.
The renderer still requires a compatible Vulkan driver.

Legacy per-stream `compression` is imported when the shared section is absent
and both streams agree; replace it with `encoder.codec`. A shared section takes
precedence over legacy compression. Move `vulkan_device` to `encoder.device`
and select `backend: "vulkan"`. Restart after changing settings.

These are startup settings. Resolution and capacity determine the generated
nDisplay atlas; restart the simulator to change them. The AppImage launcher generates the matching layout from the JSON. Source
launchers `run_scene.py` and `run_with_sim.py` also generate it. Guest dimensions must be
even, width 64–1920 and height 64–1080; capacity is 1–4; rate is 1–120 Hz;
FOV is 10–150 degrees. Bitrate is 64–100000 kbps and keyframe interval 0.1–60 s.
`camera_freq`, when positive, overrides the robot RGB rate as before.

## Encoding and delivery

The simulator uses third-party FFmpeg `libavcodec` (NVENC, QSV, VAAPI or Vulkan hardware encoders), `libavutil`
and `libswscale`. One persistent codec per robot encodes stereo as a single
1280x480 side-by-side frame; RGBD encodes only the left RGB eye. Every guest
has a separate persistent codec. Encoding runs on worker threads with bounded
in-flight jobs. RGB pixels are currently read back to CPU, converted to NV12,
and uploaded to the encoder; direct GPU texture sharing is not implemented.

A maximum GOP of `round(rate_hz * keyframe_interval_s)` encoded frames is used,
with no B-frames and low-latency tuning. If capture cannot sustain the
configured rate, the wall-clock interval between keyframes is longer.

New connections receive state immediately but skip video until a keyframe.
Codec initialization accompanies video packets. Replaced unsent video packets,
sequence gaps, and encoder recreation return only the affected connection to
waiting for a keyframe. Partially written TCP messages always finish. No join,
pose update, or recovery request forces a keyframe. Network handoffs and pending
messages remain bounded; JPEG/raw retain their latest-image behavior.

Depth uses independent type-2 messages and its existing raw float32 metres or
raw/zlib uint16 millimetres encoding. It is not passed through lossy video.
Depth capture remains separate from nDisplay RGB; their timestamps are sampled
at consumption and do not establish exact RGB/depth exposure synchronization.

## Wire format and Python

Outer TCP framing and image message v2 stay unchanged. RGB codec ID **3** is
AV1; IDs **4** and **5** are H.264 and H.265. Image flags bit 0 indicates a keyframe. All three codecs use the same inner envelope:

```
version:u8=1, layout:u8 (0 single / 1 side-by-side), epoch:LE64,
coded_width:LE16, coded_height:LE16, codec_config_length:LE32, second_eye_name_length:u8, second_eye_name:utf8,
codec_config:bytes, encoded_packet:bytes
```

The image width is the visible packed width. Hardware encoders may pad the coded surface to
the GPU alignment. Calibration records decoded dimensions when encoder metadata omits padding; the receiver validates the coded dimensions and crops to the
visible dimensions before splitting stereo. H.264/H.265 decoders may already crop padding using the stream headers; the receiver accepts sizes between the visible and coded dimensions. Video packet order and sequence IDs are
per encoder; a new epoch identifies codec recreation. State JSON remains
independent. The updated Python package uses **PyAV** (FFmpeg bindings) to keep
one decoder per stream, detect discontinuities, and split stereo into named
left/right camera dictionaries. `camera_to_rgb()` returns decoded RGB arrays,
so existing examples keep their camera API. Clients shipped with v0.1.0 do not decode codec IDs 4/5; install a wheel built from this version when using fallback.

## Build and checks

Install FFmpeg development libraries with the configured hardware encoders and AV1/H.264/H.265 validation decoders. The build
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
The Docker-built AppImage bundles FFmpeg 8.1.3 native and Vulkan video libraries and their
dependencies and license notices. GPU drivers remain host-provided. See
[Docker packaging](../Tools/packaging/README.md) for the build and distribution
baseline. Backend availability depends on the host GPU and driver; encoder registration alone does not prove that encoding works.

The standalone diagnostic uses the production FFmpeg core without Unreal or TCP:

```sh
export URS_FFMPEG_ROOT=/opt/urs-ffmpeg
export LD_LIBRARY_PATH="$URS_FFMPEG_ROOT/lib:${LD_LIBRARY_PATH:-}"
g++ -std=c++17 -O2 -pthread Source/URSoccerLab/Private/Vision/URSVideoCodec.cpp \
  Tools/runtime/test_encoder_backend.cpp -ISource/URSoccerLab/Private/Vision \
  -I"$URS_FFMPEG_ROOT/include" -L"$URS_FFMPEG_ROOT/lib" \
  -Wl,-rpath-link,"$URS_FFMPEG_ROOT/lib" -lavcodec -lavutil -lswscale \
  -o /tmp/urs-test-encoder
mkdir -p artifacts/tests/encoder-backend
/tmp/urs-test-encoder av1 auto h264 artifacts/tests/encoder-backend/probe
py_example/.venv/bin/python Tools/runtime/test_encoder_packets.py av1 artifacts/tests/encoder-backend/probe
```

Use the selected codec printed by the probe as the Python argument. The diagnostic
checks 640×480, 800×600 and 1280×480, periodic keyframes, late joins and sequence gaps.
For full robot/guest validation against an extracted candidate runtime, use
`Tools/packaging/smoke_appimage.py --executable /path/to/AppRun`; its backend,
expected codec, scene and output options leave the published AppImage untouched.
