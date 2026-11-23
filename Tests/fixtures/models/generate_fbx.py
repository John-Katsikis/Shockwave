#!/usr/bin/env python3
"""Writes the FBX test models (ASCII FBX 7.4), made for this project:

  fbx/crate.fbx  a textured cube under a parent node. Its texture is saved with an
                 artist's absolute path, C:/Users/artist/textures/checker.png, so the
                 import finds ../checker.png by name (docs/Assets.md).
  fbx/arm.fbx    a two-joint arm skinned to Shoulder and Elbow, with one take, "Wave":
                 the elbow bends 45 degrees and back over one second at 30 frames a second.

Both are Y-up, in centimetres (UnitScaleFactor 1), so Assimp and the Autodesk FBX SDK read
them without converting units or axes. Run: python3 Tests/fixtures/models/generate_fbx.py
"""
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SECOND = 46186158000  # FBX time units in one second


def numbers(values):
    out = []
    for v in values:
        if isinstance(v, float):
            text = ("%.6f" % v).rstrip("0").rstrip(".")
            out.append("0" if text in ("-0", "") else text)
        else:
            out.append(str(v))
    return ",".join(out)


def array(name, values, indent):
    return "%s%s: *%d {\n%s\ta: %s\n%s}\n" % (indent, name, len(values), indent, numbers(values), indent)


def header(objects_count):
    return """; FBX 7.4.0 project file
; Made for Shockwave's tests by Tests/fixtures/models/generate_fbx.py
FBXHeaderExtension:  {
\tFBXHeaderVersion: 1003
\tFBXVersion: 7400
\tCreator: "Shockwave test fixtures"
}
GlobalSettings:  {
\tVersion: 1000
\tProperties70:  {
\t\tP: "UpAxis", "int", "Integer", "",1
\t\tP: "UpAxisSign", "int", "Integer", "",1
\t\tP: "FrontAxis", "int", "Integer", "",2
\t\tP: "FrontAxisSign", "int", "Integer", "",1
\t\tP: "CoordAxis", "int", "Integer", "",0
\t\tP: "CoordAxisSign", "int", "Integer", "",1
\t\tP: "OriginalUpAxis", "int", "Integer", "",1
\t\tP: "OriginalUpAxisSign", "int", "Integer", "",1
\t\tP: "UnitScaleFactor", "double", "Number", "",1
\t\tP: "OriginalUnitScaleFactor", "double", "Number", "",1
\t\tP: "TimeMode", "enum", "", "",6
\t\tP: "TimeSpanStart", "KTime", "Time", "",0
\t\tP: "TimeSpanStop", "KTime", "Time", "",%d
\t\tP: "CustomFrameRate", "double", "Number", "",-1
\t}
}
""" % SECOND


def model(uid, name, kind, translation=(0, 0, 0), rotation=(0, 0, 0)):
    return """\tModel: %d, "Model::%s", "%s" {
\t\tVersion: 232
\t\tProperties70:  {
\t\t\tP: "Lcl Translation", "Lcl Translation", "", "A",%s
\t\t\tP: "Lcl Rotation", "Lcl Rotation", "", "A",%s
\t\t\tP: "Lcl Scaling", "Lcl Scaling", "", "A",1,1,1
\t\t}
\t\tShading: T
\t\tCulling: "CullingOff"
\t}
""" % (uid, name, kind, numbers(translation), numbers(rotation))


def geometry(uid, name, positions, polygons, normals, uvs, uv_indices):
    """polygons: lists of control point indices. normals and uv_indices: one per polygon corner."""
    index = []
    for poly in polygons:
        index += poly[:-1] + [-poly[-1] - 1]
    flat = [float(c) for p in positions for c in p]
    out = '\tGeometry: %d, "Geometry::%s", "Mesh" {\n' % (uid, name)
    out += array("Vertices", flat, "\t\t")
    out += array("PolygonVertexIndex", index, "\t\t")
    out += "\t\tGeometryVersion: 124\n"
    out += """\t\tLayerElementNormal: 0 {
\t\t\tVersion: 101
\t\t\tName: ""
\t\t\tMappingInformationType: "ByPolygonVertex"
\t\t\tReferenceInformationType: "Direct"
"""
    out += array("Normals", [float(c) for n in normals for c in n], "\t\t\t")
    out += "\t\t}\n"
    out += """\t\tLayerElementUV: 0 {
\t\t\tVersion: 101
\t\t\tName: "UVMap"
\t\t\tMappingInformationType: "ByPolygonVertex"
\t\t\tReferenceInformationType: "IndexToDirect"
"""
    out += array("UV", [float(c) for uv in uvs for c in uv], "\t\t\t")
    out += array("UVIndex", uv_indices, "\t\t\t")
    out += "\t\t}\n"
    out += """\t\tLayerElementMaterial: 0 {
\t\t\tVersion: 101
\t\t\tName: ""
\t\t\tMappingInformationType: "AllSame"
\t\t\tReferenceInformationType: "IndexToDirect"
\t\t\tMaterials: *1 {
\t\t\t\ta: 0
\t\t\t}
\t\t}
\t\tLayer: 0 {
\t\t\tVersion: 100
\t\t\tLayerElement:  {
\t\t\t\tType: "LayerElementNormal"
\t\t\t\tTypedIndex: 0
\t\t\t}
\t\t\tLayerElement:  {
\t\t\t\tType: "LayerElementUV"
\t\t\t\tTypedIndex: 0
\t\t\t}
\t\t\tLayerElement:  {
\t\t\t\tType: "LayerElementMaterial"
\t\t\t\tTypedIndex: 0
\t\t\t}
\t\t}
\t}
"""
    return out


def material(uid, name, diffuse):
    return """\tMaterial: %d, "Material::%s", "" {
\t\tVersion: 102
\t\tShadingModel: "lambert"
\t\tMultiLayer: 0
\t\tProperties70:  {
\t\t\tP: "DiffuseColor", "Color", "", "A",%s
\t\t\tP: "DiffuseFactor", "Number", "", "A",1
\t\t}
\t}
""" % (uid, name, numbers(diffuse))


def write(name, objects, connections):
    path = os.path.join(HERE, "fbx", name)
    with open(path, "w", newline="\n") as f:
        f.write(header(0))
        f.write("Objects:  {\n%s}\n" % objects)
        f.write("Connections:  {\n")
        for c in connections:
            if len(c) == 3:
                f.write('\tC: "%s",%d,%d\n' % c)
            else:
                f.write('\tC: "%s",%d,%d, "%s"\n' % c)
        f.write("}\n")
    print("wrote", os.path.relpath(path, os.path.dirname(os.path.dirname(os.path.dirname(HERE)))))


def crate():
    # A 100 cm cube: four corners per face, so each face has its own normal and UVs.
    faces = [((1, 0, 0), [(1, -1, -1), (1, 1, -1), (1, 1, 1), (1, -1, 1)]),
             ((-1, 0, 0), [(-1, -1, 1), (-1, 1, 1), (-1, 1, -1), (-1, -1, -1)]),
             ((0, 1, 0), [(-1, 1, -1), (-1, 1, 1), (1, 1, 1), (1, 1, -1)]),
             ((0, -1, 0), [(-1, -1, 1), (-1, -1, -1), (1, -1, -1), (1, -1, 1)]),
             ((0, 0, 1), [(-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]),
             ((0, 0, -1), [(1, -1, -1), (-1, -1, -1), (-1, 1, -1), (1, 1, -1)])]
    corners = [(-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1), (-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]
    positions = [(50 * x, 50 * y, 50 * z) for x, y, z in corners]
    polygons, normals, uv_indices = [], [], []
    for normal, quad in faces:
        polygons.append([corners.index(c) for c in quad])
        normals += [normal] * 4
        uv_indices += [0, 1, 2, 3]
    uvs = [(0, 0), (1, 0), (1, 1), (0, 1)]
    texture = "C:/Users/artist/textures/checker.png"
    objects = geometry(1001, "Crate", positions, polygons, normals, uvs, uv_indices)
    objects += model(2001, "Pallet", "Null", translation=(0, 10, 0))
    objects += '\tNodeAttribute: 3001, "NodeAttribute::Pallet", "Null" {\n\t\tTypeFlags: "Null"\n\t}\n'
    objects += model(2002, "Crate", "Mesh", translation=(0, 50, 0), rotation=(0, 30, 0))
    objects += material(4001, "Wood", (0.8, 0.6, 0.4))
    objects += """\tTexture: 5001, "Texture::Checker", "" {
\t\tType: "TextureVideoClip"
\t\tVersion: 202
\t\tTextureName: "Texture::Checker"
\t\tMedia: "Video::Checker"
\t\tFileName: "%s"
\t\tRelativeFilename: "%s"
\t}
\tVideo: 6001, "Video::Checker", "Clip" {
\t\tType: "Clip"
\t\tFileName: "%s"
\t\tRelativeFilename: "%s"
\t}
""" % (texture, texture, texture, texture)
    write("crate.fbx", objects, [
        ("OO", 2001, 0), ("OO", 3001, 2001), ("OO", 2002, 2001), ("OO", 1001, 2002), ("OO", 4001, 2002),
        ("OP", 5001, 4001, "DiffuseColor"), ("OO", 6001, 5001)])


def matrix_translate(x, y, z):
    return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1]


def arm():
    # A 10 x 200 x 10 cm bar along +Y: rings of four corners at y = 0, 100 and 200.
    ring = [(-5, -5), (5, -5), (5, 5), (-5, 5)]
    positions = [(x, y, z) for y in (0, 100, 200) for x, z in ring]
    polygons, normals, uv_indices, uvs = [], [], [], []
    for level in range(2):
        for side in range(4):
            a, b = level * 4 + side, level * 4 + (side + 1) % 4
            quad = [a, b, b + 4, a + 4]
            polygons.append(quad)
            mid = [(positions[a][i] + positions[b][i]) / 2 for i in (0, 2)]
            length = math.hypot(*mid)
            normal = (mid[0] / length, 0, mid[1] / length)
            normals += [normal] * 4
            for corner in quad:
                uvs.append((side / 4 + (0.25 if corner in (b, b + 4) else 0), positions[corner][1] / 200))
                uv_indices.append(len(uvs) - 1)
    # Weights: the bottom ring follows the shoulder, the top ring the elbow, the middle both.
    shoulder = {i: 1.0 for i in range(4)}
    shoulder.update({i: 0.5 for i in range(4, 8)})
    elbow = {i: 0.5 for i in range(4, 8)}
    elbow.update({i: 1.0 for i in range(8, 12)})
    objects = geometry(1001, "Arm", positions, polygons, normals, uvs, uv_indices)
    objects += model(2001, "Rig", "Null")
    objects += '\tNodeAttribute: 3001, "NodeAttribute::Rig", "Null" {\n\t\tTypeFlags: "Null"\n\t}\n'
    objects += model(2002, "Arm", "Mesh")
    objects += model(2003, "Shoulder", "LimbNode")
    objects += model(2004, "Elbow", "LimbNode", translation=(0, 100, 0))
    for uid, name in ((3003, "Shoulder"), (3004, "Elbow")):
        objects += '\tNodeAttribute: %d, "NodeAttribute::%s", "LimbNode" {\n\t\tTypeFlags: "Skeleton"\n\t}\n' % (uid, name)
    objects += material(4001, "Skin", (0.9, 0.7, 0.6))
    objects += '\tDeformer: 7001, "Deformer::Skin", "Skin" {\n\t\tVersion: 101\n\t\tLink_DeformAcuracy: 50\n\t}\n'
    for uid, name, weights, y in ((7002, "Shoulder", shoulder, 0), (7003, "Elbow", elbow, 100)):
        indices = sorted(weights)
        objects += '\tDeformer: %d, "SubDeformer::%s", "Cluster" {\n\t\tVersion: 100\n\t\tUserData: "", ""\n' % (uid, name)
        objects += array("Indexes", indices, "\t\t")
        objects += array("Weights", [float(weights[i]) for i in indices], "\t\t")
        objects += array("Transform", matrix_translate(0, -y, 0), "\t\t")  # the mesh in the joint's space
        objects += array("TransformLink", matrix_translate(0, y, 0), "\t\t")  # the joint in the world
        objects += "\t}\n"
    objects += """\tPose: 8001, "Pose::BIND_POSES", "BindPose" {
\t\tType: "BindPose"
\t\tVersion: 100
\t\tNbPoseNodes: 3
"""
    for uid, y in ((2002, 0), (2003, 0), (2004, 100)):
        objects += "\t\tPoseNode:  {\n\t\t\tNode: %d\n" % uid
        objects += array("Matrix", matrix_translate(0, y, 0), "\t\t\t")
        objects += "\t\t}\n"
    objects += "\t}\n"
    # The take: Elbow's Z rotation 0, 45, 0 degrees at 0, 0.5 and 1 seconds.
    objects += """\tAnimationStack: 9001, "AnimStack::Wave", "" {
\t\tProperties70:  {
\t\t\tP: "LocalStop", "KTime", "Time", "",%d
\t\t\tP: "ReferenceStop", "KTime", "Time", "",%d
\t\t}
\t}
\tAnimationLayer: 9002, "AnimLayer::BaseLayer", "" {
\t}
\tAnimationCurveNode: 9003, "AnimCurveNode::R", "" {
\t\tProperties70:  {
\t\t\tP: "d|X", "Number", "", "A",0
\t\t\tP: "d|Y", "Number", "", "A",0
\t\t\tP: "d|Z", "Number", "", "A",0
\t\t}
\t}
""" % (SECOND, SECOND)
    for uid, values in ((9004, [0.0, 0.0, 0.0]), (9005, [0.0, 0.0, 0.0]), (9006, [0.0, 45.0, 0.0])):
        objects += '\tAnimationCurve: %d, "AnimCurve::", "" {\n\t\tDefault: 0\n\t\tKeyVer: 4009\n' % uid
        objects += array("KeyTime", [0, SECOND // 2, SECOND], "\t\t")
        objects += array("KeyValueFloat", values, "\t\t")
        objects += array("KeyAttrFlags", [24840], "\t\t")  # linear interpolation
        objects += array("KeyAttrDataFloat", [0, 0, 255790911, 0], "\t\t")
        objects += array("KeyAttrRefCount", [3], "\t\t")
        objects += "\t}\n"
    write("arm.fbx", objects, [
        ("OO", 2001, 0), ("OO", 3001, 2001), ("OO", 2002, 2001), ("OO", 2003, 2001), ("OO", 2004, 2003),
        ("OO", 3003, 2003), ("OO", 3004, 2004), ("OO", 1001, 2002), ("OO", 4001, 2002),
        ("OO", 7001, 1001), ("OO", 7002, 7001), ("OO", 7003, 7001), ("OO", 2003, 7002), ("OO", 2004, 7003),
        ("OO", 9002, 9001), ("OO", 9003, 9002), ("OP", 9003, 2004, "Lcl Rotation"),
        ("OP", 9004, 9003, "d|X"), ("OP", 9005, 9003, "d|Y"), ("OP", 9006, 9003, "d|Z")])


if __name__ == "__main__":
    crate()
    arm()
