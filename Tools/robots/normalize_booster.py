#!/usr/bin/env python3
"""Convert the supplied Booster K1 archive to an external urs_robot_v1 package.
Requires numpy, trimesh and mujoco. Camera placement and servo gains are provisional.
Source joint names, inertia tensors, collision geometry and collision scale are retained.
"""
import argparse, copy, io, json, struct, zipfile
from pathlib import Path
import xml.etree.ElementTree as ET
import numpy as np
import trimesh
import mujoco


def vec(s): return np.fromstring(s, sep=' ')
def fmt(v): return ' '.join(f'{x:.12g}' for x in v)
def rotation(rpy):
    r,p,y=vec(rpy); cr,sr=np.cos(r),np.sin(r); cp,sp=np.cos(p),np.sin(p); cy,sy=np.cos(y),np.sin(y)
    return np.array([[cy*cp,cy*sp*sr-sy*cr,cy*sp*cr+sy*sr],[sy*cp,sy*sp*sr+cy*cr,sy*sp*cr-cy*sr],[-sp,cp*sr,cp*cr]])
def origin(node):
    o=node.find('origin'); return (o.get('xyz','0 0 0'),o.get('rpy','0 0 0')) if o is not None else ('0 0 0','0 0 0')
def read_glb(data):
    length=struct.unpack_from('<I',data,12)[0]; g=json.loads(data[20:20+length]); start=20+length
    size=struct.unpack_from('<I',data,start)[0]; return g,data[start+8:start+8+size]
def split_glb(g, binary, nodes, target):
    out={'asset':g['asset'],'scene':0,'scenes':[{'nodes':list(range(len(nodes)))}], 'nodes':[], 'meshes':[], 'materials':g['materials'], 'accessors':[], 'bufferViews':[]}
    accmap={};viewmap={};blob=bytearray()
    def accessor(i):
        if i in accmap:return accmap[i]
        a=copy.deepcopy(g['accessors'][i]); old=a['bufferView']
        if old not in viewmap:
            v=copy.deepcopy(g['bufferViews'][old]); blob.extend(b'\0'*((-len(blob))%4)); offset=len(blob)
            blob.extend(binary[v.get('byteOffset',0):v.get('byteOffset',0)+v['byteLength']]);v['buffer']=0;v['byteOffset']=offset
            viewmap[old]=len(out['bufferViews']);out['bufferViews'].append(v)
        a['bufferView']=viewmap[old];accmap[i]=len(out['accessors']);out['accessors'].append(a);return accmap[i]
    for node in nodes:
        mesh=copy.deepcopy(g['meshes'][node['mesh']])
        for p in mesh['primitives']:
            p['attributes']={k:accessor(v) for k,v in p['attributes'].items()}
            if 'indices' in p:p['indices']=accessor(p['indices'])
            if 'targets' in p:raise ValueError('Morph targets not supported')
        # Geometry is already link-local; source matrices pose the assembled robot.
        out['nodes'].append({'name':node['name'],'mesh':len(out['meshes'])});out['meshes'].append(mesh)
    out['buffers']=[{'byteLength':len(blob)}];js=json.dumps(out,separators=(',',':')).encode();js+=b' '*((-len(js))%4);blob.extend(b'\0'*((-len(blob))%4))
    target.write_bytes(struct.pack('<III',0x46546c67,2,12+8+len(js)+8+len(blob))+struct.pack('<II',len(js),0x4e4f534a)+js+struct.pack('<II',len(blob),0x004e4942)+blob)


def main():
    p=argparse.ArgumentParser();p.add_argument('archive',type=Path);p.add_argument('--output',type=Path,default=Path('external/robots/booster_k1'));a=p.parse_args()
    if a.output.exists():raise SystemExit('Output exists; choose a fresh output directory.')
    z=zipfile.ZipFile(a.archive);prefix='Booster_K1_URDF_PBR/';urdf=ET.fromstring(z.read(prefix+'K1_locomotion_all_joints_mesh.urdf'))
    links={l.get('name'):l for l in urdf.findall('link')};joints=urdf.findall('joint');children={j.find('child').get('link'):j for j in joints};roots=set(links)-set(children)
    assert roots=={'Trunk'}
    g,binary=read_glb(z.read(prefix+'Booster_K1_PBR.glb'));assert not g.get('images') and not g.get('skins')
    groups={name:[] for name in links}
    for n in g['nodes']:groups[n['name'].rsplit('__',1)[0]].append(n)
    out=a.output;meshes=out/'meshes';meshes.mkdir(parents=True)
    for n in z.namelist():
        if n.startswith(prefix+'meshes/') and n.lower().endswith('.stl'):(meshes/Path(n).name).write_bytes(z.read(n))
    # Compare local visual bounds against unscaled source STL in the same frame.
    C=np.array([[1,0,0],[0,0,-1],[0,1,0]]) # glTF Y-up -> MuJoCo Z-up
    bounds_errors={}
    for name,ns in groups.items():
        assert ns,name
        path=meshes/(name+'.glb');split_glb(g,binary,ns,path)
        scene=trimesh.load(path,force='scene');verts=np.concatenate([m.vertices for m in scene.geometry.values()]);v=verts@C.T
        source=links[name].find('visual/geometry/mesh').get('filename');stl=trimesh.load(io.BytesIO(z.read(prefix+source)),file_type='stl')
        err=float(np.max(np.abs(np.array([v.min(0),v.max(0)])-stl.bounds)));bounds_errors[name]=err
        if err>.003:raise ValueError(f'{name}: GLB/STL bounds differ by {err} metres')
    root=ET.Element('mujoco',model='booster_k1');ET.SubElement(root,'compiler',angle='radian',eulerseq='xyz',meshdir='meshes',inertiafromgeom='false',autolimits='true')
    asset=ET.SubElement(root,'asset');world=ET.SubElement(root,'worldbody');bodies={};collision_id=0
    def add_body(name,parent):
        nonlocal collision_id
        link=links[name];joint=children.get(name); xyz,rpy=origin(joint) if joint is not None else ('0 0 0','0 0 0')
        body=ET.SubElement(parent,'body',name=name,pos=xyz);bodies[name]=body
        # URDF rotations are extrinsic XYZ. Use quaternion to avoid MJCF Euler ambiguity.
        q=np.empty(4);mujoco.mju_mat2Quat(q,rotation(rpy).flatten());body.set('quat',fmt(q))
        inertial=link.find('inertial');ipos,irpy=origin(inertial);I=inertial.find('inertia');t=np.array([[float(I.get('ixx')),float(I.get('ixy')),float(I.get('ixz'))],[float(I.get('ixy')),float(I.get('iyy')),float(I.get('iyz'))],[float(I.get('ixz')),float(I.get('iyz')),float(I.get('izz'))]]);R=rotation(irpy);t=R@t@R.T
        ET.SubElement(body,'inertial',pos=ipos,mass=inertial.find('mass').get('value'),fullinertia=fmt([t[0,0],t[1,1],t[2,2],t[0,1],t[0,2],t[1,2]]))
        if joint is None:ET.SubElement(body,'freejoint',name='root')
        else:
            limit=joint.find('limit');ET.SubElement(body,'joint',name=joint.get('name'),type='hinge',axis=joint.find('axis').get('xyz'),range=limit.get('lower')+' '+limit.get('upper'))
        ET.SubElement(asset,'mesh',name='visual_'+name,file=name+'.glb');ET.SubElement(body,'geom',name='visual_'+name,type='mesh',mesh='visual_'+name,contype='0',conaffinity='0',mass='0',group='2')
        for col in link.findall('collision'):
            xyz,rpy=origin(col);q=np.empty(4);mujoco.mju_mat2Quat(q,rotation(rpy).flatten());geom=ET.SubElement(body,'geom',name=f'collision_{collision_id}',pos=xyz,quat=fmt(q),group='3');collision_id+=1
            shape=list(col.find('geometry'))[0]
            if shape.tag=='mesh':
                mn=f'collision_mesh_{collision_id}';ET.SubElement(asset,'mesh',name=mn,file=Path(shape.get('filename')).name,scale=shape.get('scale','1 1 1'));geom.set('type','mesh');geom.set('mesh',mn)
            elif shape.tag=='cylinder':geom.set('type','cylinder');geom.set('size',fmt([float(shape.get('radius')),float(shape.get('length'))/2]))
            elif shape.tag=='box':geom.set('type','box');geom.set('size',fmt(vec(shape.get('size'))/2))
            elif shape.tag=='sphere':geom.set('type','sphere');geom.set('size',shape.get('radius'))
            else:raise ValueError(shape.tag)
        for j in joints:
            if j.find('parent').get('link')==name:add_body(j.find('child').get('link'),body)
    add_body('Trunk',world)
    for name,y in [('left_eye',.03),('right_eye',-.03)]:ET.SubElement(bodies['Head_2'],'camera',name=name,pos=fmt([.1,y,.08]),xyaxes='0 -1 0 0 0 1',fovy='60',resolution='640 480')
    act=ET.SubElement(root,'actuator')
    for j in joints:
        limit=j.find('limit');effort=float(limit.get('effort'));ET.SubElement(act,'position',name=j.get('name')+'_servo',joint=j.get('name'),kp='50',kv='1',ctrlrange=limit.get('lower')+' '+limit.get('upper'),forcerange=fmt([-effort,effort]))
    ET.indent(root);ET.ElementTree(root).write(out/'model.xml',encoding='unicode')
    physics=copy.deepcopy(root);glbs={m.get('name') for m in physics.find('asset') if m.get('file','').endswith('.glb')}
    for parent in physics.iter():
        for child in list(parent):
            if (child.tag=='mesh' and child.get('name') in glbs) or (child.tag=='geom' and child.get('mesh') in glbs):parent.remove(child)
    physics.find('compiler').set('meshdir',str(meshes.resolve()));model=mujoco.MjModel.from_xml_string(ET.tostring(physics,encoding='unicode'));data=mujoco.MjData(model);mujoco.mj_forward(model,data)
    # Zero-joint pose: raise root until the lowest collision vertex touches z=0.
    lows=[]
    for i in range(model.ngeom):
        if model.geom_type[i]==mujoco.mjtGeom.mjGEOM_MESH:
            mid=model.geom_dataid[i];start=model.mesh_vertadr[mid];count=model.mesh_vertnum[mid];verts=model.mesh_vert[start:start+count]@data.geom_xmat[i].reshape(3,3).T+data.geom_xpos[i];lows.append(float(verts[:,2].min()))
    height=-min(lows)
    manifest={'version':'urs_robot_v1','id':'booster_k1','model':'model.xml','default_base_height_m':height,'bindings':{'base_body':'Trunk','head_body':'Head_2','left_camera':'left_eye','right_camera':'right_eye'}}
    (out/'robot.json').write_text(json.dumps(manifest,indent=2)+'\n')
    report={'links':len(links),'hinge_joints':len(joints),'actuators':model.nu,'mass_kg':float(model.body_mass.sum()),'default_base_height_m':height,'max_visual_bounds_error_m':max(bounds_errors.values()),'visual_bounds_errors_m':bounds_errors,'missing_collision':[n for n,l in links.items() if l.find('collision') is None],'camera_placement':'provisional: head local [0.1, +/-0.03, 0.08]','servo_gains':'provisional kp=50 kv=1, torque limits from URDF','inertia_policy':'original tensors; balanceinertia disabled; compiler must accept them unchanged'}
    (out/'normalization.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
