#!/usr/bin/env python3
"""Inspect RenderLab glTF buffers as text, including every vertex/index with --dump."""
import argparse
import json
from pathlib import Path
import struct


def inspect(path, dump=False):
    doc = json.loads(path.read_text())
    buffers = [(path.parent / item['uri']).read_bytes() for item in doc['buffers']]
    result = {'asset':doc['asset'],'nodes':doc['nodes'],'materials':doc['materials'], 'meshes':[]}
    formats = {5126:'f',5125:'I',5123:'H',5121:'B'}
    widths = {'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4,'MAT4':16}
    def accessor(index):
        a = doc['accessors'][index]
        view = doc['bufferViews'][a['bufferView']]
        fmt = '<' + formats[a['componentType']]*widths[a['type']]
        stride = view.get('byteStride',struct.calcsize(fmt))
        offset = view.get('byteOffset',0)+a.get('byteOffset',0)
        data = buffers[view['buffer']]
        return [struct.unpack_from(fmt,data,offset+i*stride) for i in range(a['count'])]
    for mesh in doc['meshes']:
        primitives = []
        for p in mesh['primitives']:
            item = {'vertices':doc['accessors'][p['attributes']['POSITION']]['count'],
                    'triangles':doc['accessors'][p['indices']]['count']//3,'material':p['material']}
            if dump:
                item['attributes'] = {name:accessor(index) for name,index in p['attributes'].items()}
                item['indices'] = [row[0] for row in accessor(p['indices'])]
            primitives.append(item)
        result['meshes'].append(primitives)
    return result

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('model',type=Path)
    parser.add_argument('--dump',action='store_true')
    parser.add_argument('--output',type=Path)
    args = parser.parse_args()
    text = json.dumps(inspect(args.model,args.dump),indent=2)+'\n'
    if args.output: args.output.write_text(text)
    else: print(text,end='')
