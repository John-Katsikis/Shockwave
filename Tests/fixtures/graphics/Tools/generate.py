"""Regenerate the graphics fixture's content (stdlib only, deterministic).

Everything here is self-authored and generated: no downloaded or commercial assets. Run it
after changing it; the outputs are committed so tests don't need Python.

  Content/textures/checker-fine.png  4-pixel black/white checker: aliasing and mip tests
  Content/textures/ramp.png          left-to-right 0..255 ramp: brightness and banding
  Content/models/panels.glb          blended and alpha-masked panels (double-sided)
  Content/models/skinned-strip.glb   a strip on two joints with a "bend" animation
  Content/models/materials.glb       material slot bindings (G02): KHR_texture_transform,
                                     TEXCOORD_1, wrap and filter modes, transformed normal maps
  Content/models/materials-expected.glb  the same quads with every transform baked into
                                     plain UV0 and the default sampler: must look identical
  Content/models/missing-uv1.glb     a material reading TEXCOORD_1 on a mesh without it
  Scenes/materials.swscene, Scenes/materials-expected.swscene  one of each, same framing
  Scenes/dense-occluder.swscene      1200 small cubes behind a wall, plus a visible row
  Content/textures/ridges-normal.png, leaf.png  a tiling detail normal map and an alpha mask
  Content/materials/instances/*.swmaterial  an instance of each materials.glb material
                                     that changes nothing (A02)
  Content/materials/*.swmaterial     detail, second layer and masked foliage materials
  Scenes/material-instances.swscene  materials.swscene with every part drawn through its
                                     instance: must match it exactly
  Scenes/material-layers.swscene     the detail, layer and foliage materials on cubes and quads
  Scenes/ambient-occlusion*.swscene  a corner, a box, a thin pole and a glowing block (G04),
                                     as authored, with ao_intensity 0 and with no ambient light
  Scenes/many-lights.swscene         200 point and 4 spot lights down a long floor (G11)
  Scenes/light-overflow.swscene      96 overlapping lights: overflowing cluster lists (G11)
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


# ---- Material bindings (G02) ----
# Every quad of materials.glb has a twin in materials-expected.glb whose UV0 already holds
# what the binding computes (offset + R(rotation) * (scale * uv), R = [[c, s], [-s, c]] as
# the Khronos sample renderer and three.js apply it), so the two scenes must render the
# same image in the unlit, normals and lit views.

def arrow(size=32):
    """A gradient with a white arrow pointing up the texture and a red top-left corner:
    any flip, rotation or offset error shows."""
    pixels = []
    for y in range(size):
        for x in range(size):
            r, g, b = x * 255 // (size - 1), y * 255 // (size - 1), 60
            shaft = 14 <= x <= 17 and 9 <= y <= 27
            head = 3 <= y <= 10 and abs(x - 15.5) <= (y - 3) * 0.9
            if shaft or head:
                r, g, b = 250, 250, 250
            if x < 6 and y < 6:
                r, g, b = 255, 20, 20
            pixels += [r, g, b, 255]
    return png(size, size, pixels)


def bumps(size=32):
    """Tangent-space normals (green up the texture): a dome top-left, a slope tilting +u
    on the right half, flat elsewhere."""
    pixels = []
    for y in range(size):
        for x in range(size):
            dx = dy = 0.0
            cx, cy = x - 9.5, y - 9.5
            if cx * cx + cy * cy < 64:
                dx, dy = cx / 10.0, -cy / 10.0   # dy: towards smaller v
            elif x >= size // 2:
                dx = 0.6
            length = math.sqrt(dx * dx + dy * dy + 1)
            n = (dx / length, dy / length, 1 / length)
            pixels += [round((c * 0.5 + 0.5) * 255) for c in n] + [255]
    return png(size, size, pixels)


def leaf(size=32):
    """An alpha mask: a disc and a triangle, opaque; the rest cut out."""
    pixels = []
    for y in range(size):
        for x in range(size):
            disc = (x - 10) ** 2 + (y - 10) ** 2 < 64
            tri = x > 16 and y > 16 and x + y > 44
            pixels += [60, 170, 70, 255 if disc or tri else 0]
    return png(size, size, pixels)


TILE = 16


def tile_texel(x, y):
    # Gentle steps between neighbours (nearest sampling may round a texel boundary a pixel
    # either way), and a fine checker in blue that linear filtering would blur away.
    return [40 + x * 10, 40 + y * 10, 100 + (x + y) % 2 * 60, 255]


def tile():
    return png(TILE, TILE, [c for y in range(TILE) for x in range(TILE) for c in tile_texel(x, y)])


def tile_expected():
    """What a clamp-U, mirror-V sampler shows across uv -1..2 of tile(), as one image
    sampled with plain repeat."""
    def clamp(j):
        return min(max(j, 0), TILE - 1)

    def mirror(j):
        j %= 2 * TILE
        return j if j < TILE else 2 * TILE - 1 - j
    size = 3 * TILE
    return png(size, size, [c for y in range(size) for x in range(size)
                            for c in tile_texel(clamp(x - TILE), mirror(y - TILE))])


def transform_uv(uv, t):
    if not t:
        return list(uv)
    c, s = math.cos(t.get('rotation', 0)), math.sin(t.get('rotation', 0))
    sx, sy = t.get('scale', [1, 1])
    ox, oy = t.get('offset', [0, 0])
    out = []
    for i in range(0, len(uv), 2):
        u, v = uv[i] * sx, uv[i + 1] * sy
        out += [ox + c * u + s * v, oy - s * u + c * v]
    return out


QUAD_UV = [0, 1, 1, 1, 1, 0, 0, 0]
TURNED_UV = [1, 1, 1, 0, 0, 0, 0, 1]   # the same square turned a quarter
FLAT_UV = [0.5, 0.5] * 4               # all one point: wrong if anything reads it


def material_quads():
    """(name, material, uv0, uv1, expected material, expected uv0, expected uv1)."""
    t0 = {'offset': [0.3, -0.2], 'scale': [1.5, -0.8], 'rotation': 0.7}
    tn = {'rotation': 1.1, 'scale': [-1, 1]}
    tm = {'offset': [0.1, 0.0], 'scale': [2, 2], 'rotation': 0.5}
    te = {'offset': [0.2, 0.1], 'scale': [1.25, 1.25], 'rotation': -0.4}
    tc = {'offset': [-1, -1], 'scale': [3, 3]}
    white = {'baseColorFactor': [1, 1, 1, 1], 'metallicFactor': 0, 'roughnessFactor': 0.6}

    def pbr(**extra):
        return dict(white, **extra)

    def tex(index, tt=None, tex_coord=None, **extra):
        info = dict({'index': index}, **extra)
        if tex_coord is not None:
            info['texCoord'] = tex_coord
        if tt is not None:
            info['extensions'] = {'KHR_texture_transform': tt}
        return info
    ARROW, BUMPS, LEAF, TILEX, ARROW_LINEAR, TILE_BIG = 0, 1, 2, 3, 4, 5
    return [
        ('Transform', {'pbrMetallicRoughness': pbr(baseColorTexture=tex(ARROW, t0))}, QUAD_UV, None,
         {'pbrMetallicRoughness': pbr(baseColorTexture=tex(ARROW))}, transform_uv(QUAD_UV, t0), None),
        ('UV1', {'pbrMetallicRoughness': pbr(baseColorTexture=tex(ARROW, tex_coord=1))}, FLAT_UV, TURNED_UV,
         {'pbrMetallicRoughness': pbr(baseColorTexture=tex(ARROW))}, TURNED_UV, None),
        ('KHR texCoord', {'pbrMetallicRoughness': pbr(baseColorTexture=tex(ARROW, {'texCoord': 1, 'offset': [0.25, 0]}))},
         FLAT_UV, TURNED_UV, {'pbrMetallicRoughness': pbr(baseColorTexture=tex(ARROW))},
         transform_uv(TURNED_UV, {'offset': [0.25, 0]}), None),
        ('Normal transformed', {'pbrMetallicRoughness': white, 'normalTexture': tex(BUMPS, tn)}, QUAD_UV, None,
         {'pbrMetallicRoughness': white, 'normalTexture': tex(BUMPS)}, transform_uv(QUAD_UV, tn), None),
        ('Normal UV1', {'pbrMetallicRoughness': white, 'normalTexture': tex(BUMPS, tex_coord=1)}, QUAD_UV, TURNED_UV,
         {'pbrMetallicRoughness': white, 'normalTexture': tex(BUMPS)}, TURNED_UV, None),
        ('Masked', {'alphaMode': 'MASK', 'alphaCutoff': 0.5, 'doubleSided': True,
                    'pbrMetallicRoughness': pbr(baseColorTexture=tex(LEAF, tm))}, QUAD_UV, None,
         {'alphaMode': 'MASK', 'alphaCutoff': 0.5, 'doubleSided': True,
          'pbrMetallicRoughness': pbr(baseColorTexture=tex(LEAF))}, transform_uv(QUAD_UV, tm), None),
        ('Clamp mirror nearest', {'pbrMetallicRoughness': pbr(baseColorTexture=tex(TILEX, tc))}, QUAD_UV, None,
         {'pbrMetallicRoughness': pbr(baseColorTexture=tex(TILE_BIG))}, QUAD_UV, None),
        ('Per-slot UV sets', {'pbrMetallicRoughness': pbr(baseColorTexture=tex(ARROW_LINEAR)),
                              'emissiveTexture': tex(ARROW_LINEAR, tex_coord=1), 'emissiveFactor': [0.3, 0.3, 0.3]},
         QUAD_UV, TURNED_UV, None, None, None),
        ('Emissive occlusion', {'pbrMetallicRoughness': pbr(baseColorFactor=[0.2, 0.2, 0.2, 1]),
                                'emissiveTexture': tex(ARROW, te), 'emissiveFactor': [0.6, 0.6, 0.6],
                                'occlusionTexture': tex(ARROW, te)}, QUAD_UV, None,
         {'pbrMetallicRoughness': pbr(baseColorFactor=[0.2, 0.2, 0.2, 1]), 'emissiveTexture': tex(ARROW),
          'emissiveFactor': [0.6, 0.6, 0.6], 'occlusionTexture': tex(ARROW)}, transform_uv(QUAD_UV, te), None),
    ]


def materials_models():
    for expected in (False, True):
        glb = Glb()
        images = [glb.image(arrow()), glb.image(bumps()), glb.image(leaf()), glb.image(tile()), glb.image(tile_expected())]
        nearest = {'magFilter': 9728, 'minFilter': 9728}
        samplers = [{'magFilter': 9729, 'minFilter': 9987},
                    dict(nearest, wrapS=33071, wrapT=33648),   # clamp U, mirror V
                    dict(nearest)]                            # repeat, for the expected tile
        textures = [{'source': 0, 'sampler': 0}, {'source': 1, 'sampler': 0}, {'source': 2, 'sampler': 0},
                    {'source': 3, 'sampler': 1}, {'source': 0}, {'source': 4, 'sampler': 2}]
        materials, meshes, nodes = [], [], [{'name': 'Quads', 'children': []}]
        p = [-0.5, -0.5, 0, 0.5, -0.5, 0, 0.5, 0.5, 0, -0.5, 0.5, 0]
        position, normal = glb.add(p, 'f', 'VEC3', 34962, minmax=True), glb.add([0, 0, 1] * 4, 'f', 'VEC3', 34962)
        indices = glb.add([0, 1, 2, 0, 2, 3], 'I', 'SCALAR', 34963)
        for i, (name, material, uv0, uv1, ex_material, ex_uv0, ex_uv1) in enumerate(material_quads()):
            if expected and ex_material is not None:
                material, uv0, uv1 = ex_material, ex_uv0, ex_uv1
            materials.append(dict(material, name=name))
            attributes = {'POSITION': position, 'NORMAL': normal, 'TEXCOORD_0': glb.add(uv0, 'f', 'VEC2', 34962)}
            if uv1 is not None:
                attributes['TEXCOORD_1'] = glb.add(uv1, 'f', 'VEC2', 34962)
            meshes.append({'name': name, 'primitives': [{'attributes': attributes, 'indices': indices, 'material': i}]})
            nodes[0]['children'].append(len(nodes))
            nodes.append({'name': name, 'mesh': i, 'translation': [(i % 3 - 1) * 1.2, (1 - i // 3) * 1.2, 0]})
        doc = {'extensionsUsed': ['KHR_texture_transform'], 'samplers': samplers, 'textures': textures,
               'materials': materials, 'meshes': meshes, 'nodes': nodes, 'scenes': [{'nodes': [0]}], 'scene': 0}
        name = 'materials-expected.glb' if expected else 'materials.glb'
        glb.write(root / 'Content/models' / name, doc)


def missing_uv1():
    """A material asking for TEXCOORD_1 on a mesh that has none: the importer warns, and
    --check flags any scene using it. No fixture scene does."""
    glb = Glb()
    image = glb.image(arrow())
    shape = quad(glb, 1, 1)
    doc = {'textures': [{'source': image}],
           'materials': [{'name': 'Missing UV1', 'pbrMetallicRoughness': {'baseColorTexture': {'index': 0, 'texCoord': 1}}}],
           'meshes': [{'name': 'Missing UV1', 'primitives': [dict(shape, material=0)]}],
           'nodes': [{'name': 'Missing UV1', 'mesh': 0}], 'scenes': [{'nodes': [0]}], 'scene': 0}
    glb.write(root / 'Content/models/missing-uv1.glb', doc)


def materials_scene(expected):
    model = 'models/materials-expected.glb' if expected else 'models/materials.glb'
    lines = [
        '# Generated by Tools/generate.py; edit that, not this file.',
        '# Material slot bindings (G02): nine quads, each testing one binding. materials.swscene',
        '# reads them through KHR_texture_transform, TEXCOORD_1 and sampler modes;',
        '# materials-expected.swscene draws the same quads with everything baked into UV0.',
        '# Rendered in the unlit, normals and lit views the two must match (GraphicsMaterialsCheck).',
        '# Camera: "Main Camera".',
        'shockwave_scene 2', '',
        'entity "Environment"', '  Environment', '    sky_color 0.35 0.42 0.55 1', '    ground_color 0.2 0.18 0.15 1',
        '    ambient_intensity 0.6', 'end', '',
        'entity "Sun"', '  Transform', '    rotation -25 20 0', '  Light', '    type Directional',
        '    color 1 0.95 0.88 1', '    intensity 3', 'end', '',
        'entity "Backdrop"', '  Transform', '    position 0 0 -0.6', '    scale 5 5 0.2', '  MeshRenderer',
        '    mesh cube', '    tint 0.7 0.7 0.7 1', 'end', '',
        'entity "Quads"', '  Transform', '    position 0 0 0', '  ModelRenderer', f'    model {model}', 'end', '',
        'entity "Main Camera"', '  Transform', '    position 0 0 4.4', '  Camera', '    fov 50', '    primary 1',
        'end', '',
    ]
    name = 'materials-expected.swscene' if expected else 'materials.swscene'
    (root / 'Scenes' / name).write_text('\n'.join(lines))


def ridges_normal(size=64, waves=4):
    """Tangent-space normals of diagonal sine ridges: tiles seamlessly, fine detail."""
    pixels = []
    for y in range(size):
        for x in range(size):
            slope = 0.7 * math.cos(2 * math.pi * waves * (x + y) / size)
            length = math.sqrt(2 * slope * slope + 1)
            n = (slope / length, -slope / length, 1 / length)
            pixels += [round((c * 0.5 + 0.5) * 255) for c in n] + [255]
    return png(size, size, pixels)


def slug(name):
    return ''.join(c if c.isalnum() else '-' for c in name.lower()).strip('-')


def material_files():
    """Instances of every materials.glb material that change nothing, and the A02 materials."""
    folder = root / 'Content/materials'
    (folder / 'instances').mkdir(parents=True, exist_ok=True)
    header = '# Generated by Tools/generate.py; edit that, not this file.\nswmaterial 1\n'
    for name, *_ in material_quads():
        (folder / 'instances' / f'{slug(name)}.swmaterial').write_text(
            header + f'parent "models/materials.glb" "{name}"\n')
    (folder / 'stone.swmaterial').write_text(
        header + 'base_color 0.3 0.27 0.24 1\nroughness 0.7\n')
    # The detail layer: fine ridges tiled eight times over the stone, a smoother sheen in them.
    (folder / 'stone-detail.swmaterial').write_text(
        header + 'parent "materials/stone.swmaterial"\ndetail_normal_scale 1.5\ndetail_roughness 0.35\n'
        'detail_roughness_strength 0.5\ntexture detail_normal "textures/ridges-normal.png"\n'
        'uv_transform detail_normal 0 0 8 8 0\n')
    # A second layer: moss over the stone, more of it to the right (the ramp's red channel).
    (folder / 'stone-moss.swmaterial').write_text(
        header + 'parent "materials/stone.swmaterial"\nlayer_mask texture_r\nlayer_base_color 0.12 0.32 0.08 1\n'
        'layer_roughness 0.95\ntexture layer_mask "textures/ramp.png"\n')
    # Masked foliage with coverage-keeping mipmaps: the leaves stay as full far away.
    (folder / 'leaves.swmaterial').write_text(
        header + 'alpha_mode mask\nalpha_cutoff 0.5\ndouble_sided true\nroughness 0.8\n'
        'texture base_color "textures/leaf.png"\nuv_transform base_color 0 0 6 6 0\n')


def material_instances_scene():
    text = (root / 'Scenes/materials.swscene').read_text()
    overrides = ' '.join(f'"{name}" "materials/instances/{slug(name)}.swmaterial"' for name, *_ in material_quads())
    text = text[text.index('shockwave_scene 2'):]
    text = ('# Generated by Tools/generate.py; edit that, not this file.\n'
            '# Material instances (A02): materials.swscene with every part drawn through a\n'
            '# .swmaterial instance that changes nothing. In every view it must match\n'
            '# materials.swscene exactly (GraphicsFixturesCheck).\n'
            '# Camera: "Main Camera".\n') + text
    text = text.replace('    model models/materials.glb\n', f'    model models/materials.glb\n    materials {overrides}\n')
    (root / 'Scenes/material-instances.swscene').write_text(text)


def material_layers_scene():
    lines = [
        '# Generated by Tools/generate.py; edit that, not this file.',
        '# Detail and second layers (A02): plain stone, stone with a detail normal layer, stone',
        '# with moss as a second layer, and masked foliage near and far (coverage-keeping',
        '# mipmaps keep the far one as full). material_quality low drops only the detail.',
        '# Camera: "Main Camera".',
        'shockwave_scene 2', '',
        'entity "Environment"', '  Environment', '    sky_color 0.35 0.42 0.55 1', '    ground_color 0.2 0.18 0.15 1',
        '    ambient_intensity 0.6', 'end', '',
        'entity "Sun"', '  Transform', '    rotation -35 30 0', '  Light', '    type Directional',
        '    color 1 0.95 0.88 1', '    intensity 3', 'end', '',
        'entity "Floor"', '  Transform', '    scale 30 1 30', '  MeshRenderer', '    mesh floor',
        '    material "materials/stone.swmaterial"', 'end', '',
    ]
    for i, (name, material) in enumerate([('Stone', 'stone'), ('Stone detail', 'stone-detail'),
                                          ('Stone moss', 'stone-moss')]):
        lines += [f'entity "{name}"', '  Transform', f'    position {(i - 1) * 1.4:g} 0.6 0', '    rotation 0 25 0',
                  '  MeshRenderer', '    mesh cube', f'    material "materials/{material}.swmaterial"', 'end', '']
    for name, position, size in [('Leaves near', '-1.6 1.9 0.5', 1.2), ('Leaves far', '3 3 -14', 4)]:
        lines += [f'entity "{name}"', '  Transform', f'    position {position}', '    rotation 90 0 0',
                  f'    scale {size:g} 1 {size:g}', '  MeshRenderer', '    mesh plane', '    material "materials/leaves.swmaterial"', 'end', '']
    lines += ['entity "Main Camera"', '  Transform', '    position 0 1.4 4.2', '    rotation -10 0 0', '  Camera',
              '    fov 55', '    primary 1', 'end', '']
    (root / 'Scenes/material-layers.swscene').write_text('\n'.join(lines))



# ---- Ambient occlusion (G04) ----
# A corner where two walls meet the floor, a box on the floor, a thin pole far in front of a
# distant wall, a black emissive block in the corner, and sky across the top. Three
# variants share the layout: as authored, with ao_intensity 0 (ambient occlusion on must
# then match off exactly) and with no ambient light (nothing for it to darken).

def ambient_occlusion_scene(name, note, ambient=0.8, intensity=1.0):
    lines = [
        '# Generated by Tools/generate.py; edit that, not this file.',
        '# Ambient occlusion (G04): a wall corner, a box on the floor, a thin pole 7 m in front of',
        '# a far wall, a black emissive block in the corner and sky across the top. ' + note,
        '# Camera: "Main Camera".',
        'shockwave_scene 2', '',
        'entity "Environment"', '  Environment', '    sky_color 0.35 0.42 0.55 1', '    ground_color 0.2 0.18 0.15 1',
        f'    ambient_intensity {ambient:g}', '    bloom_intensity 0', '    ao_radius 0.5', f'    ao_intensity {intensity:g}',
        'end', '',
        'entity "Sun"', '  Transform', '    rotation -50 30 0', '  Light', '    type Directional',
        '    color 1 0.95 0.88 1', '    intensity 1.5', 'end', '',
        'entity "Floor"', '  Transform', '    scale 40 1 40', '  MeshRenderer', '    mesh floor',
        '    tint 0.8 0.8 0.8 1', 'end', '',
        'entity "Back wall"', '  Transform', '    position -1.5 1.5 -4', '    scale 5 3 0.2', '  MeshRenderer',
        '    mesh cube', '    tint 0.8 0.8 0.8 1', 'end', '',
        'entity "Side wall"', '  Transform', '    position -4 1.5 -1.5', '    scale 0.2 3 5', '  MeshRenderer',
        '    mesh cube', '    tint 0.8 0.8 0.8 1', 'end', '',
        'entity "Box"', '  Transform', '    position -1.2 0.4 -2.2', '    rotation 0 20 0', '    scale 0.8 0.8 0.8',
        '  MeshRenderer', '    mesh cube', '    tint 0.8 0.8 0.8 1', 'end', '',
        'entity "Far wall"', '  Transform', '    position 2 1.5 -12', '    scale 6 3 0.2', '  MeshRenderer',
        '    mesh cube', '    tint 0.8 0.8 0.8 1', 'end', '',
        'entity "Pole"', '  Transform', '    position 2.2 1.2 -6', '    scale 0.08 2.4 0.08', '  MeshRenderer',
        '    mesh cube', '    tint 0.8 0.8 0.8 1', 'end', '',
        'entity "Glow"', '  Transform', '    position -3.6 0.25 -3.6', '    scale 0.5 0.5 0.5', '  MeshRenderer',
        '    mesh cube', '    tint 0 0 0 1', '    emissive 1 0.5 0.1 1', '    emissive_intensity 2', 'end', '',
        'entity "Main Camera"', '  Transform', '    position 0 1.8 3', '    rotation -14 20 0', '  Camera',
        '    fov 60', '    primary 1', 'end', '',
    ]
    (root / f'Scenes/{name}.swscene').write_text('\n'.join(lines))


# ---- Clustered lighting (G11) ----
# Many point and spot lights over a long floor (some past cluster_far, some casting shadows,
# a glass panel in front), and a crowd of overlapping lights that overflows every cluster's
# list near it. Clustered lighting must render both exactly as brute force does.
def light_scenes():
    def light(name, kind, position, color, intensity, rng, rotation=None, angle=None, shadows=False):
        out = [f'entity "{name}"', '  Transform', f'    position {position[0]:g} {position[1]:g} {position[2]:g}']
        if rotation: out.append(f'    rotation {rotation[0]:g} {rotation[1]:g} 0')
        out += ['  Light', f'    type {kind}', f'    color {color[0]:g} {color[1]:g} {color[2]:g} 1',
                f'    intensity {intensity:g}', f'    range {rng:g}']
        if angle: out.append(f'    spot_angle {angle:g}')
        if shadows: out.append('    cast_shadows true')
        return out + ['end', '']

    def common(far):
        return ['entity "Environment"', '  Environment', '    sky_color 0.35 0.42 0.55 1',
                '    ground_color 0.2 0.18 0.15 1', '    ambient_intensity 0.15', f'    cluster_far {far:g}', 'end', '',
                'entity "Moon"', '  Transform', '    rotation -35 20 0', '  Light', '    type Directional',
                '    color 0.6 0.7 1 1', '    intensity 0.4', 'end', '',
                'entity "Floor"', '  Transform', '    position 0 0 -40', '    scale 60 1 120', '  MeshRenderer',
                '    mesh floor', '    texture grid', '    tint 0.8 0.8 0.8 1', 'end', '']

    palette = [(1, 0.55, 0.3), (0.35, 0.6, 1), (0.4, 1, 0.5), (1, 0.4, 0.8), (1, 0.9, 0.4)]
    lines = [
        '# Generated by Tools/generate.py; edit that, not this file.',
        '# 200 point lights in a grid down a long floor, four spot lights (two shadowed), boxes',
        '# and a glass panel. cluster_far is 40 m: lights further than that are found by testing',
        '# every light. Clustered lighting (G11) must match brute force exactly.',
        '# Cameras: "Main Camera", "Overhead".',
        'shockwave_scene 2', '',
    ] + common(40)
    n = 0
    for row in range(20):
        for col in range(10):
            x, z = -18 + col * 4, 2 - row * 4
            lines += light(f'Point {n}', 'Point', (x + (row % 2), 0.6, z), palette[(row * 3 + col) % 5], 2.5, 3.5)
            n += 1
    for i, (x, yaw, shadows) in enumerate([(-6, -20, True), (-2, -5, False), (2, 5, True), (6, 20, False)]):
        lines += light(f'Spot {i}', 'Spot', (x, 5, 2), palette[i], 25, 16, (-60, yaw), 30, shadows)
    for i in range(16):
        x, z = -15 + (i % 8) * 4.3, -3 - (i // 8) * 9
        lines += [f'entity "Box {i}"', '  Transform', f'    position {x:g} 0.5 {z:g}', '    scale 1 1 1',
                  '  MeshRenderer', '    mesh cube', '    tint 0.75 0.75 0.75 1', 'end', '']
    lines += ['entity "Glass"', '  Transform', '    position 1 0 1', '  ModelRenderer', '    model models/panels.glb',
              'end', '',
              'entity "Main Camera"', '  Transform', '    position 0 3 9', '    rotation -12 0 0', '  Camera',
              '    fov 60', '    near_plane 0.1', '    primary 1', 'end', '',
              'entity "Overhead"', '  Transform', '    position 0 45 -25', '    rotation -80 0 0', '  Camera',
              '    fov 60', '    primary 0', 'end', '']
    (root / 'Scenes/many-lights.swscene').write_text('\n'.join(lines))

    lines = [
        '# Generated by Tools/generate.py; edit that, not this file.',
        '# 96 dim point lights crowded around a pillar, all reaching the middle of the view: more',
        '# than a cluster lists (64), so those clusters overflow and test every light. Clustered',
        '# lighting (G11) must still match brute force exactly. Camera: "Main Camera".',
        'shockwave_scene 2', '',
    ] + common(250)
    lines += ['entity "Pillar"', '  Transform', '    position 0 1 -1.2', '    scale 0.4 2 0.4', '  MeshRenderer',
              '    mesh cube', '    tint 0.7 0.7 0.7 1', 'end', '']
    for i in range(96):
        a = i * 2.39996
        r = 0.5 + 2.5 * ((i * 37) % 96) / 96
        lines += light(f'Crowd {i}', 'Point', (r * math.cos(a), 0.3 + (i % 4) * 0.5, -1.2 + r * math.sin(a)),
                       palette[i % 5], 0.12, 7)
    lines += ['entity "Main Camera"', '  Transform', '    position 0 1.8 5', '    rotation -12 0 0', '  Camera',
              '    fov 60', '    near_plane 0.05', '    primary 1', 'end', '']
    (root / 'Scenes/light-overflow.swscene').write_text('\n'.join(lines))


(root / 'Content/textures/checker-fine.png').write_bytes(checker())
(root / 'Content/textures/ramp.png').write_bytes(ramp())
panels()
skinned_strip()
dense_scene()
materials_models()
missing_uv1()
materials_scene(False)
materials_scene(True)
(root / 'Content/textures/ridges-normal.png').write_bytes(ridges_normal())
(root / 'Content/textures/leaf.png').write_bytes(leaf())
material_files()
material_instances_scene()
light_scenes()
material_layers_scene()
ambient_occlusion_scene('ambient-occlusion', 'GraphicsFixturesCheck: the AO view is white on sky and open floor, darker in the corner.')
ambient_occlusion_scene('ambient-occlusion-zero', 'ao_intensity 0: ambient occlusion on must match off exactly.', intensity=0)
ambient_occlusion_scene('ambient-occlusion-direct', 'No ambient light: ambient occlusion has nothing to darken.', ambient=0)
