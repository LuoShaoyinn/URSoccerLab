#!/usr/bin/env python3
"""Migrate the old editor material override into an external Pi Plus GLB package.

Read original meshes without modification. Write material-bearing GLBs and shared
textures into the external package. No Unreal assets or cooking are involved.
"""
import argparse
import json
import math
from pathlib import Path
import shutil
import struct

from PIL import Image, ImageOps


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=root/'external/robots/legacy_source/pi_plus/meshes')
    parser.add_argument('--textures', type=Path, default=root/'refs/powder-coated-metal-ue')
    parser.add_argument('--package', type=Path, default=root/'external/robots/pi_plus')
    args = parser.parse_args()
    textures = args.package/'textures'
    textures.mkdir(parents=True, exist_ok=True)
    prefix = 'powder-coated-metal_'
    shutil.copy2(args.textures/(prefix+'albedo.png'), textures/'base_color.png')
    shutil.copy2(args.textures/(prefix+'ao.png'), textures/'occlusion.png')
    rough = Image.open(args.textures/(prefix+'roughness.png')).convert('L')
    metal = Image.open(args.textures/(prefix+'metallic.png')).convert('L')
    Image.merge('RGB', (Image.new('L', rough.size, 255), rough, metal)).save(textures/'metallic_roughness.png')
    # glTF uses OpenGL normals; the old Unreal material used DirectX normals.
    r, g, b = Image.open(args.textures/(prefix+'normal-dx.png')).convert('RGB').split()
    Image.merge('RGB', (r, ImageOps.invert(g), b)).save(textures/'normal.png')
    meshes = sorted(args.source.glob('*.glb'))
    if not meshes: raise RuntimeError('no source GLBs found')
    for source in meshes:
        data = source.read_bytes()
        magic, version, size = struct.unpack_from('<III', data)
        if magic != 0x46546c67 or version != 2 or size != len(data): raise ValueError(source)
        length, kind = struct.unpack_from('<II', data, 12)
        if kind != 0x4e4f534a: raise ValueError(source)
        model = json.loads(data[20:20+length])
        offset = 20+length
        binary_length, binary_kind = struct.unpack_from('<II', data, offset)
        if binary_kind != 0x004e4942: raise ValueError(source)
        binary = bytearray(data[offset+8:offset+8+binary_length])
        model['images'] = [{'uri': '../textures/'+name+'.png'} for name in ('base_color', 'metallic_roughness', 'normal', 'occlusion')]
        model['textures'] = [{'source': i} for i in range(4)]
        model['materials'] = [{
            'name': 'powder-coated-metal',
            'pbrMetallicRoughness': {'baseColorTexture': {'index': 0}, 'metallicRoughnessTexture': {'index': 1}, 'metallicFactor': 1, 'roughnessFactor': 1},
            'normalTexture': {'index': 2}, 'occlusionTexture': {'index': 3},
        }]
        for mesh in model['meshes']:
            for primitive in mesh['primitives']:
                primitive['material'] = 0
                primitive['attributes'].pop('COLOR_0', None)
                if 'TEXCOORD_0' not in primitive['attributes']:
                    # Preserve the old override's UV0=(0,0) on meshes without UVs.
                    # A future authored UV unwrap can replace this accessor.
                    count = model['accessors'][primitive['attributes']['POSITION']]['count']
                    binary.extend(b'\0'*(-len(binary)%4))
                    start = len(binary)
                    binary.extend(b'\0'*(count*8))
                    views = model.setdefault('bufferViews', [])
                    views.append({'buffer': 0, 'byteOffset': start, 'byteLength': count*8, 'target': 34962})
                    accessors = model['accessors']
                    accessors.append({'bufferView': len(views)-1, 'componentType': 5126, 'count': count, 'type': 'VEC2'})
                    primitive['attributes']['TEXCOORD_0'] = len(accessors)-1
                    # Supply a finite orthonormal basis: automatic tangent generation
                    # divides by the UV determinant, which is zero for constant UVs.
                    normals = accessors[primitive['attributes']['NORMAL']]
                    if normals['componentType'] != 5126 or normals['type'] != 'VEC3':
                        raise ValueError('expected float normals: '+str(source))
                    view = views[normals['bufferView']]
                    offset = view.get('byteOffset', 0)+normals.get('byteOffset', 0)
                    stride = view.get('byteStride', 12)
                    start = len(binary)
                    for i in range(count):
                        x, y, z = struct.unpack_from('<3f', binary, offset+i*stride)
                        tx, ty, tz = (0, -z, y) if abs(x)<.9 else (-y, x, 0)
                        norm = math.sqrt(tx*tx+ty*ty+tz*tz)
                        if not math.isfinite(norm) or norm<1e-8: raise ValueError('invalid normal: '+str(source))
                        binary.extend(struct.pack('<4f', tx/norm, ty/norm, tz/norm, 1))
                    views.append({'buffer': 0, 'byteOffset': start, 'byteLength': count*16, 'target': 34962})
                    accessors.append({'bufferView': len(views)-1, 'componentType': 5126, 'count': count, 'type': 'VEC4'})
                    primitive['attributes']['TANGENT'] = len(accessors)-1
        model['buffers'][0]['byteLength'] = len(binary)
        payload = json.dumps(model, separators=(',', ':')).encode()
        payload += b' '*(-len(payload)%4)
        binary.extend(b'\0'*(-len(binary)%4))
        output = struct.pack('<III', magic, version, 28+len(payload)+len(binary))
        output += struct.pack('<II', len(payload), kind)+payload
        output += struct.pack('<II', len(binary), binary_kind)+binary
        (args.package/'meshes'/source.name).write_bytes(output)
    print(f'Restored powder-coated-metal PBR on {len(meshes)} external GLBs')


if __name__ == '__main__': main()
