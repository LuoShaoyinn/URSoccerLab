#!/usr/bin/env python3
"""Test external maps in the source runtime; never cook or rebuild the AppImage.

Run with py_example/.venv/bin/python Tools/runtime/test_change_map.py.
Requires the built editor, Pillow and the Python client dependencies.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'py_example/src'))
from ursoccerlab.gains import detect_gains
from ursoccerlab.media import camera_to_rgb
from ursoccerlab.tcp import RobotClient


def capture(ue: Path, output: Path, name: str, color: tuple[int, int, int], length: float, width: float) -> dict:
    image = Image.new('RGB', (800, 500), color)
    draw = ImageDraw.Draw(image)
    margin_x, margin_y = round(800*0.5/(length+1)), round(500*0.5/(width+1))
    draw.rectangle((margin_x, margin_y, 800-margin_x, 500-margin_y), outline='white', width=5)
    draw.line((400, margin_y, 400, 500-margin_y), fill='white', width=5)
    image.save(output / f'{name}.png')
    config = json.loads((ROOT / 'py_example/examples/standing/scene.json').read_text())
    config['field'] = dict(length_m=length, width_m=width, border_x_m=0.5, border_y_m=0.5, map_image=f'{name}.png')
    config['goals']['poses'] = [dict(translation_m=[-length/2,0,0],yaw_deg=0), dict(translation_m=[length/2,0,0],yaw_deg=180)]
    config['objects'][0]['physics'] = dict(radius_m=0.11, mass_kg=0.43, friction=[0.6, 0.005, 0.001], solref=[0.02, 0.7])
    config['objects'][0].pop('translation_m', None)
    config['robots'][0]['privilege'] = dict(all_pos=True)
    scene = output / f'{name}.json'
    scene.write_text(json.dumps(config, indent=2)+'\n')
    env = dict(os.environ)
    env['LD_LIBRARY_PATH'] = str(ROOT / 'Plugins/UnrealRoboticsLab/Binaries/Linux') + ':' + env.get('LD_LIBRARY_PATH', '')
    command = [sys.executable, str(ROOT/'Tools/runtime/run_scene.py'), '--ue', str(ue), '--scene-config', str(scene), '--sim-extra-arg=-FORCELOGFLUSH', f'--sim-extra-arg=-abslog={output / (name + "_ue.log")}']
    log_path = output / f'{name}.log'
    (output / f'{name}_ue.log').unlink(missing_ok=True)
    with log_path.open('w') as log:
        process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        client = None
        try:
            deadline = time.monotonic()+60
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError(f'{name}: simulator exited; see {log_path}')
                ready_log = output / f'{name}_ue.log'
                if not ready_log.exists() or ' listening on port 10000' not in ready_log.read_text(errors='replace'):
                    time.sleep(0.2)
                    continue
                try:
                    client = RobotClient('127.0.0.1', 10000)
                    break
                except OSError:
                    time.sleep(0.2)
            if client is None:
                raise RuntimeError(f'{name}: no TCP endpoint; see {log_path}')
            target = None
            best = None
            fraction = 0.0
            frames = 0
            states = 0
            deadline = time.monotonic()+8
            while time.monotonic() < deadline:
                for kind, data in client.recv():
                    if kind == 'state':
                        states += 1
                        if target is None:
                            actuators = list(data.get('actuators', {}))
                            client.set_controller_params(**detect_gains(actuators), actuator_mode='position')
                            target = {a: data.get('joints', {}).get(a.replace('_servo',''), {}).get('qpos', 0) for a in actuators}
                    elif kind in ('rgb', 'camera'):
                        for camera in data:
                            if not camera.get('data'):
                                continue
                            rgb = camera_to_rgb(camera)
                            frames += 1
                            channel = 0 if name == 'red' else 2
                            other = 2 if channel == 0 else 0
                            pixels = rgb.astype(np.int16)
                            score = float(np.mean((pixels[:,:,channel] > 50) & (pixels[:,:,channel] > pixels[:,:,other]*1.5) & (pixels[:,:,channel] > pixels[:,:,1]*1.5)))
                            if best is None or score > fraction:
                                best, fraction = rgb.copy(), score
                if target:
                    client.send_command(target)
                time.sleep(0.01)
            if best is None or fraction < 0.01 or not states:
                raise RuntimeError(f'{name}: external map not visible (fraction={fraction}, frames={frames}, states={states}); see {log_path}')
            Image.fromarray(best).save(output/f'{name}_camera.png')
            return dict(color_fraction=fraction, frames=frames, states=states, length_m=length, width_m=width)
        finally:
            if client:
                client.close()
            import signal
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ue', type=Path, default=Path.home()/'software/Unreal_Engine_5.7.4/Engine/Binaries/Linux/UnrealEditor')
    parser.add_argument('--out', type=Path, default=ROOT/'Saved/Tests/change-map-render')
    args = parser.parse_args()
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=True)
    results = {}
    for name, color, length, width in [('red', (180,15,15),7.0,4.0), ('blue',(15,15,180),9.0,6.0)]:
        results[name] = capture(args.ue, output, name, color, length, width)
        print(name, results[name], flush=True)
    (output/'results.json').write_text(json.dumps(results, indent=2)+'\n')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
