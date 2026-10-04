#!/usr/bin/env python3
"""Rendered Vulkan AV1 integration; never cooks or packages an AppImage."""
import json
import math
import os
import struct
import subprocess
import sys
import time
from pathlib import Path

import imageio.v2 as imageio
import numpy as np
from PIL import Image
from ursoccerlab import InspectorClient, RobotClient
from ursoccerlab.media import camera_to_rgb, depth_to_meters
from ursoccerlab.tcp import FrameConn, TYPE_JSON, TYPE_RGB, parse_image_message
from ursoccerlab.video import VideoDecoder
from test_network_refactor import stop

ROOT = Path(__file__).resolve().parents[2]


def run(mode):
    output = ROOT / 'Saved/Tests/av1' / mode
    output.mkdir(parents=True, exist_ok=True)
    original = ROOT / 'py_example/examples/standing/scene.json'
    config = json.loads(original.read_text())
    config['field']['map_image'] = str((original.parent / config['field']['map_image']).resolve())
    config['vision']['mode'] = mode
    config['vision']['rgb'].update(compression='av1', rate_hz=30, keyframe_interval_s=2)
    config['camera_freq'] = 30
    config['guest_inspector'].update(compression='av1', rate_hz=30)
    # Exercise non-default port, dimensions and capacity without changing robots.
    if mode == 'rgbd':
        config['guest_inspector'].update(port=12010, width=800, height=600, max_guests=2)
    scene = output / 'scene.json'
    scene.write_text(json.dumps(config, indent=2))
    logpath = output / 'ue.log'
    logpath.unlink(missing_ok=True)
    env = dict(os.environ)
    env['LD_LIBRARY_PATH'] = str(ROOT / 'Plugins/UnrealRoboticsLab/Binaries/Linux') + ':' + env.get('LD_LIBRARY_PATH', '')
    command = [sys.executable, str(ROOT / 'Tools/runtime/run_scene.py'), '--scene-config', str(scene),
               f'--sim-extra-arg=-abslog={logpath}']
    clients, writers = [], []
    with (output / 'launcher.log').open('w') as log:
        process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            deadline = time.monotonic() + 90
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError('simulator exited')
                if logpath.exists() and ' listening on port 10001' in logpath.read_text(errors='replace'):
                    break
                time.sleep(.1)
            else:
                raise TimeoutError('startup')
            robot = RobotClient('127.0.0.1')
            guest = InspectorClient(port=config['guest_inspector']['port'])
            clients.extend([robot, guest])
            guest.set_camera([-4, 0, 2], [0, math.sin(.13), 0, math.cos(.13)])
            names = ['robot_left', 'guest']
            writers = [imageio.get_writer(str(output / (name + '.mp4')), fps=30,
                                          codec='libx264', macro_block_size=1) for name in names]
            counts = [0, 0]
            arrivals = [[], []]
            states, depths = 0, 0
            first_rgb = True
            late = None
            late_decoder = VideoDecoder()
            late_first_video = None
            late_states = 0
            late_decoded = 0
            late_join_sequence = None
            key_sequences = []
            slow = None
            start = time.monotonic()
            pending = True
            last_pose = start
            deadline = start + 10
            while time.monotonic() < deadline:
                now = time.monotonic()
                if now - last_pose > .15 and not pending:
                    yaw = math.sin((now-start) * .7) * .25
                    guest.set_camera([-4, 0, 2], [-math.sin(yaw)*math.sin(.13), math.cos(yaw)*math.sin(.13),
                                                  math.sin(yaw)*math.cos(.13), math.cos(yaw)*math.cos(.13)])
                    pending = True
                    last_pose = now
                for index, client in enumerate((robot, guest)):
                    for kind, data in client.recv():
                        if kind == 'status':
                            assert data['ok'], data
                            pending = False
                        elif kind == 'state':
                            states += 1
                        elif kind == 'depth':
                            for entry in data:
                                depth = depth_to_meters(entry)
                                assert depth.shape == (480, 640)
                                assert np.isfinite(depth).all() and (depth > 0).any()
                                assert entry['codec'] == 'zlib'
                                depths += 1
                        elif kind == 'rgb':
                            assert all(entry['codec'] == 'av1' for entry in data)
                            expected = 2 if index == 0 and mode == 'stereo_rgb' else 1
                            assert len(data) == expected
                            if expected == 2:
                                assert [entry['camera_name'] for entry in data] == ['left_eye','right_eye']
                                assert data[0]['sequence'] == data[1]['sequence']
                                assert data[0]['sim_time'] == data[1]['sim_time']
                            entry = data[0]
                            pixels = camera_to_rgb(entry)
                            assert float(np.std(pixels)) > 5
                            writers[index].append_data(pixels)
                            if counts[index] == 0:
                                Image.fromarray(pixels).save(output / (names[index]+'.png'))
                            counts[index] += 1
                            arrivals[index].append(now)
                            if index == 0:
                                if entry['keyframe']:
                                    key_sequences.append(entry['sequence'])
                                if first_rgb:
                                    assert entry['keyframe']
                                    first_rgb = False
                                # Join just after a keyframe. State is immediate,
                                # video waits for the next scheduled keyframe.
                                if late is None and entry['keyframe'] and entry['sequence'] >= 60:
                                    late = FrameConn('127.0.0.1',10000)
                                    clients.append(late)
                                    late_join_sequence = entry['sequence']
                if late is not None:
                    for kind, payload in late.recv_frames():
                        if kind == TYPE_JSON:
                            late_states += 1
                        elif kind == TYPE_RGB:
                            entries = parse_image_message(payload)
                            if late_first_video is None:
                                late_first_video = entries[0]['sequence']
                                assert entries[0]['keyframe']
                                assert late_first_video > late_join_sequence
                                assert late_states > 0
                            late_decoded += len(late_decoder.decode(entries))
                # Keep a connection unread long enough to exercise its independent
                # write budget while the regular client continues decoding.
                if slow is None and now-start > 3:
                    slow = FrameConn('127.0.0.1',10000)
                    slow.sock.setsockopt(__import__('socket').SOL_SOCKET, __import__('socket').SO_RCVBUF, 4096)
                    clients.append(slow)
                time.sleep(.001)
            assert states > 100 and counts[0] > 100 and counts[1] > 100, (states, counts)
            assert late_first_video is not None and late_decoded > 10
            assert len(key_sequences) >= 3
            assert all(b-a == 60 for a,b in zip(key_sequences,key_sequences[1:])), key_sequences
            if mode == 'rgbd':
                assert depths > 30, depths
            text = logpath.read_text(errors='replace')
            assert '[URS AV1] Vulkan' in text
            assert 'Error: [URS AV1]' not in text
            result = dict(mode=mode, frames=counts, measured_fps=[(len(ts)-1)/(ts[-1]-ts[0]) for ts in arrivals],
                          states=states, depth_frames=depths, key_sequences=key_sequences,
                          late_join_sequence=late_join_sequence, late_first_video=late_first_video,
                          late_decoded=late_decoded, guest_config=config['guest_inspector'])
            (output/'results.json').write_text(json.dumps(result,indent=2)+'\n')
            print(json.dumps(result),flush=True)
        finally:
            for writer in writers:
                writer.close()
            for client in clients:
                client.close()
            stop(process)


if __name__ == '__main__':
    for mode in ('stereo_rgb','rgbd'):
        run(mode)
