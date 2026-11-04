# Regenerates Source/DragonFlight/Private/DragonWebRigData.inl from the web game.
# python3 Tools/gen_web_rig_data.py <web>/assets/models/dragon_rigged_hd.glb <web>/js/foldpose.js Source/DragonFlight/Private/DragonWebRigData.inl
import json,struct,sys,re
d=open(sys.argv[1],'rb').read()
l=struct.unpack('<I',d[12:16])[0]
g=json.loads(d[20:20+l])
N=g['nodes']; joints=set(g['skins'][0]['joints'])
root=[i for i in joints if N[i]['name']=='Root'][0]
order=[]
def dfs(i):
  order.append(i)
  for c in N[i].get('children',[]):
    if c in joints: dfs(c)
dfs(root)
assert len(order)==152
idx={j:k for k,j in enumerate(order)}
par={}
for i in order:
  for c in N[i].get('children',[]): par[c]=i
fold=json.loads(re.search(r'FOLD_POSE = (\{.*\});',open(sys.argv[2]).read(),re.S).group(1))
out=[]
out.append('// GENERATED from assets/models/dragon_rigged_hd.glb and js/foldpose.js by Tools/gen_web_rig_data.py.')
out.append('// glTF frame: Y-up, metres, nose along -Z, .L on -X. Parents precede children.')
out.append('#pragma once')
out.append('struct FDragonWebBoneDef { const char* Name; int Parent; float T[3]; float R[4]; float S; };')
out.append('static const FDragonWebBoneDef GDragonWebBones[] = {')
smax=0
for i in order:
  n=N[i]; t=n.get('translation',[0,0,0]); r=n.get('rotation',[0,0,0,1]); s=n.get('scale',[1,1,1])
  smax=max(smax,max(abs(x-s[0]) for x in s))
  p=idx[par[i]] if par.get(i) in idx else -1
  def f(v):
    s='%.7g'%v
    return (s if ('.' in s or 'e' in s) else s+'.0')+'f'
  out.append('\t{ "%s", %d, { %s }, { %s }, %s },'%(n['name'],p,', '.join(map(f,t)),', '.join(map(f,r)),f(s[0])))
out.append('};')
out.append('struct FDragonWebFoldDef { const char* Name; float Q[4]; };')
out.append('/** js/foldpose.js FOLD_POSE: the solved folded-wing local rotation per wing bone (x, y, z, w), glTF frame. */')
out.append('static const FDragonWebFoldDef GDragonWebFold[] = {')
for k,q in fold.items():
  out.append('\t{ "%s", { %s } },'%(k,', '.join(f(v) for v in q)))
out.append('};')
open(sys.argv[3],'w').write('\n'.join(out)+'\n')
print('nonuniform scale max dev',smax, 'fold',len(fold))
