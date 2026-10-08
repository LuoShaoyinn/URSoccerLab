"""Headless wire interoperability check; uses PyAV fixture encoders.
Run with the fixture environment, passing the client_probe path.
"""
import json
import socket
import struct
import subprocess
import sys
import threading
from fractions import Fraction

import av
import numpy as np


def test(codec, encoder_name):
    encoder = av.CodecContext.create(encoder_name, "w")
    encoder.width, encoder.height = 128, 64
    encoder.pix_fmt = "yuv420p"
    encoder.time_base = Fraction(1, 30)
    encoder.framerate = Fraction(30, 1)
    encoder.gop_size = 3
    encoder.max_b_frames = 0
    encoder.thread_count = 1
    encoder.options = ({"cpu-used": "8", "usage": "realtime", "lag-in-frames": "0"}
                       if codec == 3 else {"preset": "ultrafast", "tune": "zerolatency"})
    if codec == 5:
        encoder.options["x265-params"] = "pools=none:log-level=error"
    pixels = np.zeros((64, 128, 3), dtype=np.uint8)
    pixels[:, :64] = [230, 20, 10]
    pixels[:, 64:] = [10, 20, 230]
    frame = av.VideoFrame.from_ndarray(pixels, format="rgb24")
    frame.pts = 0
    packet, = encoder.encode(frame)
    assert packet.is_keyframe
    config = encoder.extradata or b""
    data = struct.pack("<BBQHHIB", 1, 0, 42, 128, 64, len(config), 0) + config + bytes(packet)
    body = struct.pack("<BBHId", 2, 1, 0, 7, 0.) + b"\x03cam"
    body += struct.pack("<BBBHHII", codec, 0, 1, 120, 60, 120*60*4, len(data)) + data
    errors = []
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        listener.settimeout(10)
        def server():
            try:
                conn, _ = listener.accept()
                with conn:
                    conn.settimeout(10)
                    def read(n):
                        b = b""
                        while len(b) < n:
                            part = conn.recv(n-len(b))
                            if not part:
                                raise EOFError()
                            b += part
                        return b
                    n, = struct.unpack(">I", read(4))
                    request = read(n)
                    assert request[0] == 0
                    command = json.loads(request[1:])
                    assert command["command"] == "set_camera"
                    assert command["args"]["translation_m"] == [-4, 0, 2]
                    assert command["args"]["rotation_quat_xyzw"] == [0, 0, 0, 1]
                    wire = struct.pack(">IB", len(body)+1, 1) + body
                    for offset in range(0, len(wire), 13):
                        conn.sendall(wire[offset:offset+13])
                    while conn.recv(1024):
                        pass
            except Exception as e:
                errors.append(e)
        thread = threading.Thread(target=server)
        thread.start()
        result = subprocess.run([sys.argv[1], str(listener.getsockname()[1])], timeout=12)
        thread.join(timeout=12)
        assert not thread.is_alive()
        assert result.returncode == 0, codec
        assert not errors, errors
    print(f"Codec {codec}: fragmented TCP, camera JSON, video crop passed")

for codec, name in [(3, "libaom-av1"), (4, "libx264"), (5, "libx265")]:
    test(codec, name)
