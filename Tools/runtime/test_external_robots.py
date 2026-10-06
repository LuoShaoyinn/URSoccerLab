#!/usr/bin/env python3
"""Exercise external pi_plus/mos9 packages, moving cameras and shared AV1 guests."""
import json
import argparse
import math
import os
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path

import imageio.v2 as imageio
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'py_example/src'))
from ursoccerlab import InspectorClient, RobotClient
from ursoccerlab.gains import detect_gains
from ursoccerlab.media import camera_to_rgb
from ursoccerlab.tcp import AdminClient
from test_network_refactor import stop


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--startup-timeout', type=float, default=90, help='Allow longer startup when compiling diagnostic shaders')
    parser.add_argument('--fixed-base', action='store_true', help='Use fixed-root MJCF copies and actuator-driven heads instead of admin pose locks')
    parser.add_argument('--direct-capture', action='store_true', help='Diagnose URLab SceneCapture without nDisplay or guests')
    parser.add_argument('--no-guest', action='store_true')
    parser.add_argument('--second-robot', choices=('pi_plus', 'mos9'), default='mos9')
    parser.add_argument('--compression', choices=('av1', 'raw'), default='av1')
    parser.add_argument('--sim-extra-arg', action='append', default=[])
    parser.add_argument('--no-robot-visuals', action='store_true')
    parser.add_argument('--no-lumen', action='store_true')
    parser.add_argument('--lamp-intensity-lumens', type=float)
    parser.add_argument('--light-source-radius-cm', type=float)
    parser.add_argument('--light-specular-scale', type=float)
    parser.add_argument('--film-grain-intensity', type=float)
    parser.add_argument('--motion-blur', choices=('on', 'off'))
    parser.add_argument('--output', type=Path, default=ROOT / 'artifacts/tests/external-robots')
    args = parser.parse_args()
    if args.direct_capture: args.no_guest = args.fixed_base = True
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    for previous in output.glob("*_?s.png"): previous.unlink()
    original = ROOT / 'py_example/examples/standing/scene.json'
    config = json.loads(original.read_text())
    config['robot_types'] = {name: str(ROOT / 'external/robots' / name / 'robot.json') for name in ('pi_plus', 'mos9')}
    if args.no_robot_visuals or args.fixed_base:
        for name, manifest_path in config['robot_types'].items():
            manifest = json.loads(Path(manifest_path).read_text())
            source_model = Path(manifest_path).parent/manifest['model']
            model = ET.parse(source_model)
            compiler = model.getroot().find('compiler')
            if compiler is not None: compiler.set('meshdir', str((source_model.parent/compiler.get('meshdir', '')).resolve()))
            if args.fixed_base:
                base = next(node for node in model.iter('body') if node.get('name') == manifest['bindings']['base_body'])
                for joint in list(base):
                    if joint.tag == 'freejoint' or (joint.tag == 'joint' and joint.get('type') == 'free'): base.remove(joint)
            glbs = {node.attrib['name'] for node in model.iter('mesh') if node.get('file', '').endswith('.glb')}
            for parent in model.iter():
                for node in list(parent):
                    if args.no_robot_visuals and node.tag == 'geom' and node.get('mesh') in glbs: parent.remove(node)
                    elif args.no_robot_visuals and node.tag == 'mesh' and node.get('name') in glbs: parent.remove(node)
            model_path = output/(name+'.xml')
            model.write(model_path)
            manifest['model'] = str(model_path)
            path = output/(name+'.json')
            path.write_text(json.dumps(manifest))
            config['robot_types'][name] = str(path)
    config['field']['visual']['base_color_map'] = str((original.parent / config['field']['visual']['base_color_map']).resolve())
    config['robots'][1]['type'] = args.second_robot
    config['robots'][1]['translation_m'][2] = .53 if args.second_robot == 'mos9' else .3762
    config['guest_inspector'].update(max_guests=1, bitrate_kbps=8000)
    config['guest_inspector']['compression'] = args.compression
    if args.no_guest: config['guest_inspector']['enabled'] = False
    config['vision']['rgb']['compression'] = args.compression
    if args.no_lumen: config.setdefault('render', {})['lumen'] = False
    if args.lamp_intensity_lumens is not None:
        config['lighting'] = {'lamp_intensity_lumens': args.lamp_intensity_lumens}
    if args.light_source_radius_cm is not None:
        config.setdefault('lighting', {})['source_radius_cm'] = args.light_source_radius_cm
    if args.light_specular_scale is not None:
        config.setdefault('lighting', {})['specular_scale'] = args.light_specular_scale
    if args.film_grain_intensity is not None:
        config.setdefault('render', {})['film_grain'] = {'intensity': args.film_grain_intensity}
    if args.motion_blur is not None:
        config.setdefault('render', {})['motion_blur'] = args.motion_blur == 'on'
    scene = output / 'scene.json'
    scene.write_text(json.dumps(config, indent=2)+'\n')
    ue_log = output / 'ue.log'
    ue_log.unlink(missing_ok=True)
    env = dict(os.environ)
    env['LD_LIBRARY_PATH'] = str(ROOT / 'Plugins/UnrealRoboticsLab/Binaries/Linux')+':'+env.get('LD_LIBRARY_PATH', '')
    command = [sys.executable, str(ROOT / 'Tools/runtime/run_scene.py'), '--scene-config', str(scene), f'--sim-extra-arg=-abslog={ue_log}']
    if args.direct_capture:
        from run_scene import DEFAULT_UE
        command = [str(DEFAULT_UE), str(ROOT/'URSoccerLab.uproject'), '/Game/Levels/URS_SoccerField',
                   '-game', '-NoSound', '-RenderOffscreen', '-ForceRes', '-ResX=1280', '-ResY=720',
                   f'-URSSceneConfig={scene}', f'-abslog={ue_log}', *args.sim_extra_arg]
    else:
        command.extend('--sim-extra-arg='+value for value in args.sim_extra_arg)
    clients, writers = [], []
    with (output / 'launcher.log').open('w') as log:
        process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            deadline = time.monotonic()+args.startup_timeout
            listeners_ready_since = None
            previous_listener_count = 0
            while time.monotonic()<deadline:
                if process.poll() is not None: raise RuntimeError('simulator exited; see '+str(ue_log))
                # Startup may rebuild listeners on frame one. Wait for all
                # endpoints and let initialization settle, without assuming
                # the core always initializes twice.
                text = ue_log.read_text(errors='replace') if ue_log.exists() else ''
                count = text.count(' listening on port 10001')
                ready = count > 0 and (args.no_guest or '[URS Inspector] listening on port 12000' in text)
                if not ready or count != previous_listener_count:
                    listeners_ready_since = time.monotonic() if ready else None
                previous_listener_count = count
                if listeners_ready_since is not None and time.monotonic()-listeners_ready_since >= 2: break
                time.sleep(.1)
            else: raise TimeoutError('external robot startup')
            robots = [RobotClient('127.0.0.1', 10000+i) for i in range(2)]
            guest = None if args.no_guest else InspectorClient()
            streams = [*robots]+([guest] if guest else [])
            clients = list(streams)
            states = [None, None]
            deadline = time.monotonic()+5
            while not all(states) and time.monotonic()<deadline:
                for i, robot in enumerate(robots):
                    for kind, data in robot.recv():
                        if kind == 'state': states[i] = data
                time.sleep(.002)
            assert all(states), 'missing states'
            for robot, state in zip(robots, states):
                names = list(state['actuators'])
                gains = detect_gains(names)
                assert gains, names
                robot.set_controller_params(**gains, actuator_mode='position')
                robot.send_command({name: 0 for name in names})
            admin = AdminClient('127.0.0.1')
            clients.append(admin)
            for instance in ([] if args.fixed_base else config['robots']):
                reply = admin.reset(instance['actor_id'])
                assert reply['ok'], reply
                reply = admin.lock_pose(instance['actor_id'], translation_m=instance['translation_m'],
                                        rotation_quat_xyzw=instance['rotation_quat_xyzw'])
                assert reply['ok'], reply
            angle = .18
            if guest: guest.set_camera([-3, -1.8, 1.3], [-math.sin(.2)*math.sin(angle), math.cos(.2)*math.sin(angle), math.sin(.2)*math.cos(angle), math.cos(.2)*math.cos(angle)])
            labels = ['pi_plus', 'mos9' if args.second_robot == 'mos9' else 'pi_plus_2']+(['guest'] if guest else [])
            writers = [imageio.get_writer(str(output/(name+'.mp4')), fps=30, codec='libx264', macro_block_size=1) for name in labels]
            frames = [0]*len(streams)
            state_counts = [0, 0]
            yaw_positions = [[], []]
            started = time.monotonic()
            next_command = started
            while time.monotonic()-started < 8:
                now = time.monotonic()
                elapsed = now-started
                if now >= next_command:
                    for i, robot in enumerate(robots):
                        values = {name: 0 for name in states[i]['actuators']}
                        for name in values:
                            if 'head_yaw' in name: values[name] = .45*math.sin(elapsed*1.3)
                            elif 'head_pitch' in name: values[name] = .15*math.sin(elapsed*1.7)
                        robot.send_command(values)
                        # Drive a deterministic render pose; lock_pose freezes all joints.
                        pose = [.45*math.sin(elapsed*1.3) if 'head_yaw' in name else
                                .15*math.sin(elapsed*1.7) if 'head_pitch' in name else 0
                                for name in states[i]['joints']]
                        if not args.fixed_base:
                            reply = admin.lock_pose(config['robots'][i]['actor_id'], joint_qpos=pose)
                            assert reply['ok'], reply
                    next_command = now+1/30
                for i, client in enumerate(streams):
                    for kind, data in client.recv():
                        if kind == 'state':
                            state_counts[i] += 1
                            for name, joint in data['joints'].items():
                                if 'head_yaw' in name: yaw_positions[i].append(joint['qpos'])
                        elif kind == 'status': assert data['ok'], data
                        elif kind == 'rgb':
                            pixels = camera_to_rgb(data[0])
                            assert pixels.shape == (480, 640, 3) and np.std(pixels)>5
                            frames[i] += 1
                            writers[i].append_data(pixels)
                            for mark in (3, 4, 5):
                                path = output/f'{labels[i]}_{mark}s.png'
                                if elapsed >= mark and not path.exists(): Image.fromarray(pixels).save(path)
                time.sleep(.001)
            assert min(frames)>150 and min(state_counts)>300, (frames, state_counts)
            ranges = [max(values)-min(values) for values in yaw_positions]
            assert min(ranges)>.2, ranges
            text = ue_log.read_text(errors='replace')
            failures = [line for line in text.splitlines() if 'Failed to initialize' in line]
            if args.fixed_base: failures = [line for line in failures if not line.endswith(': fixed_base')]
            assert not failures and 'External robot attach:' not in text, failures
            result = dict(frames=frames, states=state_counts, head_yaw_ranges=ranges)
            (output/'results.json').write_text(json.dumps(result, indent=2)+'\n')
            print(json.dumps(result), flush=True)
        finally:
            for writer in writers: writer.close()
            for client in clients: client.close()
            stop(process)


if __name__ == '__main__': main()
