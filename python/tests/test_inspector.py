import json
import socket
import struct
import time
import unittest

import numpy as np

from ursoccerlab import InspectorClient
from ursoccerlab.media import camera_to_rgb
from test_tcp import image_message
from ursoccerlab.tcp import CODEC_RAW, PIXEL_BGRA8


class InspectorTest(unittest.TestCase):
    def test_fragmented_stream_and_camera_only_request(self):
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen()
            with InspectorClient(port=listener.getsockname()[1]) as client:
                peer, _ = listener.accept()
                with peer:
                    peer.settimeout(2)
                    client.set_camera([-4, 0, 2], [0, 0, 0, 2])
                    request = bytearray()
                    while len(request) < 4 or len(request) < 4 + struct.unpack('>I', request[:4])[0]:
                        request.extend(peer.recv(4096))
                    command = json.loads(request[5:])
                    self.assertEqual(command['command'], 'set_camera')
                    self.assertEqual(command['args']['rotation_quat_xyzw'], [0, 0, 0, 1])
                    self.assertFalse(hasattr(client, 'send_command'))
                    for position, rotation in [([1, 2], [0, 0, 0, 1]),
                                               ([0, 0, float('nan')], [0, 0, 0, 1]),
                                               ([0, 0, 0], [0, 0, 0, 0])]:
                        with self.assertRaises(ValueError):
                            client.set_camera(position, rotation)
                    status = json.dumps({'version': 1, 'ok': True}).encode()
                    raw = bytes([10, 20, 230, 255])
                    pixels = image_message(sequence=9, sim_time=2.5,
                        entries=[('inspector', CODEC_RAW, PIXEL_BGRA8, 1, 1, raw, 4)])
                    wire = struct.pack('>IB', len(status)+1, 0) + status
                    wire += struct.pack('>IB', len(pixels)+1, 1) + pixels
                    peer.sendall(wire[:3])
                    self.assertEqual(list(client.recv()), [])
                    peer.sendall(wire[3:])
                    received = []
                    deadline = time.monotonic()+2
                    while len(received) < 2 and time.monotonic() < deadline:
                        received.extend(client.recv())
                        time.sleep(.001)
                    self.assertEqual([kind for kind, _ in received], ['status', 'rgb'])
                    image = received[1][1][0]
                    self.assertEqual(image['sequence'], 9)
                    np.testing.assert_array_equal(camera_to_rgb(image), [[[230, 20, 10]]])
                deadline = time.monotonic()+2
                while client.alive and time.monotonic() < deadline:
                    list(client.recv())
                    time.sleep(.001)
                self.assertFalse(client.alive)
