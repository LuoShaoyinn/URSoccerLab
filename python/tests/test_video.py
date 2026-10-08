import struct
import unittest

import av
import numpy as np

from ursoccerlab.video import VideoDecoder
from ursoccerlab.media import camera_to_rgb


class VideoTest(unittest.TestCase):
    def test_stereo_keyframe_join_gap_and_epoch(self):
        # Real third-party AV1 encoder/decoder, with different colours per eye.
        encoder = av.CodecContext.create('libaom-av1', 'w')
        encoder.width, encoder.height = 128, 64
        encoder.pix_fmt = 'yuv420p'
        from fractions import Fraction
        encoder.time_base = Fraction(1, 30)
        encoder.framerate = Fraction(30, 1)
        encoder.gop_size = 3
        encoder.thread_count = 1
        encoder.options = {'cpu-used': '8', 'usage': 'realtime', 'lag-in-frames': '0'}
        pixels = np.zeros((64, 128, 3),dtype=np.uint8)
        pixels[:, :64] = [230, 30, 10]
        pixels[:, 64:] = [10, 30, 230]
        messages = []
        for sequence in range(7):
            frame = av.VideoFrame.from_ndarray(pixels,format='rgb24')
            frame.pts = sequence
            packets = encoder.encode(frame)
            self.assertEqual(len(packets),1)
            packet = packets[0]
            right = b'right_eye'
            data = struct.pack('<BBQHHIB',1,1,1,128,64,0,len(right)) + right + bytes(packet)
            messages.append(dict(codec='av1', camera_name='left_eye', width=128,
                                 height=64, sequence=sequence, keyframe=packet.is_keyframe,
                                 sim_time=sequence/30, data=data))
        # GPU encoders may pad the coded surface. Preserve requested visible dimensions.
        cropped = dict(messages[0], width=120, height=60)
        eyes = VideoDecoder().decode([cropped])
        self.assertEqual(camera_to_rgb(eyes[0]).shape, (60,60,3))
        self.assertEqual(camera_to_rgb(eyes[1]).shape, (60,60,3))
        decoder = VideoDecoder()
        self.assertEqual(decoder.decode([messages[1]]),[])
        self.assertEqual(decoder.decode([messages[2]]),[])
        decoded = decoder.decode([messages[3]])
        self.assertEqual([image['camera_name'] for image in decoded], ['left_eye','right_eye'])
        left, right = map(camera_to_rgb,decoded)
        self.assertEqual(left.shape,(64,64,3))
        self.assertGreater(left[:,:,0].mean(),200)
        self.assertGreater(right[:,:,2].mean(),200)
        self.assertEqual(len(decoder.decode([messages[4]])),2)
        # Drop frame 5. A later dependent packet must wait for a new keyframe.
        gap = dict(messages[5], sequence=7)
        self.assertEqual(decoder.decode([gap]),[])
        self.assertEqual(len(decoder.decode([messages[6]])),2)
        recreated = dict(messages[3])
        recreated['data'] = struct.pack('<BBQHHIB',1,1,2,128,64,0,len(b'right_eye')) + b'right_eye' + bytes(messages[3]['data'][28:])
        self.assertEqual(len(decoder.decode([recreated])),2)

    def test_h264_h265_join_gap_and_codec_change(self):
        from fractions import Fraction
        decoder = VideoDecoder()
        for codec, encoder_name in [('h264', 'libx264'), ('h265', 'libx265')]:
            with self.subTest(codec=codec):
                encoder = av.CodecContext.create(encoder_name, 'w')
                encoder.width, encoder.height = 128, 64
                encoder.pix_fmt = 'yuv420p'
                encoder.time_base = Fraction(1, 30)
                encoder.framerate = Fraction(30, 1)
                encoder.gop_size = 3
                encoder.max_b_frames = 0
                encoder.thread_count = 1
                encoder.options = {'preset': 'ultrafast', 'tune': 'zerolatency'}
                if codec == 'h264':
                    encoder.options['x264-params'] = 'scenecut=0'
                else:
                    encoder.options['x265-params'] = 'scenecut=0:keyint=3:min-keyint=3:pools=none:log-level=error'
                pixels = np.zeros((64, 128, 3), dtype=np.uint8)
                pixels[:, :64] = [220, 20, 10]
                pixels[:, 64:] = [10, 20, 220]
                messages = []
                for sequence in range(7):
                    frame = av.VideoFrame.from_ndarray(pixels, format='rgb24')
                    frame.pts = sequence
                    packets = encoder.encode(frame)
                    self.assertEqual(len(packets), 1)
                    packet = packets[0]
                    right = b'right_eye'
                    config = encoder.extradata or b''
                    data = struct.pack('<BBQHHIB', 1, 1, 1, 128, 64, len(config), len(right)) + right + config + bytes(packet)
                    messages.append(dict(codec=codec, camera_name='left_eye', width=128,
                                         height=64, sequence=sequence, keyframe=packet.is_keyframe,
                                         sim_time=sequence/30, data=data))
                # H.264/H.265 may crop the coded surface before returning a frame.
                padded = dict(messages[0])
                padded_data = bytearray(padded['data'])
                struct.pack_into('<H', padded_data, 12, 72)
                padded['data'] = bytes(padded_data)
                self.assertEqual(len(VideoDecoder().decode([padded])), 2)
                # Reuse the same decoder and epoch across codecs: a new codec resets it.
                self.assertEqual(decoder.decode([messages[1]]), [])
                eyes = decoder.decode([messages[3]])
                self.assertEqual(len(eyes), 2)
                left, right = map(camera_to_rgb, eyes)
                self.assertGreater(left[:, :, 0].mean(), 200)
                self.assertGreater(right[:, :, 2].mean(), 200)
                self.assertEqual(len(decoder.decode([messages[4]])), 2)
                self.assertEqual(decoder.decode([dict(messages[5], sequence=7)]), [])
                self.assertEqual(len(decoder.decode([messages[6]])), 2)

    def test_malformed_packet_header(self):
        with self.assertRaises(ValueError):
            VideoDecoder().decode([dict(codec='av1', data=b'bad')])
