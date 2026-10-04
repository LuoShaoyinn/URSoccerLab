#!/usr/bin/env python3
"""Source-only rendered inspector integration. Does not package an AppImage."""
import json
import math
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'py_example/src'))
from ursoccerlab import InspectorClient, RobotClient, AdminClient
from ursoccerlab.media import camera_to_rgb
from PIL import Image
import numpy as np
from test_network_refactor import stop

UE = Path('/home/luoshaoyinn/software/Unreal_Engine_5.7.4/Engine/Binaries/Linux/UnrealEditor')


def receive(client, count=1, timeout=12):
    frames, statuses = [], []
    deadline = time.monotonic()+timeout
    while time.monotonic() < deadline:
        for kind, data in client.recv():
            if kind == 'status':
                statuses.append(data)
            else:
                frames.extend(data)
        if len(frames) >= count:
            return frames, statuses
        if not client.alive:
            raise ConnectionError('inspector disconnected')
        time.sleep(.002)
    raise TimeoutError('no inspector frames')


def status(client, ok, timeout=5):
    deadline = time.monotonic()+timeout
    while time.monotonic() < deadline:
        for kind, data in client.recv():
            if kind == 'status':
                assert data['ok'] is ok, data
                return data
        time.sleep(.002)
    raise TimeoutError('no inspector status')


def run(codec):
    output = ROOT / 'Saved/Tests/inspector' / codec
    output.mkdir(parents=True, exist_ok=True)
    original = ROOT / 'py_example/examples/standing/scene.json'
    config = json.loads(original.read_text())
    config['field']['map_image'] = str((original.parent / config['field']['map_image']).resolve())
    config['camera_freq'] = 12
    scene = output / 'scene.json'
    scene.write_text(json.dumps(config))
    ue_log = output / 'ue.log'
    ue_log.unlink(missing_ok=True)
    env = dict(os.environ)
    env['LD_LIBRARY_PATH'] = str(ROOT / 'Plugins/UnrealRoboticsLab/Binaries/Linux')+':'+env.get('LD_LIBRARY_PATH', '')
    command = [sys.executable, str(ROOT/'Tools/runtime/run_scene.py'), '--ue', str(UE),
               '--scene-config', str(scene), '--sim-extra-arg=-FORCELOGFLUSH',
               f'--sim-extra-arg=-abslog={ue_log}', f'--sim-extra-arg=-URSInspectorCodec={codec}']
    clients = []
    with (output/'launcher.log').open('w') as log:
        process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            deadline = time.monotonic()+90
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError('runtime exited')
                if ue_log.exists() and ' listening on port 10001' in ue_log.read_text(errors='replace'):
                    break
                time.sleep(.1)
            else:
                raise TimeoutError('runtime startup')
            assert '[URS nDisplay] reserved 4 guest camera slots.' in ue_log.read_text(errors='replace')
            robot = RobotClient('127.0.0.1')
            clients.append(robot)
            baseline_states, baseline_images = [], 0
            deadline = time.monotonic()+5
            while time.monotonic()<deadline:
                for kind, data in robot.recv():
                    if kind == 'state': baseline_states.append(data['sim_time'])
                    elif kind == 'rgb': baseline_images += len(data)
                time.sleep(.002)
            assert len(baseline_states)>30 and baseline_images>10
            robot.close()
            a, b = InspectorClient(), InspectorClient()
            clients.extend([a, b])
            # No capture or stream before a valid pose.
            time.sleep(.1)
            assert list(a.recv()) == []
            angle = math.radians(15)/2
            a.set_camera([-4, 0, 2], [0, math.sin(angle), 0, math.cos(angle)])
            # Fragment a valid camera request across TCP reads.
            request = json.dumps({'version':1,'command':'set_camera','args':{
                'translation_m':[0,-4,2],
                'rotation_quat_xyzw':[0,0,math.sin(math.pi/4),math.cos(math.pi/4)]}}).encode()
            wire = struct.pack('>IB', len(request)+1, 0)+request
            b.conn.sock.sendall(wire[:2]); time.sleep(.02); b.conn.sock.sendall(wire[2:])
            af, ast = receive(a, 3)
            bf, bst = receive(b, 3)
            assert any(s['ok'] for s in ast), ast
            assert any(s['ok'] for s in bst), bst
            assert af[-1]['codec'] == codec
            assert af[-1]['camera_name'] == 'inspector'
            assert (af[-1]['width'], af[-1]['height']) == (640, 480)
            img_a, img_b = camera_to_rgb(af[-1]), camera_to_rgb(bf[-1])
            assert np.std(img_a) > 5, 'blank image'
            difference = float(np.abs(img_a.astype(float)-img_b).mean())
            assert difference > 5, ('independent views', difference)
            Image.fromarray(img_a).save(output/'camera_a.png')
            Image.fromarray(img_b).save(output/'camera_b.png')
            # Guests cannot route robot/admin commands through their endpoint.
            a.conn.send_json({'command':'reset', 'args':{'actor_id':'robot_rp0'}})
            status(a, False)
            a.conn.send_json({'joint':10})
            status(a, False)
            a.conn.send_json({'version':1,'command':'set_camera','args':{'translation_m':[101,0,2],'rotation_quat_xyzw':[0,0,0,1]}})
            status(a, False)
            time.sleep(.04)
            a.set_camera([-2, 2, 1], [0, 0, -math.sin(math.pi/4), math.cos(math.pi/4)])
            status(a, True)
            moved, _ = receive(a, 4)
            img_moved = camera_to_rgb(moved[-1])
            assert np.abs(img_a.astype(float)-img_moved).mean() > 5
            Image.fromarray(img_moved).save(output/'camera_moved.png')
            c, slow = InspectorClient(), InspectorClient()
            clients.extend([c, slow])
            c.set_camera([-4,0,2], [0,0,0,1]); receive(c)
            slow.set_camera([-4,0,2], [0,0,0,1])
            extra = InspectorClient()
            deadline = time.monotonic()+3
            while extra.alive and time.monotonic()<deadline:
                list(extra.recv()); time.sleep(.01)
            assert not extra.alive, 'fifth guest was admitted'
            extra.close()
            # With all four slots occupied (including an unread stream), physics,
            # robot vision, admin, and a fast guest remain live.
            robot = RobotClient('127.0.0.1'); admin = AdminClient('127.0.0.1')
            clients.extend([robot, admin])
            states, robot_images, frames = [], 0, 0
            start = time.monotonic(); deadline = start+5
            while time.monotonic()<deadline:
                for kind, data in robot.recv():
                    if kind == 'state': states.append(data['sim_time'])
                    elif kind == 'rgb': robot_images += len(data)
                for guest in (a, b, c):
                    for kind, data in guest.recv():
                        if kind == 'rgb': frames += len(data)
                time.sleep(.002)
            assert len(states)>30 and states[-1]-states[0]>2, states[-3:]
            assert robot_images>10 and frames>15
            assert admin.get_pose('robot_rp0')['ok']
            assert admin.reset('robot_rp0')['ok']
            receive(a)
            receive(b)
            # Closing a streaming and a slow guest frees capacity; reconnect works.
            c.close(); slow.close(); time.sleep(.2)
            new = InspectorClient(); clients.append(new)
            new.set_camera([-4,0,2], [0,0,0,1]); receive(new)
            new.close(); time.sleep(.1)
            malformed = socket.create_connection(('127.0.0.1',12000), timeout=3)
            malformed.sendall(b'\xff\xff\xff\xff\x00')
            try: assert malformed.recv(1) == b''
            except ConnectionResetError: pass
            malformed.close()
            receive(a)
            # Exercise the shipped Python CLI including incremental MP4 output.
            b.close(); time.sleep(.1)
            capture = subprocess.run([sys.executable, str(ROOT/'py_example/examples/inspector/receive.py'),
                                      '--duration', '2', '--video', str(output/'receiver.mp4')],
                                     cwd=ROOT, capture_output=True, text=True, timeout=20)
            assert capture.returncode == 0, capture.stderr
            assert (output/'receiver.mp4').stat().st_size>1000
            assert (output/'receiver.png').exists()
            result = dict(codec=codec, independent_view_difference=difference,
                          robot_states=len(states), robot_images=robot_images, guest_frames=frames,
                          sim_advance=states[-1]-states[0], state_hz=len(states)/5, baseline_state_hz=len(baseline_states)/5,
                          baseline_robot_images=baseline_images,
                          independent_views=True, movement=True, privileged_commands_rejected=True,
                          four_guest_limit=True, slow_guest_isolation=True, reconnect=True,
                          malformed_isolation=True, robot_reset_with_guests=True, receiver_cli=True)
            (output/'results.json').write_text(json.dumps(result, indent=2)+'\n')
            print(json.dumps(result), flush=True)
            return result
        finally:
            for client in clients: client.close()
            stop(process)


if __name__ == '__main__':
    for codec in ('jpeg', 'raw'): run(codec)
