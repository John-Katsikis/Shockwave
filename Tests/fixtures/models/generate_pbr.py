"""Regenerate pbr-maps.glb, a self-authored model with every PBR map (stdlib only).

The same file is the model_import test fixture and the sample's material demo
(Games/Sample/Content/models/pbr-maps.glb). Its tiles have bevelled edges and domes in
the normal map, alternate polished metal and rough clay in the metallic-roughness map,
darken their grooves in the occlusion map and glow along one row in the emissive map.
"""
import json, math, struct, zlib
from pathlib import Path
root = Path(__file__).resolve().parent
SIZE, TILE, GROOVE, BEVEL = 256, 64, 4, 10

def png(pixels):
    def chunk(kind, data): return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    rows = b''.join(b'\0' + bytes(pixels[y * SIZE * 4:(y + 1) * SIZE * 4]) for y in range(SIZE))
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', SIZE, SIZE, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b'')

def tile(x, y):
    """(metal tile, glowing row, distance from the tile's edge in pixels, height 0..1)."""
    x %= SIZE; y %= SIZE
    lx, ly = x % TILE, y % TILE
    metal = (x // TILE + y // TILE) % 2 == 0
    d = min(lx, TILE - 1 - lx, ly, TILE - 1 - ly)
    h = 0.0 if d < GROOVE else min((d - GROOVE) / BEVEL, 1.0)
    r = math.hypot(lx - TILE / 2 + 0.5, ly - TILE / 2 + 0.5) / 18
    if not metal and r < 1: h += 0.6 * math.sqrt(1 - r * r)
    return metal, y // TILE == 1, d, h

base, normal, orm, emissive = [], [], [], []
for y in range(SIZE):
    for x in range(SIZE):
        metal, glow, d, h = tile(x, y)
        groove = d < GROOVE
        base += [40, 40, 40, 255] if groove else [230, 190, 120, 255] if metal else [180, 75, 55, 255]
        # +X right, +Y up the image (glTF): height rising to the right tilts the normal left.
        k = 6.0
        nx = -(tile(x + 1, y)[3] - tile(x - 1, y)[3]) * k
        ny = (tile(x, y + 1)[3] - tile(x, y - 1)[3]) * k
        length = math.sqrt(nx * nx + ny * ny + 1)
        normal += [round((nx / length * 0.5 + 0.5) * 255), round((ny / length * 0.5 + 0.5) * 255), round((1 / length * 0.5 + 0.5) * 255), 255]
        occlusion = 0.35 if groove else 0.6 + 0.4 * min((d - GROOVE) / BEVEL, 1.0)
        roughness = 1.0 if groove else 0.25 if metal else 0.85
        orm += [round(occlusion * 255), round(roughness * 255), 0 if groove or not metal else 255, 255]
        emissive += [255, 120, 20, 255] if groove and glow else [0, 0, 0, 255]
images = [png(p) for p in (base, normal, orm, emissive)]

def cube(size):
    faces = [((0, 0, 1), (1, 0, 0), (0, 1, 0)), ((1, 0, 0), (0, 0, -1), (0, 1, 0)), ((-1, 0, 0), (0, 0, 1), (0, 1, 0)),
             ((0, 1, 0), (1, 0, 0), (0, 0, -1)), ((0, -1, 0), (1, 0, 0), (0, 0, 1)), ((0, 0, -1), (-1, 0, 0), (0, 1, 0))]
    p, n, uv, idx = [], [], [], []
    for normal_, u, up in faces:
        first = len(p) // 3
        for s, t in ((0, 0), (1, 0), (1, 1), (0, 1)):   # top-left origin, like glTF
            p += [size * (normal_[i] * 0.5 + (s - 0.5) * u[i] + (0.5 - t) * up[i]) for i in range(3)]
            n += normal_; uv += [s, t]
        idx += [first, first + 3, first + 2, first, first + 2, first + 1]
    return p, n, uv, idx, None

def sphere(radius, segments=48, rings=24):
    p, n, uv, idx = [], [], [], []
    for r in range(rings + 1):
        theta = math.pi * r / rings
        for s in range(segments + 1):
            phi = 2 * math.pi * s / segments
            d = (math.sin(theta) * math.cos(phi), math.cos(theta), -math.sin(theta) * math.sin(phi))
            p += [radius * c for c in d]; n += d; uv += [2 * s / segments, r / rings]
    for r in range(rings):
        for s in range(segments):
            a, b = r * (segments + 1) + s, (r + 1) * (segments + 1) + s
            idx += [a, b, b + 1, a, b + 1, a + 1]
    return p, n, uv, idx, None

def mirrored_quad(width, height):
    # The texture runs right to left (u = 1 at x = 0), and the file gives tangents for it:
    # pointing along -X with handedness -1, so the bitangent still points up the image.
    p = [0, 0, 0, width, 0, 0, width, height, 0, 0, height, 0]
    uv = [2, 1, 0, 1, 0, 0, 2, 0]
    return [c - width / 2 if i % 3 == 0 else c for i, c in enumerate(p)], [0, 0, 1] * 4, uv, [0, 1, 2, 0, 2, 3], [-1, 0, 0, -1] * 4

blob = bytearray()
views, accessors = [], []
def add(data, fmt, kind, target=None, count=None, minmax=False):
    while len(blob) % 4: blob.append(0)
    raw = struct.pack('<' + fmt * len(data), *data)
    views.append({'buffer': 0, 'byteOffset': len(blob), 'byteLength': len(raw), **({'target': target} if target else {})})
    blob.extend(raw)
    components = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}[kind]
    accessor = {'bufferView': len(views) - 1, 'componentType': 5126 if fmt == 'f' else 5125, 'count': len(data) // components, 'type': kind}
    if minmax: accessor.update(min=[min(data[i::3]) for i in range(3)], max=[max(data[i::3]) for i in range(3)])
    accessors.append(accessor)
    return len(accessors) - 1
def geometry(shape):
    p, n, uv, idx, tangents = shape
    attributes = {'POSITION': add(p, 'f', 'VEC3', 34962, minmax=True), 'NORMAL': add(n, 'f', 'VEC3', 34962), 'TEXCOORD_0': add(uv, 'f', 'VEC2', 34962)}
    if tangents: attributes['TANGENT'] = add(tangents, 'f', 'VEC4', 34962)
    return {'attributes': attributes, 'indices': add(idx, 'I', 'SCALAR', 34963)}
shapes = {'cube': geometry(cube(1.6)), 'sphere': geometry(sphere(0.8)), 'quad': geometry(mirrored_quad(6, 3))}
image_views = []
for data in images:
    while len(blob) % 4: blob.append(0)
    views.append({'buffer': 0, 'byteOffset': len(blob), 'byteLength': len(data)})
    blob.extend(data)
    image_views.append({'bufferView': len(views) - 1, 'mimeType': 'image/png'})
while len(blob) % 4: blob.append(0)

def primitive(shape, material): return dict(shapes[shape], material=material)
doc = {
    'asset': {'version': '2.0', 'generator': 'Shockwave self-authored fixtures'},
    'buffers': [{'byteLength': len(blob)}], 'bufferViews': views, 'accessors': accessors,
    'images': image_views, 'samplers': [{'magFilter': 9729, 'minFilter': 9987}],
    'textures': [{'source': i, 'sampler': 0} for i in range(4)],
    'materials': [
        {'name': 'Tiles', 'pbrMetallicRoughness': {'baseColorTexture': {'index': 0}, 'metallicFactor': 1, 'roughnessFactor': 1, 'metallicRoughnessTexture': {'index': 2}},
         'normalTexture': {'index': 1, 'scale': 1}, 'occlusionTexture': {'index': 2, 'strength': 1},
         'emissiveTexture': {'index': 3}, 'emissiveFactor': [1, 1, 1]},
        {'name': 'Polished gold', 'pbrMetallicRoughness': {'baseColorFactor': [1, 0.77, 0.34, 1], 'metallicFactor': 1, 'roughnessFactor': 0.25}},
        {'name': 'Glow', 'pbrMetallicRoughness': {'baseColorFactor': [0.05, 0.05, 0.05, 1], 'metallicFactor': 0, 'roughnessFactor': 0.6}, 'emissiveFactor': [0.2, 1, 0.4]},
    ],
    'meshes': [{'name': 'Tile cube', 'primitives': [primitive('cube', 0)]}, {'name': 'Tile sphere', 'primitives': [primitive('sphere', 0)]},
               {'name': 'Gold sphere', 'primitives': [primitive('sphere', 1)]}, {'name': 'Glow sphere', 'primitives': [primitive('sphere', 2)]},
               {'name': 'Mirrored wall', 'primitives': [primitive('quad', 0)]}],
    'nodes': [{'name': 'Materials', 'children': [1, 2, 3, 4, 5]},
              {'name': 'Cube', 'mesh': 0, 'translation': [-2.2, 0.8, 0], 'rotation': [0, 0.2588, 0, 0.9659]},
              {'name': 'Tiled ball', 'mesh': 1, 'translation': [0, 0.8, 0]},
              {'name': 'Gold ball', 'mesh': 2, 'translation': [2.2, 0.8, 0]},
              {'name': 'Glow ball', 'mesh': 3, 'translation': [1.1, 0.3, 1.4], 'scale': [0.375, 0.375, 0.375]},
              {'name': 'Wall', 'mesh': 4, 'translation': [0, 0, -1.6]}],
    'scenes': [{'nodes': [0]}], 'scene': 0,
}
encoded = json.dumps(doc, separators=(',', ':')).encode()
encoded += b' ' * ((-len(encoded)) % 4)
glb = struct.pack('<III', 0x46546c67, 2, 28 + len(encoded) + len(blob)) + struct.pack('<I4s', len(encoded), b'JSON') + encoded + struct.pack('<I4s', len(blob), b'BIN\0') + bytes(blob)
(root / 'pbr-maps.glb').write_bytes(glb)
(root.parents[2] / 'Games/Sample/Content/models/pbr-maps.glb').write_bytes(glb)
