"""
Export the Night Fury model by Fulonimation Studios (https://ko-fi.com/fulonimationstudios)
for the game: one skinned mesh, the whole armature, the rig's constraints and the poses
its action constraints read, and the textures the game's materials use.

Run headless (Blender 4.5):
  /Applications/Blender.app/Contents/MacOS/Blender -b <Toothless_AutomaticTailfin.blend> \
      -P Tools/DragonModel/export_fulo.py -- <out dir>

Writes into <out dir>:
  SK_DragonFulo.glb   mesh + skin (all 718 bones, rest pose), no images, material slots named
  FuloRig.json        bones (rest matrices in armature space, parents, inherit flags),
                      constraints in evaluation order, and every action an ACTION constraint
                      reads, sampled per frame as pose-bone basis (loc, quat wxyz, scale)
  Textures/*.png      the maps the game's materials use, sized for real time

The game evaluates this rig itself (UDragonFuloRigComponent): its own motion drives the
rig's controls (wing fold, wing/tail/head/body controls, foot IK targets ...) and the
constraints do the rest exactly as they do in Blender.
"""
import bpy, json, math, os, sys
from mathutils import Matrix

argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
OUT = os.path.abspath(argv[0] if argv else bpy.path.abspath('//fulo_export'))
os.makedirs(os.path.join(OUT, 'Textures'), exist_ok=True)

arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE')
FIN_KEEP = 'Fin_black'           # every fin variant shares this geometry; the game swaps its texture

# ---------------------------------------------------------------- rig data
def mat(m):
    return [round(v, 6) for row in m for v in row]

rig = {'source': os.path.basename(bpy.data.filepath), 'credit': 'Fulonimation Studios', 'bones': [], 'actions': {},
       'object_scale': arm.matrix_world.to_scale()[0]}   # constraint distances (stretch rest lengths) are in world units
pose_now = {}
for pb in arm.pose.bones:
    pose_now[pb.name] = (tuple(pb.location), tuple(pb.rotation_quaternion) if pb.rotation_mode == 'QUATERNION' else tuple(pb.rotation_euler.to_quaternion()), tuple(pb.scale))

used_actions = set()
for b in arm.data.bones:
    pb = arm.pose.bones[b.name]
    cons = []
    for c in pb.constraints:
        d = {'type': c.type, 'influence': c.influence, 'mute': c.mute}
        for k in ('subtarget', 'owner_space', 'target_space', 'mix_mode', 'use_x', 'use_y', 'use_z', 'invert_x', 'invert_y', 'invert_z',
                  'transform_channel', 'min', 'max', 'frame_start', 'frame_end', 'head_tail', 'use_bbone_shape', 'rest_length', 'bulge',
                  'volume', 'keep_axis', 'track_axis', 'chain_count', 'pole_subtarget', 'pole_angle', 'use_tail', 'use_stretch',
                  'iterations', 'weight', 'use_rotation', 'orient_weight', 'euler_order', 'use_offset', 'use_eval_time', 'eval_time'):
            if hasattr(c, k):
                v = getattr(c, k)
                d[k] = v if isinstance(v, (int, float, str, bool)) or v is None else str(v)
        d['target'] = c.target.name if getattr(c, 'target', None) else None
        if getattr(c, 'pole_target', None): d['pole_target'] = c.pole_target.name
        if c.type == 'ACTION' and c.action:
            d['action'] = c.action.name
            used_actions.add(c.action.name)
        cons.append(d)
    rig['bones'].append({
        'name': b.name, 'parent': b.parent.name if b.parent else None, 'deform': b.use_deform,
        'rest': mat(b.matrix_local),                       # armature space, Blender convention (Y along the bone)
        'length': b.length,
        'connect': b.use_connect, 'inherit_rotation': b.use_inherit_rotation, 'inherit_scale': b.inherit_scale,
        'local_location': b.use_local_location, 'rotation_mode': pb.rotation_mode,
        'pose_loc': pose_now[b.name][0], 'pose_rot': pose_now[b.name][1], 'pose_scale': pose_now[b.name][2],
        'cons': cons,
    })

# actions read by ACTION constraints: per frame, per bone basis
saved = arm.animation_data.action if arm.animation_data else None
if not arm.animation_data: arm.animation_data_create()
for an in sorted(used_actions):
    a = bpy.data.actions[an]
    bones = sorted({fc.data_path.split('"')[1] for fc in a.fcurves if fc.data_path.startswith('pose.bones["')})
    f0, f1 = int(math.floor(a.frame_range[0])), int(math.ceil(a.frame_range[1]))
    frames = {}
    for bn in bones:
        pb = arm.pose.bones.get(bn)
        if not pb: continue
        series = []
        for f in range(f0, f1 + 1):
            vals = {}
            for fc in a.fcurves:
                if fc.data_path.startswith(f'pose.bones["{bn}"].'):
                    vals[(fc.data_path.rsplit('.', 1)[1], fc.array_index)] = fc.evaluate(f)
            loc = [vals.get(('location', i), 0.0) for i in range(3)]
            if any(k[0] == 'rotation_quaternion' for k in vals):
                q = [vals.get(('rotation_quaternion', i), 1.0 if i == 0 else 0.0) for i in range(4)]
            elif any(k[0] == 'rotation_euler' for k in vals):
                from mathutils import Euler
                e = Euler([vals.get(('rotation_euler', i), 0.0) for i in range(3)], pb.rotation_mode if pb.rotation_mode not in ('QUATERNION', 'AXIS_ANGLE') else 'XYZ')
                q = list(e.to_quaternion())
            else:
                q = [1.0, 0.0, 0.0, 0.0]
            s = [vals.get(('scale', i), 1.0) for i in range(3)]
            series.append([round(v, 6) for v in loc + q + s])
        frames[bn] = series
    rig['actions'][an] = {'frame_start': f0, 'frame_end': f1, 'bones': frames}
json.dump(rig, open(os.path.join(OUT, 'FuloRig.json'), 'w'))
print('rig: %d bones, actions %s' % (len(rig['bones']), {k: len(v['bones']) for k, v in rig['actions'].items()}))

# ---------------------------------------------------------------- mesh
# Rest pose for the bind; drop the alternative fins, the boolean cutter and the clear corneas
# (the eye material carries the gloss).
arm.animation_data.action = None
for pb in arm.pose.bones:
    pb.location = (0, 0, 0); pb.rotation_quaternion = (1, 0, 0, 0); pb.rotation_euler = (0, 0, 0); pb.scale = (1, 1, 1)
bpy.context.view_layer.update()
drop = {o.name for o in bpy.data.objects if o.type == 'MESH' and (
    (o.name.startswith('Fin_') and o.name != FIN_KEEP) or o.name in ('Boolean', 'GlassEyes'))}
meshes = [o for o in bpy.data.objects if o.type == 'MESH' and o.name not in drop]
bpy.ops.object.select_all(action='DESELECT')
for o in meshes:
    o.hide_set(False); o.hide_viewport = False; o.hide_render = False
bpy.context.view_layer.objects.active = meshes[0]
# Apply everything but the armature (mirror, boolean, corrective smooth), unparent keeping the
# transform, then join into one skinned object.
for o in meshes:
    if o.data.shape_keys:
        # Toothless keeps his teeth retracted (the Teeth "Key 1", driven by Retract.1); the eyes'
        # pupil keys stay at rest. Bake that mix into the mesh.
        for kb in o.data.shape_keys.key_blocks:
            kb.value = 1.0 if (o.name == 'Teeth' and kb.name == 'Key 1') else 0.0
        if o.data.shape_keys.animation_data:
            o.data.shape_keys.animation_data_clear()
        bpy.context.view_layer.objects.active = o
        o.select_set(True)
        bpy.ops.object.shape_key_remove(all=True, apply_mix=True)
        o.select_set(False)
    o.data = o.data.copy()   # some meshes share data with dropped objects
    bpy.context.view_layer.objects.active = o
    o.select_set(True)
    for m in list(o.modifiers):
        if m.type == 'ARMATURE':
            continue
        try:
            bpy.ops.object.modifier_apply(modifier=m.name)
        except Exception as e:
            print('! could not apply', o.name, m.name, e); o.modifiers.remove(m)
    o.select_set(False)
for o in meshes:
    mw = o.matrix_world.copy(); o.parent = None; o.matrix_world = mw
    # keep exactly one armature modifier, on the rig
    arms = [m for m in o.modifiers if m.type == 'ARMATURE']
    for m in arms[1:]: o.modifiers.remove(m)
    if not arms:
        m = o.modifiers.new('Armature', 'ARMATURE')
    o.modifiers[[m.type for m in o.modifiers].index('ARMATURE')].object = arm
    # meshes rigidly following one bone through object parenting get a full-weight group
    if not o.vertex_groups:
        print('! no vertex groups on', o.name)
bpy.ops.object.select_all(action='DESELECT')
body = bpy.data.objects['Body']
for o in meshes: o.select_set(True)
bpy.context.view_layer.objects.active = body
bpy.ops.object.join()
body.name = 'DragonFulo'
for slot in body.material_slots:
    if slot.material and slot.material.name == bpy.data.objects.get(FIN_KEEP, body).name: pass
# name the fin slot plainly
for slot in body.material_slots:
    if slot.material and slot.material.name == 'BlackFin': slot.material.name = 'Fin'
print('materials:', [s.material.name for s in body.material_slots if s.material])
print('verts:', len(body.data.vertices), 'groups:', len(body.vertex_groups))
# nothing else goes in the file (bone widgets, cutters, the other fins)
for o in list(bpy.data.objects):
    if o not in (body, arm):
        bpy.data.objects.remove(o, do_unlink=True)
bpy.ops.object.select_all(action='DESELECT')
body.select_set(True); arm.select_set(True)
bpy.context.view_layer.objects.active = arm
bpy.ops.export_scene.gltf(filepath=os.path.join(OUT, 'SK_DragonFulo.glb'), export_format='GLB', use_selection=True,
                          export_skins=True, export_def_bones=False, export_animations=False, export_apply=False,
                          export_image_format='NONE', export_yup=True, export_morph=False)

# ---------------------------------------------------------------- textures
def save(name, out, size=None, mode=None):
    im = bpy.data.images.get(name)
    if im is None:
        print('! no image', name); return
    im2 = im.copy()
    if size and (im2.size[0] > size):
        im2.scale(size, size)
    im2.filepath_raw = os.path.join(OUT, 'Textures', out)
    im2.file_format = 'PNG'
    im2.save()
    print('tex', out, tuple(im2.size))

TEX = [
    ('NightFury_Patterns2K', 'T_Fulo_Body_D.png', None),
    ('NightFury_Normal8K.png', 'T_Fulo_Body_N.png', 4096),
    ('ToothlessCavity8K_nofin.png', 'T_Fulo_Body_Cavity.png', 4096),
    ('AlphaGlow4K.png', 'T_Fulo_Body_Glow.png', 2048),
    ('NightFury_Eye2.png', 'T_Fulo_Eye_D.png', 1024),
    ('NightFury_MouthColor1K', 'T_Fulo_Mouth_D.png', None),
    ('NightFury_MouthNormal4K', 'T_Fulo_Mouth_N.png', 2048),
    ('NightFury_MouthCavity4K', 'T_Fulo_Mouth_Cavity.png', 2048),
    ('NightFury_TeethColor1K', 'T_Fulo_Teeth_D.png', None),
    ('NightFury_TongueNormal2K', 'T_Fulo_Tongue_N.png', None),
    ('NightFury_TongueCavity2K', 'T_Fulo_Tongue_Cavity.png', None),
    ('Toothless_TailSubsurf', 'T_Fulo_Fin_Subsurf.png', None),
    ('leather_red_02_nor_gl_4k.exr', 'T_Fulo_Leather_N.png', 2048),
    ('leather_red_02_rough_4k.exr', 'T_Fulo_Leather_R.png', 2048),
    ('rusty_metal_03_nor_gl_4k.exr', 'T_Fulo_Metal_N.png', 2048),
    ('rusty_metal_03_rough_4k.exr', 'T_Fulo_Metal_R.png', 2048),
]
FINS = [('Toothless_FinBlack', 'Black'), ('Toothless_httyd1Fin', 'Red'), ('Toothless_BrownFin', 'Brown'), ('Toothless_finblue', 'Blue'),
        ('Toothless_finpurple', 'Purple'), ('Toothless_GreenFlamesFin', 'GreenFlames'), ('Toothless_Fin(StrikeClass)', 'StrikeClass')]
for src, out, size in TEX:
    save(src, out, size)
for src, nm in FINS:
    save(src, 'T_Fulo_Fin_%s.png' % nm, None)
print('EXPORT DONE', OUT)
