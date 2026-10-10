"""Regenerate the small, self-authored model fixtures and material demo (stdlib only)."""
import base64, copy, json, struct, zlib
from pathlib import Path
root=Path(__file__).resolve().parent

def png():
 def chunk(kind, data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
 return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',2,2,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(b'\0'+bytes([255,255,255,255,30,30,30,255])+b'\0'+bytes([30,30,30,255,255,255,255,255])))+chunk(b'IEND',b'')
pixels=png();(root/'checker.png').write_bytes(pixels)

def model(vertices, indices, colors, translations, texture=True):
 blob=struct.pack('<'+'f'*len(vertices),*vertices)+struct.pack('<'+'H'*len(indices),*indices)
 while len(blob)%4:blob+=b'\0'
 doc={'asset':{'version':'2.0','generator':'Shockwave self-authored fixtures'},'buffers':[{'uri':'parts.bin','byteLength':len(blob)}],
 'bufferViews':[{'buffer':0,'byteOffset':0,'byteLength':len(vertices)*4,'target':34962},{'buffer':0,'byteOffset':len(vertices)*4,'byteLength':len(indices)*2,'target':34963}],
 'accessors':[{'bufferView':0,'componentType':5126,'count':len(vertices)//3,'type':'VEC3','min':[min(vertices[i::3]) for i in range(3)],'max':[max(vertices[i::3]) for i in range(3)]},{'bufferView':1,'componentType':5123,'count':len(indices),'type':'SCALAR'}],
 'materials':[{'name':name,'pbrMetallicRoughness':{'baseColorFactor':color,'metallicFactor':0,'roughnessFactor':.8}} for name,color in colors],
 'meshes':[{'name':f'Part{i}','primitives':[{'attributes':{'POSITION':0},'indices':1,'material':i}]} for i in range(len(colors))],
 'nodes':[{'name':'Assembly','translation':[2,0,0],'children':list(range(1,len(translations)+1))}]+[{'name':f'Instance{i}','mesh':i%len(colors),'translation':t} for i,t in enumerate(translations)],'scenes':[{'nodes':[0]}],'scene':0}
 doc['materials'][1].update(alphaMode='MASK',alphaCutoff=.4,doubleSided=True)
 if len(colors)>2:doc['materials'][2].update(alphaMode='BLEND',doubleSided=True)
 if texture:
  uvs=[0,0,1,0,0,1] if len(vertices)==9 else [0,0]*(len(vertices)//3)
  offset=len(blob);blob+=struct.pack('<'+'f'*len(uvs),*uvs)
  doc['bufferViews'].append({'buffer':0,'byteOffset':offset,'byteLength':len(uvs)*4,'target':34962})
  doc['accessors'].append({'bufferView':2,'componentType':5126,'count':len(uvs)//2,'type':'VEC2'})
  for mesh in doc['meshes']:mesh['primitives'][0]['attributes']['TEXCOORD_0']=2
  doc['images']=[{'uri':'checker.png'}];doc['textures']=[{'source':0}]
  doc['materials'][0]['pbrMetallicRoughness']['baseColorTexture']={'index':0}
  doc['buffers'][0]['byteLength']=len(blob)
 return doc,blob

def glb(doc,blob):
 d=copy.deepcopy(doc);d['buffers'][0].pop('uri',None)
 if 'images' in d:
  offset=len(blob);blob+=pixels
  d['bufferViews'].append({'buffer':0,'byteOffset':offset,'byteLength':len(pixels)})
  d['images']=[{'bufferView':len(d['bufferViews'])-1,'mimeType':'image/png'}]
  d['buffers'][0]['byteLength']=len(blob)
 encoded=json.dumps(d,separators=(',',':')).encode()
 encoded+=b' '*((-len(encoded))%4);blob+=b'\0'*((-len(blob))%4)
 return struct.pack('<III',0x46546c67,2,28+len(encoded)+len(blob))+struct.pack('<I4s',len(encoded),b'JSON')+encoded+struct.pack('<I4s',len(blob),b'BIN\0')+blob
colors=[('Paint red',[1,.1,.1,1]),('Cutout blue',[.1,.3,1,1]),('Glass green',[.1,1,.3,.4])]
doc,blob=model([0,0,0,1,0,0,0,1,0],[0,1,2],colors,[[0,0,0],[0,0,1],[2,0,0],[0,2,0]])
(root/'parts.gltf').write_text(json.dumps(doc,indent=2)+'\n');(root/'parts.bin').write_bytes(blob);(root/'parts.glb').write_bytes(glb(doc,blob))
(root/'quad.obj').write_text('mtllib quad.mtl\no Quad\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nusemtl Paint\nf 1/1 2/2 3/3 4/4\n')
(root/'quad.mtl').write_text('newmtl Paint\nKd 0.25 0.5 0.75\nmap_Kd checker.png\n')
# Self-authored three-part reusable model to try in the browser.
v=[-.5,-.5,-.5,.5,-.5,-.5,.5,.5,-.5,-.5,.5,-.5,-.5,-.5,.5,.5,-.5,.5,.5,.5,.5,-.5,.5,.5]
i=[0,2,1,0,3,2,4,5,6,4,6,7,0,1,5,0,5,4,3,7,6,3,6,2,0,4,7,0,7,3,1,2,6,1,6,5]
d,b=model(v,i,colors,[[-1.3,.5,0],[0,.5,0],[1.3,.5,0]],texture=False)
d['nodes'][0]['translation']=[0,0,0]
d['nodes'][3]['scale']=[-1,1,1]
(root.parents[2]/'assets/models/material-demo.glb').write_bytes(glb(d,b))
