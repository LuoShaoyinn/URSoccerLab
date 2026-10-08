#!/usr/bin/env python3
"""Render external PBR maps through robot and inspector cameras, without cooking."""
import json
import math
import os
import subprocess
import sys
import time
from pathlib import Path

import imageio.v2 as imageio
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
from ursoccerlab import InspectorClient, RobotClient
from ursoccerlab.media import camera_to_rgb
from test_network_refactor import stop


def capture(output, detailed):
    label = 'pbr' if detailed else 'base_only'
    folder = output / label
    folder.mkdir(parents=True, exist_ok=True)
    original = ROOT / 'py_example/examples/standing/scene.json'
    config = json.loads(original.read_text())
    config["robot_types"] = {name: str((original.parent / path).resolve()) for name, path in config["robot_types"].items()}
    config['field']['visual'] = dict(base_color_map=str((original.parent / config['field']['visual']['base_color_map']).resolve()),
                                    detail_tile_size_m=.5, roughness=.8, metallic=0)
    if detailed:
        config['field']['visual'].update(normal_map='../normal.png', roughness_map='../roughness.png',
                                        metallic_map='../metallic.png', ao_map='../ao.png')
    config['field']['physics'].update(friction=[.4,.007,.002],condim=4,solref=[.015,.8])
    config['guest_inspector'].update(max_guests=1, width=800, height=600,rate_hz=30,bitrate_kbps=8000)
    config['vision']['rgb']['bitrate_kbps'] = 6000
    scene = folder / 'scene.json'
    scene.write_text(json.dumps(config,indent=2)+'\n')
    ue_log = folder/'ue.log'
    ue_log.unlink(missing_ok=True)
    command = [sys.executable,str(ROOT/'Tools/runtime/run_scene.py'),'--scene-config',str(scene),
               f'--sim-extra-arg=-abslog={ue_log}']
    env = dict(os.environ)
    env['LD_LIBRARY_PATH'] = str(ROOT/'Plugins/UnrealRoboticsLab/Binaries/Linux')+':'+env.get('LD_LIBRARY_PATH','')
    clients=[]
    frames=[]
    with (folder/'launcher.log').open('w') as log:
        process = subprocess.Popen(command,cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
        writer=None
        try:
            deadline=time.monotonic()+90
            while time.monotonic()<deadline:
                if process.poll() is not None: raise RuntimeError(f'{label}: simulator exited; see {ue_log}')
                if ue_log.exists() and ' listening on port 10001' in ue_log.read_text(errors='replace'): break
                time.sleep(.1)
            else: raise TimeoutError('simulator startup')
            robot=RobotClient('127.0.0.1'); guest=InspectorClient()
            clients=[robot,guest]
            pitch=math.pi/8
            guest.set_camera([-2.5,0,2.5],[0,math.sin(pitch),0,math.cos(pitch)])
            pending=True
            started=time.monotonic(); last_pose=started
            rgb_counts=[0,0]; states=0
            writer=imageio.get_writer(str(folder/'guest_moving.mp4'),fps=30,codec='libx264',macro_block_size=1)
            while time.monotonic()-started < 7:
                now=time.monotonic(); elapsed=now-started
                if elapsed>3 and not pending and now-last_pose>.15:
                    yaw=math.sin((elapsed-3)*1.3)*.2
                    guest.set_camera([-2.5,0,2.5],[-math.sin(yaw)*math.sin(pitch),math.cos(yaw)*math.sin(pitch),
                                                           math.sin(yaw)*math.cos(pitch),math.cos(yaw)*math.cos(pitch)])
                    pending=True; last_pose=now
                for index,client in enumerate(clients):
                    for kind,data in client.recv():
                        if kind=='state': states+=1
                        elif kind=='status':
                            assert data['ok'],data
                            pending=False
                        elif kind=='rgb':
                            pixels=camera_to_rgb(data[0]); rgb_counts[index]+=1
                            assert float(np.std(pixels))>5
                            if index==0 and rgb_counts[index]==30: Image.fromarray(pixels).save(folder/'robot.png')
                            if index==1:
                                assert pixels.shape==(600,800,3)
                                writer.append_data(pixels)
                                if 2<elapsed<3: frames.append(pixels.astype(np.float32))
                                for mark in (4,5,6):
                                    path=folder/f'guest_{mark}s.png'
                                    if elapsed>=mark and not path.exists(): Image.fromarray(pixels).save(path)
                time.sleep(.002)
            assert min(rgb_counts)>100 and states>100,(rgb_counts,states)
            assert frames,'no static comparison frames'
            text=ue_log.read_text(errors='replace')
            assert 'URS field physics: friction=0.4,0.007,0.002 condim=4' in text
            assert 'Error: [URS AV1]' not in text
            averaged=np.median(frames,axis=0)
            Image.fromarray(averaged.astype(np.uint8)).save(folder/'static.png')
            result=dict(frames=rgb_counts,states=states,static_frames=len(frames))
            (folder/'results.json').write_text(json.dumps(result,indent=2)+'\n')
            return averaged,result
        finally:
            if writer: writer.close()
            for client in clients: client.close()
            stop(process)


def main():
    output=ROOT/'artifacts/tests/field-pbr-render'
    output.mkdir(parents=True,exist_ok=True)
    y,x=np.mgrid[0:64,0:64]
    normal=np.stack([128+75*np.sin(x*2*np.pi/64),128+75*np.cos(y*2*np.pi/64),np.full_like(x,235)],axis=2).astype(np.uint8)
    Image.fromarray(normal).save(output/'normal.png')
    for name,values in [('roughness',np.where(x<32,32,220)),('metallic',np.where(y<32,0,100)),
                        ('ao',np.where((x//16+y//16)%2,64,255))]:
        Image.fromarray(values.astype(np.uint8)).save(output/(name+'.png'))
    base,base_result=capture(output,False)
    pbr,pbr_result=capture(output,True)
    mask=(base[:,:,1]>base[:,:,0]*1.2)&(base[:,:,1]>base[:,:,2]*1.2)&(base[:,:,1]>40)
    assert mask.mean()>.1,'field not visible'
    difference=float(np.abs(base-pbr)[mask].mean())
    assert difference>2,('PBR maps did not change rendered field',difference)
    result=dict(base_only=base_result,pbr=pbr_result,field_pixel_fraction=float(mask.mean()),field_mean_absolute_difference=difference)
    (output/'results.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result),flush=True)


if __name__=='__main__': main()
