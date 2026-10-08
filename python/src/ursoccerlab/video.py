"""Stateful third-party video decoding for each TCP connection."""
from __future__ import annotations

import struct


class VideoDecoder:
    def __init__(self):
        self.streams = {}

    def decode(self, images):
        output = []
        for image in images:
            if image['codec'] not in ('av1', 'h264', 'h265'):
                output.append(image)
                continue
            import av
            data = image['data']
            if len(data) < 19:
                raise ValueError('truncated video packet header')
            version, layout, epoch, coded_width, coded_height, config_length, name_length = struct.unpack_from('<BBQHHIB', data)
            offset = 19 + name_length
            if version != 1 or layout not in (0, 1) or offset + config_length >= len(data):
                raise ValueError('invalid video packet header')
            if coded_width < image['width'] or coded_height < image['height']:
                raise ValueError('video coded dimensions are smaller than the visible image')
            right_name = data[19:offset].decode('utf-8')
            if layout == 1 and (not right_name or image['width'] % 2):
                raise ValueError('invalid side-by-side stereo layout')
            config = data[offset:offset + config_length]
            packet_bytes = data[offset + config_length:]
            name = image['camera_name']
            sequence = image['sequence']
            stream = self.streams.get(name)
            if stream is None or stream['epoch'] != epoch or stream['codec'] != image['codec']:
                stream = {'epoch': epoch, 'codec': image['codec'], 'last': None, 'decoder': None}
                self.streams[name] = stream
            if stream['last'] is not None and sequence != ((stream['last'] + 1) & 0xffffffff):
                stream['decoder'] = None
            stream['last'] = sequence
            if stream['decoder'] is None:
                if not image['keyframe']:
                    continue
                decoder = av.CodecContext.create({'av1': 'libdav1d', 'h264': 'h264', 'h265': 'hevc'}[image['codec']], 'r')
                decoder.thread_count = 1
                decoder.thread_type = 'SLICE'
                if config:
                    decoder.extradata = config
                stream['decoder'] = decoder
            packet = av.Packet(packet_bytes)
            packet.pts = sequence
            try:
                frames = stream['decoder'].decode(packet)
            except av.FFmpegError:
                stream['decoder'] = None
                continue
            for frame in frames:
                # H.264/H.265 decoders can apply SPS cropping to a padded surface.
                # AV1 may retain padding; either must contain the whole visible image.
                if not (image['width'] <= frame.width <= coded_width and
                        image['height'] <= frame.height <= coded_height):
                    raise ValueError(f"video decoded size {(frame.width, frame.height)} is outside visible/coded bounds")
                rgb = frame.to_ndarray(format='rgb24')[:image['height'], :image['width']]
                eyes = [(name, rgb)] if layout == 0 else [
                    (name, rgb[:, :image['width'] // 2]),
                    (right_name, rgb[:, image['width'] // 2:]),
                ]
                for index, (eye_name, pixels) in enumerate(eyes):
                    result = dict(image, camera_name=eye_name, cam_index=index,
                                  width=pixels.shape[1], height=pixels.shape[0],
                                  uncompressed_len=pixels.shape[1] * pixels.shape[0] * 4,
                                  video_epoch=epoch, _rgb=pixels.copy())
                    output.append(result)
        return output
