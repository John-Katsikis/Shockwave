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
  Scenes/crowded-city.swscene        G12: blocks of buildings full of props, sky gaps, foliage,
                                     glass and a tower off camera whose shadow falls in view
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
  Content/effects/*.efkefc           particle effects in Effekseer's runtime format (A04): soft
                                     smoke in each blend mode, heat haze, lit smoke, two layers
                                     (Content/effects/upstream/ is Effekseer's own, not generated)
  Content/effects/textures/*.png     their textures: a soft puff and a ripple distortion map
  Scenes/particles-soft.swscene, particles-distortion.swscene, particles-lit.swscene, and
                                     particles-soft-empty.swscene (the same room without them)
  Content/postprocess/*.swpostprocess  fog, grading, neutral and auto exposure profiles (A05, A06)
  Content/postprocess/luts/*.cube    a warm grade and an identity LUT, 17 entries a side
  Scenes/postprocess-*.swscene       a foggy avenue with a lamp, a graded colour chart, a dark
                                     and a bright room metered by auto exposure, and each
                                     without its volume
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


def crowded_city():
    # G12 (docs/Drawing.md): a street-level view down an avenue of buildings, each block
    # hiding a yard of props. Sky shows between the buildings and over them, so culling
    # must keep what a gap reveals; the tower behind the camera casts its shadow into view
    # (shadows are never culled); masked foliage and glass panels line the street.
    lines = [
        '# Generated by Tools/generate.py; edit that, not this file.',
        '# Crowded city (G12): instancing, occlusion culling and indirect draws must leave every',
        '# pixel as it was. Cameras: "Main Camera" (street level), "Overhead" (sees everything).',
        'shockwave_scene 2', '',
        'entity "Environment"', '  Environment', '    sky_color 0.4 0.5 0.65 1', '    ground_color 0.2 0.18 0.15 1',
        '    ambient_intensity 0.5', 'end', '',
        'entity "Sun"', '  Transform', '    rotation -35 150 0', '  Light', '    type Directional',
        '    color 1 0.93 0.85 1', '    intensity 3', 'end', '',
        'entity "Ground"', '  Transform', '    position 0 0 -30', '    scale 160 1 160', '  MeshRenderer',
        '    mesh floor', '    texture grid', '    tint 0.5 0.5 0.52 1', 'end', '',
        'entity "Tower"', '  Transform', '    position -9 15 14', '    scale 4 30 4', '  MeshRenderer', '    mesh cube',
        '    tint 0.6 0.6 0.65 1', 'end', '',
        'entity "Main Camera"', '  Transform', '    position 0.5 1.7 8', '    rotation -2 4 0', '  Camera', '    fov 60',
        '    primary 1', 'end', '',
        'entity "Overhead"', '  Transform', '    position 0 70 -20', '    rotation -82 0 0', '  Camera', '    fov 60',
        '    primary 0', 'end', '',
    ]
    palette = [(0.75, 0.68, 0.6), (0.55, 0.6, 0.68), (0.7, 0.55, 0.45), (0.62, 0.62, 0.58)]
    props = [(0.8, 0.3, 0.25), (0.3, 0.7, 0.35), (0.3, 0.45, 0.85), (0.85, 0.75, 0.3)]
    n = 0
    for bx in (-1, 1):   # the two sides of the avenue
        for bz in range(6):
            cx, cz = bx * 9, -4 - bz * 12
            height = 6 + ((bz * 7 + (bx + 1) * 3) % 5) * 2
            c = palette[(bz + bx + 2) % 4]
            # Four walls around a yard; the one on the street has a gap, a little wider each block.
            gap = 0.4 + bz * 0.3
            for i, (px, pz, sx, sz) in enumerate([
                    (cx, cz - 4.8, 10, 0.4), (cx + bx * 4.8, cz, 0.4, 10),
                    (cx - bx * 4.8, cz - 2.4 - gap / 4, 0.4, 5.2 - gap / 2),
                    (cx - bx * 4.8, cz + 2.4 + gap / 4, 0.4, 5.2 - gap / 2),
                    (cx, cz + 4.8, 10, 0.4)]):
                lines += [f'entity "Building {n}"', '  Transform', f'    position {px:g} {height / 2:g} {pz:g}',
                          f'    scale {sx:g} {height:g} {sz:g}', '  MeshRenderer', '    mesh cube',
                          f'    tint {c[0]:g} {c[1]:g} {c[2]:g} 1', 'end', '']
                n += 1
            # The yard: a grid of props, spheres and cubes, two layers.
            for layer in range(2):
                for r in range(8):
                    for q in range(8):
                        x, z = cx - 3.5 + q, cz - 3.5 + r
                        y = 0.35 + layer * 0.8
                        k = (r * 3 + q + layer + bz) % 4
                        pc = props[k]
                        mesh = 'sphere' if k % 2 else 'cube'
                        lines += [f'entity "Prop {n}"', '  Transform', f'    position {x:g} {y:g} {z:g}',
                                  '    scale 0.5 0.5 0.5', '  MeshRenderer', f'    mesh {mesh}',
                                  f'    tint {pc[0]:g} {pc[1]:g} {pc[2]:g} 1', 'end', '']
                        n += 1
    # Street furniture in the open: lamp posts, masked hedges and glass panels.
    for i in range(10):
        z = 4 - i * 7
        for side in (-1, 1):
            lines += [f'entity "Post {i} {side}"', '  Transform', f'    position {side * 3.2:g} 1.5 {z:g}',
                      '    scale 0.15 3 0.15', '  MeshRenderer', '    mesh cube', '    tint 0.2 0.2 0.22 1', 'end', '']
        lines += [f'entity "Hedge {i}"', '  Transform', f'    position -2.6 0.6 {z - 3:g}', '    rotation 0 90 90',
                  '    scale 1.6 1 1.2', '  MeshRenderer', '    mesh plane', '    material "materials/leaves.swmaterial"',
                  'end', '']
    for i in range(3):
        lines += [f'entity "Glass {i}"', '  Transform', f'    position 2.2 0 {-2 - i * 14:g}', '  ModelRenderer',
                  '    model models/panels.glb', 'end', '']
    (root / 'Scenes/crowded-city.swscene').write_text('\n'.join(lines))


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


# ---- Particle effects (A04) -----------------------------------------------------------
# Written straight to Effekseer's runtime format, version 1610, as Games/Sample's
# MakeParticleStress.py does (field order from Effekseer 1.7.3's loader:
# EffectImplemented::LoadBody, EffectNodeImplemented::LoadParameter, EffectNodeSprite).
# Every particle is still and lives forever, so a screenshot is the same on any frame.
FX_VERSION = 1610
FX_COLOR_TEXTURES = ['textures/puff.png']
FX_DISTORTION_TEXTURES = ['textures/ripple.png']
FX_FOREVER = 2**31 - 1
FX_DEFAULT, FX_DISTORTION, FX_LIT = 0, 6, 7   # Effekseer's RendererMaterialType
FX_OPAQUE, FX_ALPHA, FX_ADD, FX_SUBTRACT, FX_MULTIPLY = range(5)   # AlphaBlendType


def i32(*v):
    return struct.pack(f'<{len(v)}i', *v)


def f32(*v):
    return struct.pack(f'<{len(v)}f', *v)


def puff(size=64):
    """White, alpha falling smoothly from 1 in the middle to 0 at the edge."""
    pixels = []
    for y in range(size):
        for x in range(size):
            r = math.hypot((x + 0.5) / size * 2 - 1, (y + 0.5) / size * 2 - 1)
            a = max(0.0, 1 - r) ** 1.5
            pixels += [255, 255, 255, round(a * 255)]
    return png(size, size, pixels)


def ripple(size=64, waves=3):
    """A distortion map (data, not colour): red and green are the offset along u and v,
    0.5 meaning none, in rings around the middle; alpha fades out toward the edge."""
    pixels = []
    for y in range(size):
        for x in range(size):
            u, v = (x + 0.5) / size * 2 - 1, (y + 0.5) / size * 2 - 1
            r = math.hypot(u, v)
            w = math.sin(r * waves * 2 * math.pi) * (1 - min(r, 1))
            dx, dy = (u / r * w, v / r * w) if r > 1e-6 else (0, 0)
            pixels += [round((dx * 0.5 + 0.5) * 255), round((dy * 0.5 + 0.5) * 255), 128,
                       round(max(0.0, 1 - r) ** 0.5 * 255)]
    return png(size, size, pixels)


def fx_sprite(at, size, color, blend=FX_ALPHA, material=FX_DEFAULT, soft=(0, 0, 0), distortion=1.0):
    """One still, camera-facing sprite at `at` (metres), size metres wide. soft: Effekseer's
    soft particle distances (far, near, near offset)."""
    return dict(at=at, size=size, color=color, blend=blend, material=material, soft=soft, distortion=distortion)


def fx_node(n, number):
    out = [i32(2, 1, number)]   # a sprite, drawn, its slot in the draw list
    # Common values: one particle, bound at creation, never removed, living forever, no delay.
    out += [i32(80), i32(*[-1] * 7), i32(1, 1, 1, 1), i32(1, 0, 0), i32(FX_FOREVER, FX_FOREVER), f32(0, 0), f32(0, 0)]
    out += [i32(1, 96), i32(*[-1] * 6), f32(0, 0, 0, 0, 0, 0), f32(0, 0, 0, 0, 0, 0), f32(0, 0, 0, 0, 0, 0)]
    out += [i32(4)] + [i32(0) + f32(1, 0, 0, 0, 0, 0, 0) + i32(0)] * 4   # no local force fields
    out += [i32(1, 96), i32(*[-1] * 6), f32(0, 0, 0, 0, 0, 0), f32(0, 0, 0, 0, 0, 0), f32(0, 0, 0, 0, 0, 0)]
    out += [i32(3, 24), f32(n['size'], n['size']), f32(0, 0), f32(0, 0)]   # one scale for x and y
    out += [i32(0, 0), f32(*n['at'], *n['at'])]   # spawned at a point
    out += [f32(0), i32(0, 0), f32(1, 3.4028234663852886e38), i32(0, 0), f32(1)]   # depth: defaults
    texture = 0
    out += [i32(n['material'])]
    if n['material'] != FX_DISTORTION:
        out += [f32(1)]   # emissive scaling
    out += [i32(texture, *[-1] * 6), i32(n['blend'])]
    out += [i32(1, 1) * 7, i32(1, 0)]   # linear, clamped textures; depth test, no depth write
    out += [i32(0), i32(0)]   # no fade in or out
    out += [i32(0, 0, 0), f32(0.01), i32(0, -1, 0, 0), f32(0.01)]   # no UV animation
    out += [i32(0), f32(n['distortion']), i32(0, 0), i32(0), i32(0), f32(*n['soft'])]
    out += [i32(2, 0, 0), i32(0), bytes(max(0, min(255, round(c * 255))) for c in n['color'])]
    out += [i32(0), i32(1), f32(-0.5, -0.5, 0.5, -0.5, -0.5, 0.5, 0.5, 0.5)]
    out += [i32(0), i32(0)]   # no sound, no children
    return b''.join(out)


def fx_file(*nodes):
    body = [b'SKFE', i32(FX_VERSION), i32(len(FX_COLOR_TEXTURES))]
    for path in FX_COLOR_TEXTURES:
        body += [i32(len(path) + 1), (path + '\0').encode('utf-16-le')]
    body += [i32(0), i32(len(FX_DISTORTION_TEXTURES))]   # no normal images; distortion images
    for path in FX_DISTORTION_TEXTURES:
        body += [i32(len(path) + 1), (path + '\0').encode('utf-16-le')]
    body += [i32(0) * 5]   # sounds, models, materials, curves, procedural models
    body += [i32(4), f32(0, 0, 0, 0), i32(0)]   # dynamic inputs, no equations
    body += [i32(len(nodes), 0), f32(1), i32(-1), i32(0)]   # magnification 1, random seed, no culling
    body += [i32(-1, len(nodes))] + [fx_node(n, i) for i, n in enumerate(nodes)]
    runtime = b''.join(body)
    return b'EFKE' + i32(0) + b'BIN_' + i32(len(runtime)) + runtime


def particle_fixtures():
    fx = root / 'Content/effects'
    (fx / 'textures').mkdir(parents=True, exist_ok=True)
    (fx / 'textures/puff.png').write_bytes(puff())
    (fx / 'textures/ripple.png').write_bytes(ripple())
    soft = (1.0, 0.0, 0.0)   # fade over a metre to the scene behind
    # Big puffs half sunk into the floor and against the wall, in each blend mode, left to right.
    for name, blend, color in [('alpha', FX_ALPHA, (0.85, 0.85, 0.9, 0.9)), ('add', FX_ADD, (1, 0.6, 0.25, 1)),
                               ('subtract', FX_SUBTRACT, (0.5, 0.5, 0.5, 1)),
                               ('multiply', FX_MULTIPLY, (0.2, 0.5, 1, 1))]:
        nodes = [fx_sprite((0, 0, 0), 2.4, color, blend, soft=soft), fx_sprite((0, 1.2, -1.0), 2.4, color, blend, soft=soft)]
        (fx / f'soft-{name}.efkefc').write_bytes(fx_file(*nodes))
    # A puff that fades out toward the camera: over 2 m, gone at 0.5 m (near 2, offset 0.5).
    (fx / 'soft-near.efkefc').write_bytes(fx_file(fx_sprite((0, 0, 0), 0.5, (0.9, 0.9, 0.9, 0.9), soft=(0, 2.0, 0.5))))
    (fx / 'heat-haze.efkefc').write_bytes(
        fx_file(fx_sprite((0, 0, 0), 2.5, (1, 1, 1, 1), FX_ALPHA, FX_DISTORTION, distortion=0.25)))
    (fx / 'lit-smoke.efkefc').write_bytes(fx_file(fx_sprite((0, 0, 0), 2.0, (0.8, 0.8, 0.8, 1), FX_ALPHA, FX_LIT)))
    # Two parts made near one first, a far glow behind it: sorted whole they keep that order,
    # sorted in parts the far one goes first (EffectsTest).
    (fx / 'two-layers.efkefc').write_bytes(fx_file(fx_sprite((0, 0, 0), 1.0, (1, 1, 1, 0.8)),
                                                   fx_sprite((0, 0, -3), 1.0, (1, 0.5, 0.2, 1), FX_ADD)))

    def effect(name, file, position, **extra):
        out = [f'entity "{name}"', '  Transform', f'    position {position[0]:g} {position[1]:g} {position[2]:g}',
               '  ParticleEffect', f'    effect effects/{file}']
        out += [f'    {k} {v}' for k, v in extra.items()]
        return out + ['end', '']

    def room(ambient=0.5):
        return ['entity "Environment"', '  Environment', '    sky_color 0.35 0.42 0.55 1',
                '    ground_color 0.2 0.18 0.15 1', f'    ambient_intensity {ambient:g}', 'end', '',
                'entity "Sun"', '  Transform', '    rotation -50 30 0', '  Light', '    type Directional',
                '    color 1 0.96 0.9 1', '    intensity 1.5', 'end', '',
                'entity "Floor"', '  Transform', '    scale 20 1 20', '  MeshRenderer', '    mesh floor',
                '    texture grid', '    tint 0.8 0.8 0.8 1', 'end', '',
                'entity "Wall"', '  Transform', '    position 0 2 -2', '    scale 14 4 0.2', '  MeshRenderer',
                '    mesh cube', '    texture textures/checker-fine.png', '    tint 0.9 0.9 0.9 1', 'end', '']

    head = ['# Generated by Tools/generate.py; edit that, not this file.']
    lines = head + [
        '# Soft particles (A04): big puffs sunk into the floor and against the wall, alpha, add,',
        '# subtract and multiply from left to right, each fading over 1 m to the scene behind; and',
        '# a small one 1.3 m from the camera, partly faded by its near fade. soft_particles false shows the hard',
        '# edges. Camera: "Main Camera".', 'shockwave_scene 2', ''] + room()
    for i, name in enumerate(['alpha', 'add', 'subtract', 'multiply']):
        lines += effect(f'Puff {name}', f'soft-{name}.efkefc', (-4.5 + i * 3, 0, -0.6))
    lines += effect('Near puff', 'soft-near.efkefc', (0.9, 2.5, 4.7))
    lines += ['entity "Main Camera"', '  Transform', '    position 0 2.5 6', '    rotation -12 0 0', '  Camera',
              '    fov 60', '    near_plane 0.1', '    primary 1', 'end', '']
    (root / 'Scenes/particles-soft.swscene').write_text('\n'.join(lines))
    camera = ['entity "Main Camera"', '  Transform', '    position 0 2.5 6', '    rotation -12 0 0', '  Camera',
              '    fov 60', '    near_plane 0.1', '    primary 1', 'end', '']
    (root / 'Scenes/particles-soft-empty.swscene').write_text('\n'.join(head + [
        '# particles-soft.swscene without its particles: what soft particles fade to at the floor and',
        '# wall. Camera: "Main Camera".', 'shockwave_scene 2', ''] + room() + camera))

    lines = head + [
        '# Heat haze (A04): a distortion sprite in front of the checker wall, with a post nearer',
        '# than it down its middle that must stay sharp (the haze bends only what is behind it).',
        '# particle_distortion false draws no haze at all. Camera: "Main Camera".', 'shockwave_scene 2', ''] + room()
    lines += effect('Haze', 'heat-haze.efkefc', (0, 1.6, 0))
    lines += ['entity "Post"', '  Transform', '    position 0.4 1 1.2', '    scale 0.15 2 0.15', '  MeshRenderer',
              '    mesh cube', '    tint 0.9 0.3 0.2 1', 'end', '',
              'entity "Main Camera"', '  Transform', '    position 0 1.6 4', '  Camera', '    fov 50',
              '    near_plane 0.1', '    primary 1', 'end', '']
    (root / 'Scenes/particles-distortion.swscene').write_text('\n'.join(lines))

    lines = head + [
        '# Scene-lit particles (A04): a lit smoke puff with a red point light just left of it and a',
        '# blue one right of it. particle_lighting scene tints its sides red and blue; simple lights',
        '# it by the sun and ambient alone. Camera: "Main Camera".', 'shockwave_scene 2', ''] + room(0.3)
    lines += effect('Smoke', 'lit-smoke.efkefc', (0, 1.5, 0))
    for name, x, c in [('Red lamp', -1.3, '1 0.15 0.1 1'), ('Blue lamp', 1.3, '0.15 0.3 1 1')]:
        lines += [f'entity "{name}"', '  Transform', f'    position {x:g} 1.5 0.6', '  Light', '    type Point',
                  f'    color {c}', '    intensity 6', '    range 3', 'end', '']
    lines += ['entity "Main Camera"', '  Transform', '    position 0 1.6 4', '  Camera', '    fov 50',
              '    near_plane 0.1', '    primary 1', 'end', '']
    (root / 'Scenes/particles-lit.swscene').write_text('\n'.join(lines))



# ---- Colour grading, exposure and fog (A05, A06) ---------------------------------------
def cube_lut(size, grade, title):
    rows = [f'TITLE "{title}"', f'LUT_3D_SIZE {size}']
    for b in range(size):
        for g in range(size):
            for r in range(size):
                rgb = grade(r / (size - 1), g / (size - 1), b / (size - 1))
                rows.append(' '.join(f'{min(max(c, 0.0), 1.0):.6f}' for c in rgb))
    return '\n'.join(rows) + '\n'


def postprocess_fixtures():
    pp = root / 'Content/postprocess'
    (pp / 'luts').mkdir(parents=True, exist_ok=True)
    (pp / 'luts/neutral.cube').write_text(cube_lut(17, lambda r, g, b: (r, g, b), 'neutral'))
    # Warm and a little desaturated, in sRGB-encoded values as grading tools write them.
    def warm(r, g, b):
        y = 0.2126 * r + 0.7152 * g + 0.0722 * b
        return (0.8 * r + 0.2 * y + 0.06, 0.8 * g + 0.2 * y + 0.02, 0.7 * b + 0.2 * y)
    (pp / 'luts/warm.cube').write_text(cube_lut(17, warm, 'warm'))
    head = '# Generated by Tests/fixtures/graphics/Tools/generate.py; edit that, not this file.\nswpostprocess 1\n'
    (pp / 'fog.swpostprocess').write_text(head + '\n'.join([
        'fog_density 0.04', 'fog_height_falloff 0.12', 'fog_base_height 0', 'fog_color 0.45 0.5 0.58',
        'fog_albedo 0.9', 'fog_anisotropy 0.4', 'fog_volumetric_distance 48']) + '\n')
    (pp / 'grade.swpostprocess').write_text(head + '\n'.join([
        'white_point 0.2807 0.2883', 'lut "postprocess/luts/warm.cube"', 'lut_encoding srgb']) + '\n')
    (pp / 'neutral.swpostprocess').write_text(head + '\n'.join([
        'white_point 0.3127 0.329', 'lut "postprocess/luts/neutral.cube"']) + '\n')
    (pp / 'auto.swpostprocess').write_text(head + '\n'.join([
        'auto_exposure true', 'auto_min_ev -6', 'auto_max_ev 6', 'auto_reset_on_cut true']) + '\n')

    gen = ['# Generated by Tools/generate.py; edit that, not this file.']
    def volume(profile):
        return ['entity "Postprocess"', '  PostProcessVolume', f'    profile postprocess/{profile}.swpostprocess',
                '    unbounded true', 'end', '']
    def camera(position, rotation=(0, 0)):
        return ['entity "Main Camera"', '  Transform', f'    position {position[0]:g} {position[1]:g} {position[2]:g}',
                f'    rotation {rotation[0]:g} {rotation[1]:g} 0', '  Camera', '    fov 60', '    near_plane 0.1',
                '    primary 1', 'end', '']

    # The avenue: pillars every 10 m down both sides out to 150 m, a lamp 12 m away.
    avenue = ['entity "Environment"', '  Environment', '    sky_color 0.35 0.42 0.55 1',
              '    ground_color 0.2 0.18 0.15 1', '    ambient_intensity 0.4', 'end', '',
              'entity "Sun"', '  Transform', '    rotation -25 160 0', '  Light', '    type Directional',
              '    color 1 0.95 0.85 1', '    intensity 2', 'end', '',
              'entity "Floor"', '  Transform', '    position 0 0 -70', '    scale 40 1 200', '  MeshRenderer',
              '    mesh floor', '    texture grid', '    tint 0.8 0.8 0.8 1', 'end', '']
    for i in range(16):
        for side, x in (('left', -3), ('right', 3)):
            avenue += [f'entity "Pillar {side} {i}"', '  Transform', f'    position {x:g} 2 {-i * 10:g}',
                       '    scale 0.6 4 0.6', '  MeshRenderer', '    mesh cube', '    tint 0.75 0.7 0.65 1', 'end', '']
    avenue += ['entity "Lamp"', '  Transform', '    position 0 2.5 -12', '  Light', '    type Point',
               '    color 1 0.7 0.35 1', '    intensity 80', '    range 8', 'end', '']
    note = ['# Fog (A06): pillars down an avenue to 150 m and an orange lamp 12 m out, in height fog.',
            '# Far pillars fade to the fog colour, near ones barely change; the froxel volume shows the',
            '# lamp glowing in the air, which analytic fog can\'t. Camera: "Main Camera".']
    (root / 'Scenes/postprocess-fog.swscene').write_text('\n'.join(
        gen + note + ['shockwave_scene 2', ''] + avenue + volume('fog') + camera((0, 1.7, 6), (-3, 0))))
    (root / 'Scenes/postprocess-fog-none.swscene').write_text('\n'.join(
        gen + ['# postprocess-fog.swscene without its volume: fog_quality off must match it exactly.',
               'shockwave_scene 2', ''] + avenue + camera((0, 1.7, 6), (-3, 0))))

    # A colour chart: a grey ramp and saturated cubes under a neutral sun.
    chart = ['entity "Environment"', '  Environment', '    sky_color 0.3 0.3 0.3 1', '    ground_color 0.3 0.3 0.3 1',
             '    ambient_intensity 0.6', '    bloom_intensity 0', 'end', '',
             'entity "Sun"', '  Transform', '    rotation -60 20 0', '  Light', '    type Directional',
             '    color 1 1 1 1', '    intensity 1.5', 'end', '',
             'entity "Backdrop"', '  Transform', '    position 0 1.5 -1', '    scale 8 3 0.2', '  MeshRenderer',
             '    mesh cube', '    texture textures/ramp.png', '    tint 1 1 1 1', 'end', '']
    colors = [(0.9, 0.1, 0.1), (0.1, 0.8, 0.1), (0.1, 0.2, 0.9), (0.9, 0.8, 0.1), (0.9, 0.9, 0.9), (0.15, 0.15, 0.15)]
    for i, c in enumerate(colors):
        chart += [f'entity "Swatch {i}"', '  Transform', f'    position {-2.5 + i:g} 0.6 0.5', '    scale 0.8 0.8 0.8',
                  '  MeshRenderer', '    mesh cube', f'    tint {c[0]:g} {c[1]:g} {c[2]:g} 1', 'end', '']
    note = ['# Colour grading (A05): a grey ramp and six swatches. grade warms it through a white point',
            '# and a LUT; neutral (D65 and an identity LUT) changes nothing. Camera: "Main Camera".']
    for name, profile in (('grade', 'grade'), ('neutral', 'neutral')):
        (root / f'Scenes/postprocess-{name}.swscene').write_text('\n'.join(
            gen + note + ['shockwave_scene 2', ''] + chart + volume(profile) + camera((0, 1.5, 4.5), (-8, 0))))
    (root / 'Scenes/postprocess-chart.swscene').write_text('\n'.join(
        gen + ['# The colour chart without a volume: color_grading false must match it exactly.',
               'shockwave_scene 2', ''] + chart + camera((0, 1.5, 4.5), (-8, 0))))

    # Auto exposure: the same room lit dimly and brightly (5 stops apart).
    def room(intensity, auto):
        lines = ['entity "Environment"', '  Environment', '    sky_color 0.3 0.33 0.4 1',
                 '    ground_color 0.2 0.18 0.15 1', f'    ambient_intensity {0.3 * intensity:g}',
                 '    bloom_intensity 0', 'end', '',
                 'entity "Sun"', '  Transform', '    rotation -50 30 0', '  Light', '    type Directional',
                 '    color 1 0.96 0.9 1', f'    intensity {1.5 * intensity:g}', 'end', '',
                 'entity "Floor"', '  Transform', '    scale 20 1 20', '  MeshRenderer', '    mesh floor',
                 '    texture grid', '    tint 0.8 0.8 0.8 1', 'end', '',
                 'entity "Wall"', '  Transform', '    position 0 2 -3', '    scale 14 4 0.2', '  MeshRenderer',
                 '    mesh cube', '    texture textures/checker-fine.png', '    tint 0.9 0.9 0.9 1', 'end', '',
                 'entity "Box"', '  Transform', '    position 0.5 0.5 -1', '    rotation 0 25 0', '  MeshRenderer',
                 '    mesh cube', '    tint 0.8 0.5 0.3 1', 'end', '']
        return lines + (volume('auto') if auto else []) + camera((0, 1.6, 4), (-10, 0))
    for name, intensity in (('dark', 1 / 8), ('bright', 4)):
        note = [f'# Auto exposure (A05): the room lit {"8x dimmer" if name == "dark" else "4x brighter"} than usual, '
                'metered by auto exposure.', '# Camera: "Main Camera".']
        (root / f'Scenes/postprocess-{name}.swscene').write_text('\n'.join(
            gen + note + ['shockwave_scene 2', ''] + room(intensity, True)))
        (root / f'Scenes/postprocess-{name}-manual.swscene').write_text('\n'.join(
            gen + [f'# postprocess-{name}.swscene without its volume: auto_exposure false must match it exactly.',
                   'shockwave_scene 2', ''] + room(intensity, False)))


# ---- Character and specialized shading (A07-A09) ----
# A clearcoat on red spheres (clearcoat 0 must match plain exactly), thin and solid glass in
# front of a checker wall, two skin spheres side by side (diffusion mustn't bleed from one to
# the other) and hair cards. Each setting off matches its fallback scene, with no pass and no
# memory (GraphicsShadingCheck).

def strands(size=64):
    """Hair: strands running top to bottom (+v), cut out between them and at the tips."""
    pixels = []
    for y in range(size):
        for x in range(size):
            phase = (x * 7) % 11
            tip = size - 1 - (x * 13) % 9
            opaque = phase < 7 and y < tip
            shade = 150 + (x * 37) % 60
            pixels += [shade, int(shade * 0.72), int(shade * 0.45), 255 if opaque else 0]
    return png(size, size, pixels)


def shading_fixtures():
    folder = root / 'Content/materials/shading'
    folder.mkdir(parents=True, exist_ok=True)
    header = '# Generated by Tools/generate.py; edit that, not this file.\nswmaterial 1\n'
    files = {
        'red': 'base_color 0.75 0.08 0.06 1\nroughness 0.6\n',
        'red-coat': 'parent "materials/shading/red.swmaterial"\nclearcoat 1\nclearcoat_roughness 0.05\n',
        'red-coat-zero': 'parent "materials/shading/red.swmaterial"\nclearcoat 0\nclearcoat_roughness 0.05\n',
        'glass-thin': 'family transmission\nbase_color 0.85 1 0.9 1\nroughness 0.02\ntransmission_mode thin\n'
                      'double_sided true\n',
        'glass-solid': 'family transmission\nbase_color 1 1 1 1\nroughness 0.03\nior 1.5\nthickness 1\n'
                       'transmission_mode solid\nattenuation_color 0.55 0.75 1\nattenuation_distance 1.2\n',
        'glass-frosted': 'parent "materials/shading/glass-solid.swmaterial"\nroughness 0.45\n',
        'skin': 'family skin\nbase_color 0.8 0.55 0.45 1\nroughness 0.5\nskin_radius 12 5 3\n'
                'skin_strength 1\nskin_max_pixels 24\n',
        'skin-flat': 'parent "materials/shading/skin.swmaterial"\nskin_strength 0\n',
        'hair': 'family hair_card\nbase_color 0.55 0.38 0.22 1\nroughness 0.35\nalpha_mode mask\nalpha_cutoff 0.5\n'
                'double_sided true\nhair_anisotropy 0.8\nhair_direction v\ntexture base_color "textures/strands.png"\n',
    }
    for name, body in files.items():
        (folder / f'{name}.swmaterial').write_text(header + body)

    gen = ['# Generated by Tools/generate.py; edit that, not this file.']
    def lit(sun_rotation, ambient=0.5):
        return ['entity "Environment"', '  Environment', '    sky_color 0.35 0.42 0.55 1',
                '    ground_color 0.2 0.18 0.15 1', f'    ambient_intensity {ambient:g}', '    bloom_intensity 0', 'end',
                '', 'entity "Sun"', '  Transform', f'    rotation {sun_rotation[0]:g} {sun_rotation[1]:g} 0', '  Light',
                '    type Directional', '    color 1 0.96 0.9 1', '    intensity 3', 'end', '',
                'entity "Floor"', '  Transform', '    scale 20 1 20', '  MeshRenderer', '    mesh floor',
                '    texture grid', '    tint 0.7 0.7 0.7 1', 'end', '']
    def camera(position, rotation=(0, 0)):
        return ['entity "Main Camera"', '  Transform', f'    position {position[0]:g} {position[1]:g} {position[2]:g}',
                f'    rotation {rotation[0]:g} {rotation[1]:g} 0', '  Camera', '    fov 60', '    near_plane 0.1',
                '    primary 1', 'end', '']
    def thing(name, mesh, material, position, scale=(1, 1, 1), rotation=(0, 0, 0)):
        return [f'entity "{name}"', '  Transform', '    position ' + ' '.join(f'{v:g}' for v in position),
                '    rotation ' + ' '.join(f'{v:g}' for v in rotation), '    scale ' + ' '.join(f'{v:g}' for v in scale),
                '  MeshRenderer', f'    mesh {mesh}', f'    material "materials/shading/{material}.swmaterial"', 'end', '']
    def write(name, note, lines):
        (root / f'Scenes/{name}.swscene').write_text('\n'.join(gen + note + ['shockwave_scene 2', ''] + lines))

    # Clearcoat: three red spheres; the middle one's material varies by scene.
    for scene, middle, note in (
            ('shading-coat', 'red-coat', 'the middle sphere clear-coated: a sharp highlight over the rough red.'),
            ('shading-coat-plain', 'red', 'no coat anywhere: clearcoat false must match shading-coat exactly.'),
            ('shading-coat-zero', 'red-coat-zero', 'clearcoat 0 on the middle sphere: must match plain exactly.')):
        lines = lit((-40, 30))
        for i, material in enumerate(('red', middle, 'red')):
            lines += thing(f'Sphere {i}', 'sphere', material, ((i - 1) * 1.2, 0.5, 0))
        write(scene, [f'# Clearcoat (A07): {note}', '# Camera: "Main Camera".'], lines + camera((0, 1.0, 3.2), (-10, 0)))

    # Glass: a checker wall with a blue box behind thin, solid and frosted glass.
    wall = ['entity "Wall"', '  Transform', '    position 0 1.5 -2', '    scale 8 3 0.2', '  MeshRenderer', '    mesh cube',
            '    texture textures/checker-fine.png', '    tint 0.9 0.9 0.9 1', 'end', '',
            'entity "Box behind"', '  Transform', '    position 0 0.4 -1.2', '    scale 0.5 0.8 0.5', '  MeshRenderer',
            '    mesh cube', '    tint 0.15 0.3 0.9 1', 'end', '']
    glass = (thing('Thin pane', 'plane', 'glass-thin', (-1.6, 0.8, 0), (1, 1, 1.2), (90, 0, 0)) +
             thing('Solid block', 'cube', 'glass-solid', (0, 0.55, 0), (1, 1.1, 1)) +
             thing('Frosted block', 'cube', 'glass-frosted', (1.6, 0.55, 0), (1, 1.1, 1)))
    write('shading-glass', ['# Transmission (A08): a thin pane, a solid block that bends the wall behind it and a',
                            '# frosted one that blurs it. Camera: "Main Camera".'],
          lit((-50, 20)) + wall + glass + camera((0, 1.2, 3.6), (-8, 0)))
    write('shading-glass-none', ['# shading-glass.swscene without its glass: the scene behind it.'],
          lit((-50, 20)) + wall + camera((0, 1.2, 3.6), (-8, 0)))

    # Skin: two head-sized spheres side by side, lit from the side so the terminator is sharp.
    for scene, material, note in (
            ('shading-skin', 'skin', 'diffusion softens the terminator and reddens it, without bleeding between'),
            ('shading-skin-flat', 'skin-flat', 'skin_strength 0: skin_quality off must match shading-skin exactly;')):
        lines = lit((-10, 90), ambient=0.25)
        lines += thing('Head left', 'sphere', material, (-0.13, 0.6, 0), (0.25, 0.25, 0.25))
        lines += thing('Head right', 'sphere', material, (0.13, 0.6, 0), (0.25, 0.25, 0.25))
        write(scene, [f'# Skin (A09): {note}', '# the two spheres. Camera: "Main Camera".'],
              lines + camera((0, 0.62, 0.6), (-2, 0)))

    # Hair: cards facing the camera, lit from above and in front.
    lines = lit((-60, 0))
    for i in range(3):
        lines += thing(f'Card {i}', 'plane', 'hair', ((i - 1) * 0.9, 0.9, -0.1 * i), (0.8, 1, 1.4), (90, 0, 0))
    write('shading-hair', ['# Hair cards (A09): strands top to bottom, alpha-tested, with the anisotropic',
                           '# highlight across them (hair_quality high) or round (low). Camera: "Main Camera".'],
          lines + camera((0, 1.0, 2.6), (-5, 0)))


(root / 'Content/textures/checker-fine.png').write_bytes(checker())
(root / 'Content/textures/ramp.png').write_bytes(ramp())
panels()
skinned_strip()
dense_scene()
crowded_city()
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
particle_fixtures()
postprocess_fixtures()
(root / 'Content/textures/strands.png').write_bytes(strands())
shading_fixtures()
