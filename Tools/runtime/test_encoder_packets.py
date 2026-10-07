#!/usr/bin/env python3
"""Decode packets from test_encoder_backend.cpp through the Python client.
Usage: python test_encoder_packets.py CODEC OUTPUT_PREFIX
"""
import struct
import sys
from pathlib import Path
import numpy as np
from ursoccerlab.video import VideoDecoder
from ursoccerlab.media import camera_to_rgb

paths=sorted(Path(sys.argv[2]).parent.glob(Path(sys.argv[2]).name+'-*.pkts'))
assert len(paths)>=2, 'Missing diagnostic packet files'
for path in paths:
    width=int(path.stem.rsplit('-',1)[1]);height=600 if width==800 else 480
    data=path.read_bytes()
    offset=0; messages=[]
    while offset<len(data):
        size,config_size,coded_w,coded_h,key,pts=struct.unpack_from('<IIIIIq',data,offset)
        offset+=28
        config=data[offset:offset+config_size];offset+=config_size
        packet=data[offset:offset+size];offset+=size
        right=b'right_eye' if width==1280 else b''
        envelope=struct.pack('<BBQHHIB',1,bool(right),1,coded_w,coded_h,len(config),len(right))+right+config+packet
        messages.append(dict(codec=sys.argv[1],camera_name='left_eye',width=width,height=height,
                             sequence=pts,keyframe=bool(key),sim_time=pts/30,data=envelope))
    decoder=VideoDecoder(); count=0
    for message in messages:
        eyes=decoder.decode([message]);assert len(eyes)==(2 if width==1280 else 1),(width,message['sequence'])
        for eye in eyes:
            pixels=camera_to_rgb(eye)
            assert pixels.shape==(height,640 if width==1280 else width,3) and np.std(pixels)>5
        count+=1
    assert count==90,count
    # Joining at a dependent frame and dropping a packet wait for periodic recovery.
    decoder=VideoDecoder();assert decoder.decode([messages[1]])==[]
    assert decoder.decode([messages[30]])
    assert decoder.decode([messages[32]])==[]
    assert decoder.decode([messages[60]])
    print(f'{sys.argv[1]} width={width}: decoded={count}, join/gap recovery passed')
