"""Regenerate the foliage fixtures (W2a; stdlib only, deterministic).

  Content/models/grass-tuft.glb       three crossed, tapered blades 0.8 m tall (double-sided)
  Content/models/pebble.glb           a squat 0.3 m box
  Content/foliage/meadow.instances    20,000 instances over 120 m by 120 m: 18,000 tufts and
                                      2,000 pebbles, placed by a fixed hash
  Content/foliage/meadow.swfoliage    the tufts sway (wind 0.15) and cast no shadows
  Content/foliage/meadow-still.swfoliage  the same instances with wind 0
  Scenes/foliage-meadow*.swscene      the meadow from a low camera; -still with wind 0, -none
                                      without the Foliage component
  Broken/type-7.instances             one instance of type 7, for FoliageCheck.cmake's --check
                                      (outside Content, so the project never loads it)
Run it after changing it; the outputs are committed so tests don't need Python.
"""
import json, math, struct
from pathlib import Path

root = Path(__file__).resolve().parents[1]


class Glb:
    def __init__(self):
        self.blob = bytearray()
        self.views = []
        self.accessors = []

    def add(self, data, fmt, kind, target=None, minmax=False):
        while len(self.blob) % 4:
            self.blob.append(0)
        raw = struct.pack('<' + fmt * len(data), *data)
        view = {'buffer': 0, 'byteOffset': len(self.blob), 'byteLength': len(raw)}
        if target:
            view['target'] = target
        self.views.append(view)
        self.blob.extend(raw)
        components = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3}[kind]
        accessor = {'bufferView': len(self.views) - 1, 'componentType': {'f': 5126, 'I': 5125}[fmt],
                    'count': len(data) // components, 'type': kind}
        if minmax:
            accessor['min'] = [min(data[i::components]) for i in range(components)]
            accessor['max'] = [max(data[i::components]) for i in range(components)]
        self.accessors.append(accessor)
        return len(self.accessors) - 1

    def write(self, path, doc):
        while len(self.blob) % 4:
            self.blob.append(0)
        doc.update({'asset': {'version': '2.0', 'generator': 'Shockwave graphics fixtures (Tools/foliage.py)'},
                    'buffers': [{'byteLength': len(self.blob)}], 'bufferViews': self.views,
                    'accessors': self.accessors})
        encoded = json.dumps(doc, separators=(',', ':'), sort_keys=True).encode()
        encoded += b' ' * ((-len(encoded)) % 4)
        path.write_bytes(struct.pack('<III', 0x46546c67, 2, 28 + len(encoded) + len(self.blob)) +
                         struct.pack('<I4s', len(encoded), b'JSON') + encoded +
                         struct.pack('<I4s', len(self.blob), b'BIN\0') + bytes(self.blob))


def model(path, name, positions, normals, indices, color):
    glb = Glb()
    primitive = {'attributes': {'POSITION': glb.add(positions, 'f', 'VEC3', 34962, minmax=True),
                                'NORMAL': glb.add(normals, 'f', 'VEC3', 34962)},
                 'indices': glb.add(indices, 'I', 'SCALAR', 34963), 'material': 0}
    doc = {'materials': [{'name': name, 'doubleSided': True,
                          'pbrMetallicRoughness': {'baseColorFactor': color, 'metallicFactor': 0,
                                                   'roughnessFactor': 0.8}}],
           'meshes': [{'name': name, 'primitives': [primitive]}],
           'nodes': [{'name': name, 'mesh': 0}], 'scenes': [{'nodes': [0]}], 'scene': 0}
    glb.write(path, doc)


def tuft(height=0.8, width=0.18, blades=3):
    p, n, idx = [], [], []
    for b in range(blades):
        a = math.pi * b / blades
        dx, dz = math.cos(a) * width / 2, math.sin(a) * width / 2
        nx, nz = -math.sin(a), math.cos(a)
        base = len(p) // 3
        p += [-dx, 0, -dz, dx, 0, dz, 0, height, 0]
        n += [nx, 0, nz] * 3
        idx += [base, base + 1, base + 2]
    return p, n, idx


def pebble(size=0.3, height=0.15):
    s, h = size / 2, height
    p, n, idx = [], [], []
    faces = [((0, 1, 0), [(-s, h, -s), (-s, h, s), (s, h, s), (s, h, -s)]),
             ((1, 0, 0), [(s, 0, -s), (s, h, -s), (s, h, s), (s, 0, s)]),
             ((-1, 0, 0), [(-s, 0, s), (-s, h, s), (-s, h, -s), (-s, 0, -s)]),
             ((0, 0, 1), [(s, 0, s), (s, h, s), (-s, h, s), (-s, 0, s)]),
             ((0, 0, -1), [(-s, 0, -s), (-s, h, -s), (s, h, -s), (s, 0, -s)])]
    for normal, corners in faces:
        base = len(p) // 3
        for c in corners:
            p += list(c)
            n += list(normal)
        idx += [base, base + 1, base + 2, base, base + 2, base + 3]
    return p, n, idx


def lowbias(x):
    x = (x + 0x9e3779b9) & 0xffffffff
    x ^= x >> 16
    x = (x * 0x7feb352d) & 0xffffffff
    x ^= x >> 15
    x = (x * 0x846ca68b) & 0xffffffff
    x ^= x >> 16
    return (x >> 8) / float(1 << 24)


def instances(count=20000, pebbles=2000, size=120.0):
    out = bytearray()
    for i in range(count):
        kind = 1 if i % (count // pebbles) == 0 else 0
        x = (lowbias(i * 3) - 0.5) * size
        z = (lowbias(i * 3 + 1) - 0.5) * size
        yaw = lowbias(i * 3 + 2) * 2 * math.pi
        scale = 0.7 + 0.6 * lowbias(i * 7 + 5)
        out += struct.pack('<HHfffff', kind, 0, x, 0.0, z, yaw, scale)
    return bytes(out)


def header(wind):
    return ('swfoliage 1\n'
            'instances "foliage/meadow.instances"\n'
            'type "models/grass-tuft.glb"\n'
            '  distance 70\n'
            '  fade 20\n'
            '  shadows false\n'
            f'  wind {wind}\n'
            'type "models/pebble.glb"\n'
            '  distance 50\n'
            '  fade 10\n'
            '  shadows true\n'
            '  wind 0\n')


def scene(name, note, foliage):
    lines = ['# ' + line for line in note] + ['shockwave_scene 2', '',
             'entity "Environment"', '  Environment', '    sky_color 0.45 0.55 0.7 1',
             '    ground_color 0.2 0.18 0.15 1', '    ambient_intensity 0.5', 'end', '',
             'entity "Sun"', '  Transform', '    rotation -40 30 0', '  Light', '    type Directional',
             '    color 1 0.95 0.85 1', '    intensity 2', 'end', '',
             'entity "Ground"', '  Transform', '    scale 140 1 140', '  MeshRenderer', '    mesh floor',
             '    tint 0.35 0.3 0.22 1', 'end', '']
    if foliage:
        lines += ['entity "Meadow"', '  Transform', '  Foliage', f'    asset {foliage}', 'end', '']
    lines += ['entity "Main Camera"', '  Transform', '    position 0 1.7 30', '    rotation -12 0 0', '  Camera',
              '    fov 60', '    near_plane 0.1', '    primary 1', 'end', '']
    (root / f'Scenes/{name}.swscene').write_text('\n'.join(lines))


models = root / 'Content/models'
model(models / 'grass-tuft.glb', 'Grass', *tuft(), [0.3, 0.6, 0.15, 1])
model(models / 'pebble.glb', 'Pebble', *pebble(), [0.5, 0.5, 0.48, 1])
foliage = root / 'Content/foliage'
foliage.mkdir(parents=True, exist_ok=True)
(foliage / 'meadow.instances').write_bytes(instances())
(foliage / 'meadow.swfoliage').write_text(header(0.15))
(foliage / 'meadow-still.swfoliage').write_text(header(0))
broken = root / 'Broken'
broken.mkdir(exist_ok=True)
(broken / 'type-7.instances').write_bytes(struct.pack('<HHfffff', 7, 0, 0.0, 0.0, 0.0, 0.0, 1.0))
scene('foliage-meadow', ['Foliage (W2a): 20,000 tufts and pebbles on a 120 m meadow, seen from a low camera.',
                         'Written by Tools/foliage.py. Camera: "Main Camera".'], 'foliage/meadow.swfoliage')
scene('foliage-meadow-still', ['The meadow with wind 0 (Tools/foliage.py).'], 'foliage/meadow-still.swfoliage')
scene('foliage-meadow-none', ['The meadow without its Foliage component (Tools/foliage.py).'], None)
