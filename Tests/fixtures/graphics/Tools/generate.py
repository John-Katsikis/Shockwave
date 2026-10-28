"""Regenerate the graphics fixture's content (stdlib only, deterministic).

Everything here is self-authored and generated: no downloaded or commercial assets. Run it
after changing it; the outputs are committed so tests don't need Python.

  Content/textures/checker-fine.png  4-pixel black/white checker: aliasing and mip tests
  Content/textures/ramp.png          left-to-right 0..255 ramp: brightness and banding
  Content/models/panels.glb          blended and alpha-masked panels (double-sided)
  Content/models/skinned-strip.glb   a strip on two joints with a "bend" animation
  Scenes/dense-occluder.swscene      1200 small cubes behind a wall, plus a visible row
"""
import json, math, struct, zlib
from pathlib import Path

root = Path(__file__).resolve().parents[1]


def png(width, height, pixels):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    rows = b''.join(b'\0' + bytes(pixels[y * width * 4:(y + 1) * width * 4]) for y in range(height))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))


def checker(size=256, cell=4):
    pixels = []
    for y in range(size):
        for x in range(size):
            v = 255 if (x // cell + y // cell) % 2 == 0 else 0
            pixels += [v, v, v, 255]
    return png(size, size, pixels)


def ramp(width=256, height=16):
    pixels = []
    for y in range(height):
        for x in range(width):
            pixels += [x, x, x, 255]
    return png(width, height, pixels)


def cutout(size=64):
    """White with round holes on a grid (alpha 0), for alpha-tested edges."""
    pixels = []
    for y in range(size):
        for x in range(size):
            lx, ly = x % 16 - 7.5, y % 16 - 7.5
            hole = lx * lx + ly * ly < 5.5 * 5.5
            pixels += [235, 220, 160, 0 if hole else 255]
    return png(size, size, pixels)


class Glb:
    """A glTF binary built up one accessor at a time."""

    def __init__(self):
        self.blob = bytearray()
        self.views, self.accessors, self.images = [], [], []

    def align(self):
        while len(self.blob) % 4:
            self.blob.append(0)

    def add(self, data, fmt, kind, target=None, minmax=False, normalized=False):
        self.align()
        raw = struct.pack('<' + fmt * len(data), *data)
        view = {'buffer': 0, 'byteOffset': len(self.blob), 'byteLength': len(raw)}
        if target:
            view['target'] = target
        self.views.append(view)
        self.blob.extend(raw)
        components = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}[kind]
        component = {'f': 5126, 'I': 5125, 'H': 5123, 'B': 5121}[fmt]
        accessor = {'bufferView': len(self.views) - 1, 'componentType': component,
                    'count': len(data) // components, 'type': kind}
        if minmax:
            accessor['min'] = [min(data[i::components]) for i in range(components)]
            accessor['max'] = [max(data[i::components]) for i in range(components)]
        if normalized:
            accessor['normalized'] = True
        self.accessors.append(accessor)
        return len(self.accessors) - 1

    def image(self, data):
        self.align()
        self.views.append({'buffer': 0, 'byteOffset': len(self.blob), 'byteLength': len(data)})
        self.blob.extend(data)
        self.images.append({'bufferView': len(self.views) - 1, 'mimeType': 'image/png'})
        return len(self.images) - 1

    def write(self, path, doc):
        self.align()
        doc.update({'asset': {'version': '2.0', 'generator': 'Shockwave graphics fixtures (Tools/generate.py)'},
                    'buffers': [{'byteLength': len(self.blob)}], 'bufferViews': self.views,
                    'accessors': self.accessors})
        if self.images:
            doc['images'] = self.images
        encoded = json.dumps(doc, separators=(',', ':'), sort_keys=True).encode()
        encoded += b' ' * ((-len(encoded)) % 4)
        glb = (struct.pack('<III', 0x46546c67, 2, 28 + len(encoded) + len(self.blob)) +
               struct.pack('<I4s', len(encoded), b'JSON') + encoded +
               struct.pack('<I4s', len(self.blob), b'BIN\0') + bytes(self.blob))
        path.write_bytes(glb)


def quad(glb, width, height):
    w, h = width / 2, height / 2
    p = [-w, -h, 0, w, -h, 0, w, h, 0, -w, h, 0]
    n = [0, 0, 1] * 4
    uv = [0, 1, 1, 1, 1, 0, 0, 0]
    return {'attributes': {'POSITION': glb.add(p, 'f', 'VEC3', 34962, minmax=True),
                           'NORMAL': glb.add(n, 'f', 'VEC3', 34962),
                           'TEXCOORD_0': glb.add(uv, 'f', 'VEC2', 34962)},
            'indices': glb.add([0, 1, 2, 0, 2, 3], 'I', 'SCALAR', 34963)}


def panels():
    glb = Glb()
    shape = quad(glb, 1.6, 1.6)
    hole = glb.image(cutout())
    materials = [
        {'name': 'Blue glass', 'alphaMode': 'BLEND', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [0.2, 0.55, 1.0, 0.4], 'metallicFactor': 0, 'roughnessFactor': 0.1}},
        {'name': 'Red glass', 'alphaMode': 'BLEND', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [1.0, 0.25, 0.1, 0.5], 'metallicFactor': 0, 'roughnessFactor': 0.1}},
        {'name': 'Cutout', 'alphaMode': 'MASK', 'alphaCutoff': 0.5, 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorTexture': {'index': 0}, 'metallicFactor': 0, 'roughnessFactor': 0.7}},
    ]
    doc = {
        'samplers': [{'magFilter': 9729, 'minFilter': 9987}],
        'textures': [{'source': hole, 'sampler': 0}],
        'materials': materials,
        'meshes': [{'name': m['name'], 'primitives': [dict(shape, material=i)]} for i, m in enumerate(materials)],
        'nodes': [{'name': 'Panels', 'children': [1, 2, 3]},
                  {'name': 'Blue glass', 'mesh': 0, 'translation': [-0.5, 0.8, 0.3]},
                  {'name': 'Red glass', 'mesh': 1, 'translation': [0.3, 0.9, -0.3]},
                  {'name': 'Cutout', 'mesh': 2, 'translation': [2.0, 0.8, 0.0]}],
        'scenes': [{'nodes': [0]}], 'scene': 0,
    }
    glb.write(root / 'Content/models/panels.glb', doc)


def skinned_strip(segments=12, width=0.4, height=3.0, hinge=1.5):
    glb = Glb()
    p, n, uv, joints, weights, idx = [], [], [], [], [], []
    for s in range(segments + 1):
        y = height * s / segments
        # The tip joint takes over smoothly across the middle third.
        t = min(max((y - 1.0) / 1.0, 0.0), 1.0)
        w = t * t * (3 - 2 * t)
        for x in (-width / 2, width / 2):
            p += [x, y, 0]
            n += [0, 0, 1]
            uv += [0 if x < 0 else 1, 1 - s / segments]
            joints += [0, 1, 0, 0]
            weights += [1 - w, w, 0, 0]
    for s in range(segments):
        a = s * 2
        idx += [a, a + 1, a + 3, a, a + 3, a + 2]
    ibm = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1,
           1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -hinge, 0, 1]
    times = [0.0, 1.0, 2.0]
    angle = math.radians(70) / 2
    rotations = [0, 0, 0, 1, 0, 0, math.sin(angle), math.cos(angle), 0, 0, 0, 1]
    primitive = {'attributes': {'POSITION': glb.add(p, 'f', 'VEC3', 34962, minmax=True),
                                'NORMAL': glb.add(n, 'f', 'VEC3', 34962),
                                'TEXCOORD_0': glb.add(uv, 'f', 'VEC2', 34962),
                                'JOINTS_0': glb.add(joints, 'H', 'VEC4', 34962),
                                'WEIGHTS_0': glb.add(weights, 'f', 'VEC4', 34962)},
                 'indices': glb.add(idx, 'I', 'SCALAR', 34963), 'material': 0}
    doc = {
        'materials': [{'name': 'Strip', 'doubleSided': True,
                       'pbrMetallicRoughness': {'baseColorFactor': [0.9, 0.6, 0.2, 1], 'metallicFactor': 0,
                                                'roughnessFactor': 0.5}}],
        'meshes': [{'name': 'Strip', 'primitives': [primitive]}],
        'skins': [{'joints': [1, 2], 'skeleton': 1, 'inverseBindMatrices': glb.add(ibm, 'f', 'MAT4')}],
        'nodes': [{'name': 'Strip', 'mesh': 0, 'skin': 0},
                  {'name': 'Root', 'children': [2]},
                  {'name': 'Tip', 'translation': [0, hinge, 0]}],
        'animations': [{'name': 'bend',
                        'samplers': [{'input': glb.add(times, 'f', 'SCALAR', minmax=True),
                                      'output': glb.add(rotations, 'f', 'VEC4'), 'interpolation': 'LINEAR'}],
                        'channels': [{'sampler': 0, 'target': {'node': 2, 'path': 'rotation'}}]}],
        'scenes': [{'nodes': [0, 1]}], 'scene': 0,
    }
    glb.write(root / 'Content/models/skinned-strip.glb', doc)


def dense_scene():
    lines = [
        '# Generated by Tools/generate.py; edit that, not this file.',
        '# Dense repeated geometry: 1200 small cubes in a block behind a wall, which hides',
        '# almost all of them from the main camera, and a row in front that stays visible.',
        '# Occlusion culling (G12) must never lose the visible row; the overhead camera sees',
        '# everything and checks nothing went missing.',
        'shockwave_scene 2',
        '',
        'entity "Environment"', '  Environment', '    sky_color 0.35 0.42 0.55 1', '    ground_color 0.2 0.18 0.15 1',
        '    ambient_intensity 0.6', 'end', '',
        'entity "Sun"', '  Transform', '    rotation -50 30 0', '  Light', '    type Directional',
        '    color 1 0.95 0.88 1', '    intensity 3', 'end', '',
        'entity "Ground"', '  Transform', '    scale 60 1 60', '  MeshRenderer', '    mesh floor', '    texture grid',
        '    tint 0.6 0.6 0.6 1', 'end', '',
        'entity "Occluder"', '  Transform', '    position 0 3 -4', '    scale 24 6 0.4', '  MeshRenderer', '    mesh cube',
        '    tint 0.75 0.72 0.68 1', 'end', '',
        'entity "Main Camera"', '  Transform', '    position 0 1.7 6', '    rotation -4 0 0', '  Camera', '    fov 60',
        '    primary 1', 'end', '',
        'entity "Overhead"', '  Transform', '    position 0 40 -14', '    rotation -80 0 0', '  Camera', '    fov 60',
        '    primary 0', 'end', '',
    ]
    palette = [(0.8, 0.3, 0.25), (0.3, 0.7, 0.35), (0.3, 0.45, 0.85), (0.85, 0.75, 0.3)]
    n = 0
    for layer in range(3):
        for row in range(20):
            for col in range(20):
                x, y, z = -9.5 + col, 0.3 + layer * 0.9, -6 - row
                c = palette[(row + col + layer) % 4]
                lines += [f'entity "Hidden {n}"', '  Transform', f'    position {x:g} {y:g} {z:g}',
                          '    scale 0.6 0.6 0.6', '  MeshRenderer', '    mesh cube',
                          f'    tint {c[0]:g} {c[1]:g} {c[2]:g} 1', 'end', '']
                n += 1
    for col in range(9):
        x = -4 + col
        lines += [f'entity "Visible {col}"', '  Transform', f'    position {x:g} 0.4 0', '    scale 0.5 0.8 0.5',
                  '  MeshRenderer', '    mesh cube', '    tint 0.9 0.9 0.9 1', 'end', '']
    (root / 'Scenes/dense-occluder.swscene').write_text('\n'.join(lines))


(root / 'Content/textures/checker-fine.png').write_bytes(checker())
(root / 'Content/textures/ramp.png').write_bytes(ramp())
panels()
skinned_strip()
dense_scene()
