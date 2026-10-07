#!/usr/bin/env python3
"""Check the JSON-only packaged launcher, stereo/RGBD and guest video streams.
Uses the local external Booster package and ball PBR fixture from source validation.
"""
import json
import argparse
import math
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import time
import numpy as np
from PIL import Image
ROOT=Path(__file__).resolve().parents[2]
from ursoccerlab import InspectorClient, RobotClient
from ursoccerlab.tcp import AdminClient
from ursoccerlab.media import camera_to_rgb, depth_to_meters


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--shipping',action='store_true',help='Shipping disables engine logs; skip the PBR log assertion.')
    parser.add_argument('--builtin-ball',action='store_true',help='Render the bundled ball skin instead of external checker PBR maps.')
    parser.add_argument('--executable',type=Path,default=ROOT/'dist/URSoccerLab.AppImage',help='AppImage or extracted AppRun for source-runtime validation.')
    parser.add_argument('--scene',type=Path,default=ROOT/'artifacts/tests/ball-pbr/scene.json')
    parser.add_argument('--output',type=Path,default=ROOT/'artifacts/tests/appimage')
    parser.add_argument('--backend',default='auto',choices=['auto','nvenc','qsv','vaapi','vulkan'])
    parser.add_argument('--codec',default='av1',choices=['av1','h264','h265'])
    parser.add_argument('--fallback-codec',default='h264',choices=['h264','h265','none'])
    parser.add_argument('--expected-codec',default='av1',choices=['av1','h264','h265'])
    parser.add_argument('--mode',default='all',choices=['all','stereo_rgb','rgbd'])
    parser.add_argument('--duration',type=float,default=6,help='Capture seconds per mode; allow more time on a busy shared GPU.')
    options=parser.parse_args()
    image=options.executable.resolve()
    output=options.output.resolve();output.mkdir(parents=True,exist_ok=True)
    environment=dict(os.environ,APPIMAGE_EXTRACT_AND_RUN='1',XDG_DATA_HOME=str(output/'runtime'))
    for args in [[],['missing.json'],[str(ROOT/'Config/URS_scene.json'),'-NoSound']]:
        r=subprocess.run([str(image),*args],env=environment,capture_output=True,text=True,timeout=30)
        assert r.returncode==2,(args,r.returncode,r.stderr[-1000:])
    config=json.loads(options.scene.read_text())
    if options.builtin_ball:
        for obj in config['objects']:
            if obj['type']=='soccer_ball':
                obj.pop('visual',None)
    config['encoder']={'codec':options.codec,'backend':options.backend,'fallback_codec':None if options.fallback_codec=='none' else options.fallback_codec}
    config['guest_inspector'].pop('compression',None)
    config['vision']['rgb'].pop('compression',None)
    config['guest_inspector'].update(max_guests=1)
    config['vision']['rgb'].update(keyframe_interval_s=2)
    report={}
    for mode in (['stereo_rgb','rgbd'] if options.mode=='all' else [options.mode]):
        config['vision']['mode']=mode
        scene=output/'scene.json';scene.write_text(json.dumps(config,indent=2))
        clients=[]
        with (output/(mode+'.log')).open('w') as log:
            process=subprocess.Popen([str(image),str(scene)],env=environment,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
            try:
                deadline=time.monotonic()+90
                while time.monotonic()<deadline:
                    if process.poll() is not None:raise RuntimeError('AppImage exited; see '+str(output/(mode+'.log')))
                    try:
                        for port in (10000,11000,12000):
                            with socket.create_connection(('127.0.0.1',port),timeout=.1):
                                pass
                        break
                    except OSError:
                        time.sleep(.1)
                else:raise TimeoutError('packaged startup')
                time.sleep(2)
                admin=AdminClient('127.0.0.1');clients.append(admin)
                assert admin.lock_pose('robot_rp0',[-1,0,.552],[0,0,0,1],[0]*22)['ok']
                robot=RobotClient('127.0.0.1');guest=InspectorClient();clients.extend([robot,guest])
                guest.set_camera([1,-1,.5],[-math.sin(3*math.pi/8)*math.sin(.1),math.cos(3*math.pi/8)*math.sin(.1),math.sin(3*math.pi/8)*math.cos(.1),math.cos(3*math.pi/8)*math.cos(.1)])
                counts=[0,0];states=depths=0;latest=None;deadline=time.monotonic()+options.duration
                while time.monotonic()<deadline:
                    for i,client in enumerate([robot,guest]):
                        for kind,data in client.recv():
                            if kind=='state':states+=1;assert len(data['actuators'])==22
                            elif kind=='depth':
                                depth=depth_to_meters(data[0]);assert depth.shape==(480,640) and np.isfinite(depth).all() and (depth>0).any();depths+=1
                            elif kind=='rgb':
                                assert all(e['codec']==options.expected_codec for e in data)
                                assert len(data)==(2 if i==0 and mode=='stereo_rgb' else 1)
                                pixels=camera_to_rgb(data[0])
                                expected_shape=(480,640,3) if i==0 else (config['guest_inspector']['height'],config['guest_inspector']['width'],3)
                                assert pixels.shape==expected_shape and np.std(pixels)>5
                                counts[i]+=1
                                if i==1:latest=pixels
                    time.sleep(.002)
                assert min(counts)>30 and states>60,(counts,states)
                if mode=='rgbd':assert depths>30,depths
                runtime_log=output/'runtime/URSoccerLab/Saved/Logs/URSoccerLab.log'
                if not options.shipping and not options.builtin_ball:
                    assert 'external PBR applied to 1 material slots' in runtime_log.read_text(errors='replace')
                if mode=='stereo_rgb':Image.fromarray(latest).save(output/'guest.png')
                report[mode]={'rgb_frames':counts,'states':states,'depth_frames':depths,'seconds':options.duration,'codec':options.expected_codec,'backend':options.backend}
                print(mode,report[mode],flush=True)
            finally:
                for client in clients:client.close()
                try:os.killpg(process.pid,signal.SIGTERM)
                except ProcessLookupError:pass
                try:process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid,signal.SIGKILL);process.wait()
                # The AppImage extraction wrapper can exit before its game child.
                # Wait for socket shutdown before probing the next process's ports.
                deadline=time.monotonic()+15
                while time.monotonic()<deadline:
                    busy=False
                    for port in (10000,11000,12000):
                        try:
                            with socket.create_connection(('127.0.0.1',port),timeout=.1):
                                busy=True
                        except OSError:
                            pass
                    if not busy:break
                    time.sleep(.1)
                else:raise TimeoutError('packaged socket shutdown')
    (output/'results.json').write_text(json.dumps(report,indent=2)+'\n')


if __name__=='__main__':main()
