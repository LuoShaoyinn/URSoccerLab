#!/usr/bin/env python3
"""Check the JSON-only packaged launcher, stereo/RGBD and guest AV1 streams.
Uses the local external Booster package and ball PBR fixture from source validation.
"""
import json
import math
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
import numpy as np
from PIL import Image
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'py_example/src'))
from ursoccerlab import InspectorClient, RobotClient
from ursoccerlab.tcp import AdminClient
from ursoccerlab.media import camera_to_rgb, depth_to_meters


def main():
    image=ROOT/'dist/URSoccerLab.AppImage'
    output=ROOT/'artifacts/tests/appimage';output.mkdir(parents=True,exist_ok=True)
    environment=dict(os.environ,APPIMAGE_EXTRACT_AND_RUN='1',XDG_DATA_HOME=str(output/'runtime'))
    for args in [[],['missing.json'],[str(ROOT/'Config/URS_scene.json'),'-NoSound']]:
        r=subprocess.run([str(image),*args],env=environment,capture_output=True,text=True,timeout=30)
        assert r.returncode==2,(args,r.returncode,r.stderr[-1000:])
    config=json.loads((ROOT/'artifacts/tests/ball-pbr/scene.json').read_text())
    config['guest_inspector'].update(max_guests=1,compression='av1')
    config['vision']['rgb'].update(compression='av1',keyframe_interval_s=2)
    report={}
    for mode in ['stereo_rgb','rgbd']:
        config['vision']['mode']=mode
        scene=output/'scene.json';scene.write_text(json.dumps(config,indent=2))
        clients=[]
        with (output/(mode+'.log')).open('w') as log:
            process=subprocess.Popen([str(image),str(scene)],env=environment,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
            try:
                deadline=time.monotonic()+90
                while time.monotonic()<deadline:
                    if process.poll() is not None:raise RuntimeError('AppImage exited; see '+str(output/(mode+'.log')))
                    text=(output/(mode+'.log')).read_text(errors='replace')
                    if '[URS Inspector] listening on port 12000' in text:break
                    time.sleep(.1)
                else:raise TimeoutError('packaged startup')
                time.sleep(2)
                admin=AdminClient('127.0.0.1');clients.append(admin)
                assert admin.lock_pose('robot_rp0',[-1,0,.552],[0,0,0,1],[0]*22)['ok']
                robot=RobotClient('127.0.0.1');guest=InspectorClient();clients.extend([robot,guest])
                guest.set_camera([1,-1,.5],[-math.sin(3*math.pi/8)*math.sin(.1),math.cos(3*math.pi/8)*math.sin(.1),math.sin(3*math.pi/8)*math.cos(.1),math.cos(3*math.pi/8)*math.cos(.1)])
                counts=[0,0];states=depths=0;latest=None;deadline=time.monotonic()+6
                while time.monotonic()<deadline:
                    for i,client in enumerate([robot,guest]):
                        for kind,data in client.recv():
                            if kind=='state':states+=1;assert len(data['actuators'])==22
                            elif kind=='depth':
                                depth=depth_to_meters(data[0]);assert depth.shape==(480,640) and np.isfinite(depth).all() and (depth>0).any();depths+=1
                            elif kind=='rgb':
                                assert all(e['codec']=='av1' for e in data)
                                assert len(data)==(2 if i==0 and mode=='stereo_rgb' else 1)
                                pixels=camera_to_rgb(data[0]);assert pixels.shape==(480,640,3) and np.std(pixels)>5
                                counts[i]+=1
                                if i==1:latest=pixels
                    time.sleep(.002)
                assert min(counts)>30 and states>60,(counts,states)
                if mode=='rgbd':assert depths>30,depths
                assert 'external PBR applied to 1 material slots' in (output/(mode+'.log')).read_text(errors='replace')
                if mode=='stereo_rgb':Image.fromarray(latest).save(output/'guest.png')
                report[mode]={'rgb_frames':counts,'states':states,'depth_frames':depths,'seconds':6}
                print(mode,report[mode],flush=True)
            finally:
                for client in clients:client.close()
                try:os.killpg(process.pid,signal.SIGTERM)
                except ProcessLookupError:pass
                try:process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid,signal.SIGKILL);process.wait()
    (output/'results.json').write_text(json.dumps(report,indent=2)+'\n')


if __name__=='__main__':main()
