"""
Procedural, original rigged dragon for Dragon Flight (UE5).

Run headless (Blender 4.5 LTS):
  /Applications/Blender.app/Contents/MacOS/Blender -b -P Tools/DragonModel/build_dragon.py -- [options]

Options (after the "--"):
  --quick              geometry + rig + previews only (no texture bake, no export)
  --tex N              bake resolution (default 2048)
  --preview-dir PATH   where preview PNGs go (default Data/Dragon/Previews)
  --samples N          Cycles samples for previews (default 32)
  --no-render          skip preview renders

Outputs (relative to the repo root):
  Data/Dragon/SK_Dragon.fbx, Data/Dragon/SK_Dragon.blend,
  Data/Dragon/Textures/T_Dragon_{Body,Wing}_{D,N,ARM}.png

Conventions: built in metres, Blender character convention (faces -Y, up +Z,
left side on +X).  The FBX export is rotated +90 deg about Z so the dragon
faces UE +X (left wing on UE -Y), and baked to centimetres.
"""
import bpy, bmesh, math, sys, os, time
import numpy as np
from mathutils import Vector, Matrix, Quaternion
from mathutils.bvhtree import BVHTree

T0 = time.time()
ARGV = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []


def opt(name, default=None, cast=str):
    if name in ARGV:
        i = ARGV.index(name)
        if i + 1 < len(ARGV) and not ARGV[i + 1].startswith('--'):
            return cast(ARGV[i + 1])
        return True
    return default


QUICK = bool(opt('--quick', False))
TEXRES = opt('--tex', 2048, int)
SAMPLES = opt('--samples', 32, int)
NO_RENDER = bool(opt('--no-render', False))
REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
OUT = os.path.join(REPO, 'Data', 'Dragon')
TEXDIR = os.path.join(OUT, 'Textures')
PREV = opt('--preview-dir', os.path.join(OUT, 'Previews'))
for d in (OUT, TEXDIR, PREV):
    os.makedirs(d, exist_ok=True)


def log(*a):
    print('[dragon %6.1fs]' % (time.time() - T0), *a, flush=True)


V = lambda *a: Vector(a if len(a) == 3 else a[0])
TARGET_LEN = 8.6     # nose to tail tip, metres
TARGET_SPAN = 15.1   # wingtip to wingtip, metres

# ---------------------------------------------------------------------------
# Scene reset
# ---------------------------------------------------------------------------
bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.unit_settings.system = 'METRIC'
scene.unit_settings.scale_length = 1.0
COL = scene.collection


def link(ob):
    COL.objects.link(ob)
    return ob


def mesh_obj(name, verts, faces):
    me = bpy.data.meshes.new(name)
    me.from_pydata([tuple(v) for v in verts], [], faces)
    me.validate()
    me.update()
    return link(bpy.data.objects.new(name, me))


def apply_mods(ob):
    dg = bpy.context.evaluated_depsgraph_get()
    me = bpy.data.meshes.new_from_object(ob.evaluated_get(dg))
    old = ob.data
    ob.modifiers.clear()
    ob.data = me
    bpy.data.meshes.remove(old)


def recalc_normals(ob, merge=0.0):
    bm = bmesh.new()
    bm.from_mesh(ob.data)
    if merge > 0:
        bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=merge)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(ob.data)
    bm.free()
    ob.data.update()


def bvh_of(ob):
    bm = bmesh.new()
    bm.from_mesh(ob.data)
    bm.transform(ob.matrix_world)
    t = BVHTree.FromBMesh(bm)
    bm.free()
    return t


# ---------------------------------------------------------------------------
# Skeleton landmarks (metres; nose toward -Y, left = +X).  Everything below is
# generated from these so mesh and bones always agree.
# ---------------------------------------------------------------------------
J = {}
J['pelvis'] = V(0, 0.95, 1.60)
J['spine_01'] = V(0, 0.55, 1.65)
J['spine_02'] = V(0, 0.13, 1.68)
J['spine_03'] = V(0, -0.30, 1.69)
J['chest'] = V(0, -0.72, 1.68)
J['neck_01'] = V(0, -1.22, 1.72)
J['neck_02'] = V(0, -1.57, 1.97)
J['neck_03'] = V(0, -1.89, 2.22)
J['neck_04'] = V(0, -2.17, 2.43)
J['head'] = V(0, -2.43, 2.57)
J['head_end'] = V(0, -3.36, 2.42)
J['jaw'] = V(0, -2.56, 2.45)
J['jaw_end'] = V(0, -3.33, 2.33)

# tail: smooth path, 12 bones, segment length shrinking toward the tip
TAIL_CTRL = [  # (y, z, half-width, half-height)
    (0.95, 1.54, 0.38, 0.42),
    (1.55, 1.43, 0.29, 0.32),
    (2.20, 1.29, 0.20, 0.225),
    (2.90, 1.14, 0.13, 0.15),
    (3.60, 1.04, 0.095, 0.108),
    (4.30, 0.99, 0.065, 0.072),
    (4.85, 1.00, 0.043, 0.046),
    (5.22, 1.02, 0.024, 0.026),
]

# legs (left side; right side mirrors x)
L = {}
L['shoulder'] = V(0.40, -1.00, 1.38)
L['elbow'] = V(0.46, -0.80, 0.82)
L['wrist'] = V(0.43, -1.04, 0.24)
L['palm'] = V(0.43, -1.19, 0.075)
L['hip'] = V(0.37, 0.92, 1.38)
L['knee'] = V(0.50, 0.52, 0.86)
L['ankle'] = V(0.47, 1.05, 0.36)
L['ball'] = V(0.47, 0.83, 0.075)
# wing (left)
W = {}
W['shoulder'] = V(0.26, -0.95, 1.96)
W['elbow'] = V(1.50, -0.52, 2.12)
W['wrist'] = V(3.50, -1.06, 2.22)
W['knuckle'] = V(3.76, -0.97, 2.22)
W['tip1'] = V(7.689, 0.14, 2.20)
W['tip2'] = V(6.62, 1.76, 2.15)
W['tip3'] = V(5.10, 2.74, 2.10)
W['tip4'] = V(3.25, 2.80, 2.04)
W['attach'] = V(0.30, 0.82, 1.80)   # trailing root of the membrane on the flank


def mx(v, side):
    return Vector((v.x * side, v.y, v.z))


def catmull(pts, n):
    """Centripetal-ish Catmull-Rom through pts (list of Vector/np arrays), n samples."""
    P = [np.array(p, float) for p in pts]
    P = [2 * P[0] - P[1]] + P + [2 * P[-1] - P[-2]]
    out = []
    segs = len(P) - 3
    for i in range(n):
        u = i / (n - 1) * segs
        k = min(int(u), segs - 1)
        t = u - k
        p0, p1, p2, p3 = P[k], P[k + 1], P[k + 2], P[k + 3]
        out.append(0.5 * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t
                          + (-p0 + 3 * p1 - 3 * p2 + p3) * t ** 3))
    return np.array(out)


def resample_arclen(path, n):
    path = np.asarray(path, float)
    seg = np.linalg.norm(np.diff(path, axis=0), axis=1)
    s = np.concatenate([[0], np.cumsum(seg)])
    t = np.linspace(0, s[-1], n)
    return np.stack([np.interp(t, s, path[:, k]) for k in range(path.shape[1])], 1), s[-1]


# Tail joints: dense centreline then split into 12 shrinking segments
_tail_dense = catmull([(0, y, z, w, h) for (y, z, w, h) in TAIL_CTRL], 400)
_tail_dense, TAIL_LEN = resample_arclen(_tail_dense, 2000)
_ratio = 0.9
_lens = np.array([_ratio ** i for i in range(12)])
_cum = np.concatenate([[0], np.cumsum(_lens)]) / _lens.sum()
for i in range(13):
    idx = min(int(round(_cum[i] * (len(_tail_dense) - 1))), len(_tail_dense) - 1)
    p = _tail_dense[idx]
    J['tail_%02d' % (i + 1) if i < 12 else 'tail_end'] = V(p[0], p[1], p[2])

# ---------------------------------------------------------------------------
# Metaball body
# ---------------------------------------------------------------------------
THRESH = 0.6


def single_ratio(s):
    return math.sqrt(1 - (THRESH / s) ** (1 / 3))


_chain_cache = {}


def chain_ratio(s, c):
    key = (s, c)
    if key in _chain_cache:
        return _chain_cache[key]
    ks = np.arange(-80, 81) * c
    lo, hi = 0.0, 1.0
    for _ in range(60):
        d = 0.5 * (lo + hi)
        f = np.clip(1 - (d * d + ks * ks), 0, None)
        if (s * f ** 3).sum() > THRESH:
            lo = d
        else:
            hi = d
    _chain_cache[key] = lo
    return lo


def frame_from(T, upref=(0, 0, 1)):
    T = Vector(T).normalized()
    up = Vector(upref)
    if abs(T.dot(up)) > 0.95:
        up = Vector((0, 1, 0)) if abs(T.y) < 0.9 else Vector((1, 0, 0))
    Lt = T.cross(up).normalized()
    U = T.cross(Lt).normalized()
    return Matrix((T, Lt, U)).transposed()  # columns: local x,y,z


def mb_ell(mb, center, half, rot=None, s=4.0, neg=False):
    e = mb.elements.new(type='ELLIPSOID')
    e.co = Vector(center)
    e.radius = 1.0 / single_ratio(s)
    e.size_x, e.size_y, e.size_z = half
    e.stiffness = s
    e.use_negative = neg
    if rot is not None:
        e.rotation = rot.to_quaternion()
    return e


def mb_chain(mb, pts, ws, hs, s=4.0, c=0.3, upref=(0, 0, 1), smooth=True):
    """Tapered elliptical tube of metaball ellipsoids along a path."""
    data = [(p[0], p[1], p[2], w, h) for p, w, h in zip(pts, ws, hs)]
    if len(pts) == 2:
        dense = np.array([np.array(data[0]) * (1 - t) + np.array(data[1]) * t for t in np.linspace(0, 1, 200)])
    else:
        dense = catmull(data, 200)
    dense, length = resample_arclen(dense, 600)
    seg = np.linalg.norm(np.diff(dense[:, :3], axis=0), axis=1)
    sacc = np.concatenate([[0], np.cumsum(seg)])
    cr = chain_ratio(s, c)
    pos = 0.0
    n = 0
    while pos <= length + 1e-6:
        i = min(np.searchsorted(sacc, pos), len(dense) - 1)
        p = dense[i]
        i0, i1 = max(i - 3, 0), min(i + 3, len(dense) - 1)
        T = dense[i1, :3] - dense[i0, :3]
        w, h = p[3], p[4]
        R = math.sqrt(w * h)
        e = mb.elements.new(type='ELLIPSOID')
        e.co = Vector(p[:3])
        e.radius = R / cr
        e.size_x = 1.0
        e.size_y = w / R
        e.size_z = h / R
        e.stiffness = s
        e.rotation = frame_from(T, upref).to_quaternion()
        n += 1
        pos += c * R / cr
    return n


def build_body():
    mb = bpy.data.metaballs.new('MBBody')
    mb.resolution = 0.016
    mb.render_resolution = 0.016
    mb.threshold = THRESH
    ob = link(bpy.data.objects.new('MBBody', mb))

    # Torso: deep keeled ribcage, tucked waist, heavy hips (croc / monitor / big-cat references)
    torso = [  # y, z, W, H
        (1.25, 1.50, 0.36, 0.38),
        (0.85, 1.50, 0.43, 0.43),
        (0.45, 1.48, 0.44, 0.45),
        (-0.05, 1.44, 0.50, 0.55),
        (-0.55, 1.40, 0.54, 0.63),
        (-0.98, 1.44, 0.52, 0.61),
        (-1.32, 1.60, 0.42, 0.48),
    ]
    mb_chain(mb, [V(0, y, z) for y, z, w, h in torso], [t[2] for t in torso], [t[3] for t in torso], s=3.0)
    # sternal keel along the bottom of the chest
    mb_chain(mb, [V(0, -1.32, 1.06), V(0, -0.95, 0.86), V(0, -0.55, 0.80), V(0, -0.15, 0.88), V(0, 0.25, 1.05)],
             [0.13, 0.17, 0.18, 0.16, 0.12], [0.12, 0.14, 0.14, 0.12, 0.1], s=3.0)
    # epaxial muscles along the spine (top line) and glutes over the pelvis
    mb_chain(mb, [V(0, 1.0, 1.82), V(0, 0.3, 1.85), V(0, -0.5, 1.92), V(0, -1.1, 2.0)],
             [0.2, 0.22, 0.24, 0.22], [0.12, 0.13, 0.14, 0.13], s=3.0)
    for sd in (1, -1):
        mb_ell(mb, V(0.21 * sd, 0.95, 1.74), (0.32, 0.16, 0.14), frame_from((0, 1, -0.2)), s=3.0)
        # pectorals: sternum -> shoulder, big-cat style
        mb_ell(mb, V(0.27 * sd, -1.16, 1.22), (0.34, 0.17, 0.24), frame_from((0.35 * sd, 0.15, 0.6)), s=3.0)
        # wing anchor: deltoid/trapezius mass on top of the shoulders + supracoracoid bulk in front
        mb_ell(mb, V(0.24 * sd, -0.90, 1.97), (0.42, 0.22, 0.16), frame_from((0.75 * sd, 0.35, 0.15)), s=3.0)
        mb_ell(mb, V(0.36 * sd, -1.08, 1.66), (0.26, 0.18, 0.2), frame_from((0.4 * sd, -0.2, 0.6)), s=3.0)
        # ribcage flank fill
        mb_ell(mb, V(0.30 * sd, -0.45, 1.38), (0.55, 0.18, 0.42), frame_from((0, 1, 0)), s=3.0)

    # Neck: thick, with epaxial top muscle, side (sternomastoid-like) bands and a throat sloping into the chest
    neckpts = [V(0, -1.30, 1.76), J['neck_02'], J['neck_03'], J['neck_04'], J['head'] + V(0, -0.05, 0.0)]
    mb_chain(mb, neckpts, [0.37, 0.28, 0.21, 0.17, 0.15], [0.44, 0.33, 0.255, 0.205, 0.175], s=4.0)
    mb_chain(mb, [V(0, -1.25, 2.02), J['neck_02'] + V(0, 0.02, 0.16), J['neck_03'] + V(0, 0.02, 0.13),
                  J['neck_04'] + V(0, 0.03, 0.11), J['head'] + V(0, 0.06, 0.13)],
             [0.2, 0.15, 0.12, 0.1, 0.09], [0.1, 0.08, 0.07, 0.06, 0.05], s=3.5)
    for sd in (1, -1):
        mb_chain(mb, [V(0.12 * sd, -2.42, 2.40), V(0.15 * sd, -2.05, 2.08), V(0.17 * sd, -1.70, 1.72),
                      V(0.16 * sd, -1.38, 1.30)], [0.07, 0.085, 0.095, 0.1], [0.07, 0.085, 0.095, 0.1], s=3.0)
        # jowl under the jaw hinge
        mb_ell(mb, V(0.10 * sd, -2.50, 2.36), (0.13, 0.085, 0.085), frame_from((0, -1, -0.2)), s=3.5)
    mb_chain(mb, [V(0, -2.46, 2.33), V(0, -2.15, 2.12), V(0, -1.80, 1.75), V(0, -1.45, 1.30)],
             [0.11, 0.16, 0.21, 0.26], [0.09, 0.13, 0.17, 0.2], s=3.5)

    # Head: long cranium + narrow crocodilian snout (upper jaw only; lower jaw is separate)
    hd = J['head']
    mb_ell(mb, V(0, -2.60, 2.63), (0.24, 0.175, 0.16), frame_from((0, -1, -0.15)), s=4.0)
    mb_chain(mb, [V(0, -2.70, 2.60), V(0, -2.95, 2.56), V(0, -3.20, 2.51), V(0, -3.40, 2.475)],
             [0.135, 0.105, 0.082, 0.064], [0.115, 0.092, 0.074, 0.062], s=5.0)
    for sd in (1, -1):
        # heavy brow ridge over the eye, running back into the horn base
        mb_chain(mb, [V(0.10 * sd, -2.84, 2.685), V(0.125 * sd, -2.72, 2.715), V(0.12 * sd, -2.56, 2.72)],
                 [0.045, 0.06, 0.06], [0.028, 0.035, 0.035], s=2.6)
        # jaw muscle / cheek
        mb_ell(mb, V(0.135 * sd, -2.56, 2.53), (0.13, 0.065, 0.085), frame_from((0, -1, 0.1)), s=4.0)
        # nostril bosses and holes
        mb_ell(mb, V(0.042 * sd, -3.31, 2.52), (0.05, 0.03, 0.025), frame_from((0, -1, -0.2)), s=5.0)
        mb_ell(mb, V(0.05 * sd, -3.36, 2.535), (0.028, 0.017, 0.018), frame_from((0, -1, 0)), s=4.0, neg=True)
        # eye socket
        mb_ell(mb, V(0.175 * sd, -2.76, 2.665), (0.05, 0.04, 0.035), frame_from((0, -1, 0)), s=4.0, neg=True)
    # occipital crest
    mb_ell(mb, V(0, -2.45, 2.74), (0.14, 0.06, 0.07), frame_from((0, -1, 0.5)), s=4.0)
    # skin bosses the horns grow out of (no bare intersection seam)
    for sd in (1, -1):
        mb_ell(mb, V(0.10 * sd, -2.50, 2.74), (0.075, 0.07, 0.05), frame_from((0.3 * sd, 1, 0.3)), s=4.0)
        mb_ell(mb, V(0.165 * sd, -2.455, 2.58), (0.06, 0.055, 0.05), frame_from((0.4 * sd, 1, 0)), s=4.0)

    # Legs
    for sd in (1, -1):
        g = {k: mx(v, sd) for k, v in L.items()}
        S_, E_, W_, P_ = g['shoulder'], g['elbow'], g['wrist'], g['palm']
        H_, K_, A_, B_ = g['hip'], g['knee'], g['ankle'], g['ball']
        back = V(0, 1, 0)
        # scapula, upper arm with triceps (behind) and biceps (front)
        mb_ell(mb, mx(V(0.34, -0.98, 1.52), sd), (0.34, 0.14, 0.26), frame_from((0, 0.25, 1)), s=3.0)
        mb_chain(mb, [S_, E_], [0.24, 0.16], [0.25, 0.17], s=4.0)
        mb_ell(mb, S_.lerp(E_, 0.45) + back * 0.09 + mx(V(0.02, 0, 0), sd), (0.30, 0.12, 0.13), frame_from(E_ - S_), s=3.5)
        mb_ell(mb, S_.lerp(E_, 0.4) - back * 0.08, (0.22, 0.09, 0.1), frame_from(E_ - S_), s=3.5)
        # forearm: extensor/flexor bellies high, tendons low
        mb_chain(mb, [E_, W_], [0.155, 0.095], [0.165, 0.10], s=4.0)
        mb_ell(mb, E_.lerp(W_, 0.28) + back * 0.04, (0.24, 0.12, 0.125), frame_from(W_ - E_), s=3.5)
        mb_ell(mb, E_.lerp(W_, 0.25) - back * 0.05 + mx(V(0.03, 0, 0), sd), (0.18, 0.08, 0.08), frame_from(W_ - E_), s=3.5)
        for dx in (-0.03, 0.03):
            mb_chain(mb, [E_.lerp(W_, 0.55) - back * 0.06 + mx(V(dx, 0, 0), sd), W_ - back * 0.055 + mx(V(dx, 0, 0), sd)],
                     [0.022, 0.018], [0.022, 0.018], s=5.0)
        mb_ell(mb, W_, (0.09, 0.095, 0.085), s=4.5)
        mb_chain(mb, [W_, P_], [0.1, 0.105], [0.095, 0.072], s=4.0)
        for ang, ln in ((-0.45, 0.20), (0.0, 0.25), (0.45, 0.20)):
            d = Vector((math.sin(ang) * sd, -math.cos(ang), 0))
            tip = P_ + d * ln
            tip.z = 0.036
            mid = P_.lerp(tip, 0.55) + V(0, 0, 0.012)
            mb_chain(mb, [P_ + d * 0.02, mid, tip], [0.052, 0.042, 0.034], [0.044, 0.036, 0.028], s=5.0, smooth=False)
            mb_ell(mb, mid, (0.035, 0.034, 0.03), s=5.0)   # knuckle
        # hind: glute/thigh mass, quads (front), hamstrings (back), caudofemoral into the tail
        mb_ell(mb, mx(V(0.31, 0.80, 1.30), sd), (0.46, 0.22, 0.35), frame_from((0, -0.35, -1)), s=3.0)
        mb_chain(mb, [H_, K_], [0.27, 0.16], [0.27, 0.16], s=4.0)
        mb_ell(mb, H_.lerp(K_, 0.55) - back * 0.1 + mx(V(0.03, 0, 0), sd), (0.30, 0.14, 0.15), frame_from(K_ - H_), s=3.5)
        mb_ell(mb, H_.lerp(K_, 0.4) + back * 0.15, (0.32, 0.13, 0.15), frame_from(K_ - H_ + back * 0.2), s=3.5)
        mb_chain(mb, [mx(V(0.26, 0.85, 1.22), sd), mx(V(0.18, 1.4, 1.30), sd), mx(V(0.10, 1.95, 1.26), sd)],
                 [0.17, 0.13, 0.08], [0.17, 0.13, 0.08], s=3.0)
        # shank: gastrocnemius behind, Achilles tendon to the heel
        mb_chain(mb, [K_, A_], [0.17, 0.11], [0.18, 0.115], s=4.0)
        mb_ell(mb, K_.lerp(A_, 0.3) + back * 0.08, (0.24, 0.11, 0.12), frame_from(A_ - K_), s=3.5)
        mb_chain(mb, [K_.lerp(A_, 0.55) + back * 0.09, A_ + back * 0.07 + V(0, 0, 0.02)], [0.04, 0.03], [0.04, 0.03], s=4.5)
        mb_ell(mb, A_ + back * 0.03, (0.1, 0.105, 0.095), s=4.5)
        mb_chain(mb, [A_, B_], [0.11, 0.115], [0.1, 0.078], s=4.0)
        for ang, ln in ((-0.45, 0.24), (0.0, 0.30), (0.45, 0.24)):
            d = Vector((math.sin(ang) * sd, -math.cos(ang), 0))
            tip = B_ + d * ln
            tip.z = 0.038
            mid = B_.lerp(tip, 0.5) + V(0, 0, 0.014)
            mb_chain(mb, [B_ + d * 0.02, mid, tip], [0.06, 0.048, 0.038], [0.05, 0.04, 0.03], s=5.0, smooth=False)
            mb_ell(mb, mid, (0.04, 0.038, 0.034), s=5.0)
        mb_chain(mb, [B_ + V(0, 0.03, 0.03), B_ + V(-0.04 * sd, 0.15, 0.0)], [0.038, 0.027], [0.032, 0.024], s=5.0)

        # Wing arm (humerus + forearm + metacarpal) with shoulder muscle
        w = {k: mx(v, sd) for k, v in W.items()}
        mb_chain(mb, [w['shoulder'], w['elbow']], [0.15, 0.085], [0.13, 0.08], s=4.0)
        mb_chain(mb, [w['elbow'], w['wrist']], [0.07, 0.048], [0.06, 0.045], s=5.0)
        mb_ell(mb, w['wrist'], (0.06, 0.06, 0.05), s=5.0)
        mb_chain(mb, [w['wrist'], w['knuckle']], [0.05, 0.045], [0.045, 0.04], s=5.0)

    # Tail
    tp = [V(0, y, z) for y, z, w, h in TAIL_CTRL]
    mb_chain(mb, tp, [t[2] for t in TAIL_CTRL], [t[3] for t in TAIL_CTRL], s=4.0)

    # Lower jaw: separate family so the mouth can open cleanly
    mj = bpy.data.metaballs.new('MBJaw')
    mj.resolution = 0.012
    mj.render_resolution = 0.012
    mj.threshold = THRESH
    oj = link(bpy.data.objects.new('MBJaw', mj))
    mb_chain(mj, [V(0, -2.50, 2.36), V(0, -2.80, 2.38), V(0, -3.10, 2.375), V(0, -3.37, 2.37)],
             [0.17, 0.12, 0.085, 0.058], [0.085, 0.06, 0.05, 0.042], s=5.0)
    return ob, oj


def finish_metaball(ob, name, voxel, target_tris, smooth_iter=4):
    dg = bpy.context.evaluated_depsgraph_get()
    me = bpy.data.meshes.new_from_object(ob.evaluated_get(dg))
    mob = link(bpy.data.objects.new(name, me))
    bpy.data.objects.remove(ob)
    log(name, 'metaball verts', len(me.vertices))
    log(name, 'voxel size', voxel)
    m = mob.modifiers.new('rm', 'REMESH')
    m.mode = 'VOXEL'
    m.voxel_size = voxel
    m.adaptivity = 0.0
    apply_mods(mob)
    m = mob.modifiers.new('sm', 'SMOOTH')
    m.factor = 0.5
    m.iterations = smooth_iter
    apply_mods(mob)
    tris = sum(len(p.vertices) - 2 for p in mob.data.polygons)
    if tris > target_tris:
        m = mob.modifiers.new('dc', 'DECIMATE')
        m.ratio = target_tris / tris
        m.use_collapse_triangulate = True
        apply_mods(mob)
    recalc_normals(mob)
    log(name, 'tris', sum(len(p.vertices) - 2 for p in mob.data.polygons))
    return mob


# ---------------------------------------------------------------------------
# Loft / tube helpers for horns, spines, claws, teeth, fingers, fin
# ---------------------------------------------------------------------------
def loft(name, rings, cap0=True, cap1=True):
    n = len(rings[0])
    verts = [tuple(p) for ring in rings for p in ring]
    faces = []
    for i in range(len(rings) - 1):
        for j in range(n):
            a, b = i * n + j, i * n + (j + 1) % n
            faces.append((a, b, b + n, a + n))
    if cap0:
        c = len(verts)
        verts.append(tuple(sum((Vector(p) for p in rings[0]), Vector()) / n))
        for j in range(n):
            faces.append((c, (j + 1) % n, j))
    if cap1:
        c = len(verts)
        base = (len(rings) - 1) * n
        verts.append(tuple(sum((Vector(p) for p in rings[-1]), Vector()) / n))
        for j in range(n):
            faces.append((c, base + j, base + (j + 1) % n))
    ob = mesh_obj(name, verts, faces)
    # cylindrical UVs in metres (u around, v along), planar caps; packed later with the body
    R_ = [[Vector(p) for p in ring] for ring in rings]
    cen = [sum(r, Vector()) / n for r in R_]
    per = max(1e-4, sum(sum((r[(j + 1) % n] - r[j]).length for j in range(n)) for r in R_) / len(R_))
    vs = [0.0]
    for i in range(1, len(R_)):
        vs.append(vs[-1] + (cen[i] - cen[i - 1]).length)
    fuv = []
    for i in range(len(R_) - 1):
        for j in range(n):
            u0, u1 = per * j / n, per * (j + 1) / n
            fuv.append([(u0, vs[i]), (u1, vs[i]), (u1, vs[i + 1]), (u0, vs[i + 1])])
    vmax = vs[-1] + 0.02

    def capuv(ring, c, off):
        return [(off + (q - c).length * math.cos(2 * math.pi * jj / n), vmax + per * 0.2 + (q - c).length *
                 math.sin(2 * math.pi * jj / n)) for jj, q in enumerate(ring)]
    if cap0:
        cu = capuv(R_[0], cen[0], per * 0.25)
        cc = (per * 0.25, vmax + per * 0.2)
        for j in range(n):
            fuv.append([cc, cu[(j + 1) % n], cu[j]])
    if cap1:
        cu = capuv(R_[-1], cen[-1], per * 0.75)
        cc = (per * 0.75, vmax + per * 0.2)
        for j in range(n):
            fuv.append([cc, cu[j], cu[(j + 1) % n]])
    me_ = ob.data
    uvl = me_.uv_layers.new(name='UVMap')
    for poly, uvs in zip(me_.polygons, fuv):
        for li, uv in zip(poly.loop_indices, uvs):
            uvl.data[li].uv = uv
    me_.attributes.new('hasuv', 'INT', 'POINT').data.foreach_set('value', [1] * len(me_.vertices))
    ka = [j / n for i in range(len(R_)) for j in range(n)] + [0.0] * (len(me_.vertices) - len(R_) * n)
    me_.attributes.new('karound', 'FLOAT', 'POINT').data.foreach_set('value', ka)
    recalc_normals(ob, merge=1e-5)
    return ob


def tube(name, path, radii, sides=10, prof=(1.0, 1.0), upref=(0, 0, 1)):
    path = [Vector(p) for p in path]
    n = len(path)
    Ts = []
    for i in range(n):
        a, b = path[max(i - 1, 0)], path[min(i + 1, n - 1)]
        Ts.append((b - a).normalized())
    up = Vector(upref)
    N = (up - Ts[0] * up.dot(Ts[0]))
    if N.length < 1e-4:
        N = Ts[0].orthogonal()
    N.normalize()
    rings = []
    for i in range(n):
        if i > 0:  # parallel transport
            N = (N - Ts[i] * N.dot(Ts[i])).normalized()
        B = Ts[i].cross(N).normalized()
        r = radii[i]
        ring = []
        for j in range(sides):
            a = 2 * math.pi * j / sides
            ring.append(path[i] + B * (math.cos(a) * r * prof[0]) + N * (math.sin(a) * r * prof[1]))
        rings.append(ring)
    return loft(name, rings)


def bez(p0, p1, p2, n, p3=None):
    out = []
    for i in range(n):
        t = i / (n - 1)
        if p3 is None:
            out.append(p0 * (1 - t) ** 2 + p1 * 2 * t * (1 - t) + p2 * t * t)
        else:
            out.append(p0 * (1 - t) ** 3 + p1 * 3 * t * (1 - t) ** 2 + p2 * 3 * t * t * (1 - t) + p3 * t ** 3)
    return out


def set_attrs(ob, wclass, rbone=-1, mask=(0, 0, 0), tip=None):
    me = ob.data
    nv = len(me.vertices)
    for nm, tp in (('wclass', 'INT'), ('rbone', 'INT'), ('tip', 'FLOAT')):
        if nm not in me.attributes:
            me.attributes.new(nm, tp, 'POINT')
    if 'mask' not in me.attributes:
        me.attributes.new('mask', 'FLOAT_COLOR', 'POINT')
    if 'eyeq' not in me.attributes:
        me.attributes.new('eyeq', 'FLOAT_VECTOR', 'POINT')
    me.attributes['wclass'].data.foreach_set('value', [wclass] * nv)
    me.attributes['rbone'].data.foreach_set('value', [rbone] * nv)
    if tip is None:
        tip = [0.0] * nv
    me.attributes['tip'].data.foreach_set('value', list(tip))
    m = mask if callable(mask) else None
    cols = []
    for i, v in enumerate(me.vertices):
        c = m(i, v.co) if m else mask
        cols.extend((c[0], c[1], c[2], 1.0))
    me.attributes['mask'].data.foreach_set('color', cols)


def tip_along(ob, base, axis):
    """0 at base -> 1 at far end along axis, per vertex."""
    axis = Vector(axis).normalized()
    ds = [(v.co - base).dot(axis) for v in ob.data.vertices]
    mxd = max(ds) or 1.0
    return [max(0.0, min(1.0, d / mxd)) for d in ds]


# ---------------------------------------------------------------------------
# Bone list (name, head, tail, parent, roll reference)
# ---------------------------------------------------------------------------
def bone_defs():
    Z = (0, 0, 1)
    F = (0, -1, 0)
    B = [('root', V(0, 0, 0), V(0, -0.6, 0), None, Z),
         ('pelvis', J['pelvis'], J['spine_01'], 'root', Z),
         ('spine_01', J['spine_01'], J['spine_02'], 'pelvis', Z),
         ('spine_02', J['spine_02'], J['spine_03'], 'spine_01', Z),
         ('spine_03', J['spine_03'], J['chest'], 'spine_02', Z),
         ('chest', J['chest'], J['neck_01'], 'spine_03', Z),
         ('neck_01', J['neck_01'], J['neck_02'], 'chest', Z),
         ('neck_02', J['neck_02'], J['neck_03'], 'neck_01', Z),
         ('neck_03', J['neck_03'], J['neck_04'], 'neck_02', Z),
         ('neck_04', J['neck_04'], J['head'], 'neck_03', Z),
         ('head', J['head'], J['head_end'], 'neck_04', Z),
         ('jaw', J['jaw'], J['jaw_end'], 'head', Z)]
    prev = 'pelvis'
    for i in range(1, 13):
        nm = 'tail_%02d' % i
        nxt = J['tail_%02d' % (i + 1)] if i < 12 else J['tail_end']
        B.append((nm, J[nm], nxt, prev, Z))
        prev = nm
    for sd, sfx in ((1, '_l'), (-1, '_r')):
        g = {k: mx(v, sd) for k, v in L.items()}
        w = {k: mx(v, sd) for k, v in W.items()}
        B += [('upperarm' + sfx, g['shoulder'], g['elbow'], 'chest', F),
              ('forearm' + sfx, g['elbow'], g['wrist'], 'upperarm' + sfx, F),
              ('hand' + sfx, g['wrist'], g['palm'] + V(0, -0.19, -0.04), 'forearm' + sfx, F),
              ('thigh' + sfx, g['hip'], g['knee'], 'pelvis', F),
              ('calf' + sfx, g['knee'], g['ankle'], 'thigh' + sfx, F),
              ('foot' + sfx, g['ankle'], g['ball'] + V(0, -0.24, -0.04), 'calf' + sfx, F),
              ('wing_shoulder' + sfx, w['shoulder'], w['elbow'], 'chest', Z),
              ('wing_elbow' + sfx, w['elbow'], w['wrist'], 'wing_shoulder' + sfx, Z),
              ('wing_wrist' + sfx, w['wrist'], w['knuckle'], 'wing_elbow' + sfx, Z)]
        for k in range(1, 5):
            B.append(('wing_finger%d%s' % (k, sfx), w['knuckle'], w['tip%d' % k], 'wing_wrist' + sfx, Z))
    return B


# approximate flesh radius around each bone (for distance-based skinning)
def bone_radius(name):
    base = name[:-2] if name.endswith(('_l', '_r')) else name
    R = {'pelvis': 0.42, 'spine_01': 0.45, 'spine_02': 0.52, 'spine_03': 0.58, 'chest': 0.58,
         'neck_01': 0.36, 'neck_02': 0.28, 'neck_03': 0.22, 'neck_04': 0.18, 'head': 0.17, 'jaw': 0.08,
         'upperarm': 0.23, 'forearm': 0.13, 'hand': 0.09, 'thigh': 0.3, 'calf': 0.15, 'foot': 0.095,
         'wing_shoulder': 0.11, 'wing_elbow': 0.06, 'wing_wrist': 0.05}
    if base.startswith('tail_'):
        i = int(base[5:])
        return float(np.interp(i, [1, 3, 5, 7, 9, 11, 12], [0.33, 0.22, 0.15, 0.11, 0.075, 0.048, 0.035]))
    if base.startswith('wing_finger'):
        return 0.03
    return R.get(base, 0.1)


BONES = bone_defs()
BONE_NAMES = [b[0] for b in BONES]
BI = {n: i for i, n in enumerate(BONE_NAMES)}

# ---------------------------------------------------------------------------
# Build geometry
# ---------------------------------------------------------------------------
log('building metaballs')
mbo, mjo = build_body()
body = finish_metaball(mbo, 'Body', 0.0095, 250000, smooth_iter=3)
jaw = finish_metaball(mjo, 'Jaw', 0.0065, 16000, smooth_iter=2)
set_attrs(body, 0)
set_attrs(jaw, 2, BI['jaw'])
BV = bvh_of(body)
JV = bvh_of(jaw)


def ray_down(x, y, z0=5.0, tree=None):
    hit = (tree or BV).ray_cast(Vector((x, y, z0)), Vector((0, 0, -1)))
    return hit


def ray_in(y, z, side, tree=None):
    hit = (tree or BV).ray_cast(Vector((2.0 * side, y, z)), Vector((-side, 0, 0)))
    return hit


parts = []  # (object, material key)

# --- eyes: iris sphere with a corneal bulge, set in a socket, with eyelids
def ring_tube(name, center, fwd, up, side, rx, ry, radius_fn, n=56, sides=12):
    rings = []
    for i in range(n):
        th = 2 * math.pi * i / n
        p = center + side * (math.cos(th) * rx) + up * (math.sin(th) * ry)
        T = (-side * (math.sin(th) * rx) + up * (math.cos(th) * ry)).normalized()
        Nn = (p - center).normalized()
        B = T.cross(Nn).normalized()
        r = radius_fn(th)
        rings.append([p + Nn * (math.cos(2 * math.pi * j / sides) * r) + B * (math.sin(2 * math.pi * j / sides) * r * 1.15)
                      for j in range(sides)])
    nn = sides
    verts = [tuple(q) for ring in rings for q in ring]
    faces = []
    for i in range(n):
        i2 = (i + 1) % n
        for j in range(nn):
            j2 = (j + 1) % nn
            faces.append((i * nn + j, i * nn + j2, i2 * nn + j2, i2 * nn + j))
    ob = mesh_obj(name, verts, faces)
    recalc_normals(ob)
    return ob


EYE_INFO = []
for sd in (1, -1):
    ey, ez = -2.76, 2.665
    h = ray_in(ey, ez, sd)
    xs = abs(h[0].x) if h[0] else 0.17
    R = 0.036
    c = Vector((sd * (xs - 0.010), ey, ez))
    fwd = Vector((sd * 1.0, -0.45, 0.12)).normalized()
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=64, v_segments=40, radius=R)
    for v in bm.verts:   # corneal bulge on the front cap
        zc = v.co.z / R
        if zc > 0.55:
            v.co.z += R * 0.12 * ((zc - 0.55) / 0.45) ** 1.5
    rot = fwd.to_track_quat('Z', 'Y').to_matrix().to_4x4()
    bmesh.ops.transform(bm, matrix=Matrix.Translation(c) @ rot, verts=bm.verts)
    me = bpy.data.meshes.new('Eye')
    bm.to_mesh(me)
    bm.free()
    eo = link(bpy.data.objects.new('Eye', me))
    up = Vector((0, 0, 1))
    up = (up - fwd * up.dot(fwd)).normalized()
    side_ax = fwd.cross(up)
    set_attrs(eo, 2, BI['head'], (1.0, 0, 0))
    q = []
    for v in me.vertices:
        d = (v.co - c) / R
        q += [d.dot(side_ax), d.dot(up), d.dot(fwd)]
    me.attributes['eyeq'].data.foreach_set('vector', q)
    parts.append(eo)
    EYE_INFO.append((c, fwd, up, side_ax, R))
    # eyelids: a heavy upper lid and a thinner lower lid, as one ring around the opening
    lc = c + fwd * (R * 0.50)
    lid = ring_tube('Lid', lc, fwd, up, side_ax, R * 0.92, R * 0.78,
                    lambda th: 0.0065 + 0.006 * max(0.0, math.sin(th)) ** 1.5)
    set_attrs(lid, 2, BI['head'], (0, 0, 0))
    parts.append(lid)

# --- horns: two pairs, swept back
for sd in (1, -1):
    # major pair from the top-back of the skull
    b0 = V(0.095 * sd, -2.52, 2.73)
    path = bez(b0, b0 + V(0.10 * sd, 0.30, 0.06), b0 + V(0.16 * sd, 0.62, 0.05), 48, b0 + V(0.19 * sd, 0.86, 0.13))
    rad = [0.068 * (1 - t) ** 0.85 * (1 + 0.06 * math.sin(t * 46)) + 0.003 for t in np.linspace(0, 1, 48)]
    ho = tube('HornA', path, rad, sides=20, prof=(1.0, 0.85))
    set_attrs(ho, 2, BI['head'], (0, 1, 0), tip_along(ho, b0, path[-1] - b0))
    parts.append(ho)
    # minor pair from behind the cheek
    b1 = V(0.16 * sd, -2.47, 2.58)
    path = bez(b1, b1 + V(0.09 * sd, 0.18, -0.02), b1 + V(0.15 * sd, 0.38, -0.03), 36, b1 + V(0.17 * sd, 0.54, 0.02))
    rad = [0.046 * (1 - t) ** 0.85 * (1 + 0.06 * math.sin(t * 40)) + 0.002 for t in np.linspace(0, 1, 36)]
    ho = tube('HornB', path, rad, sides=16, prof=(1.0, 0.85))
    set_attrs(ho, 2, BI['head'], (0, 1, 0), tip_along(ho, b1, path[-1] - b1))
    parts.append(ho)
    # small cheek spikes along the jaw line
    for k, (yy, zz, ln) in enumerate(((-2.60, 2.51, 0.045), (-2.51, 2.50, 0.06), (-2.42, 2.51, 0.045))):
        hh = ray_in(yy, zz, sd)
        xs = abs(hh[0].x) if hh[0] else 0.2
        p0 = V(sd * (xs - 0.02), yy, zz)
        path = [p0, p0 + V(0.03 * sd, ln * 0.6, -0.01), p0 + V(0.045 * sd, ln, -0.005)]
        so = tube('Cheek', path, [0.016, 0.008, 0.001], sides=8)
        set_attrs(so, 2, BI['head'], (0, 1, 0), tip_along(so, p0, path[-1] - p0))
        parts.append(so)

# --- dorsal spines: skull to mid-tail along the top line
dpath = [J['head'] + V(0, 0.02, 0.1), J['neck_04'], J['neck_03'], J['neck_02'], J['neck_01'], J['chest'],
         J['spine_03'], J['spine_02'], J['spine_01'], J['pelvis']] + [J['tail_%02d' % i] for i in range(1, 8)]
dd = catmull(dpath, 600)
dd, dlen = resample_arclen(dd, 1200)
spine_prof = [(0.0, 0.07), (0.12, 0.12), (0.25, 0.15), (0.40, 0.22), (0.52, 0.22), (0.62, 0.19),
              (0.75, 0.15), (0.88, 0.11), (1.0, 0.05)]
s_pos = 0.0
k = 0
while s_pos < dlen:
    u = s_pos / dlen
    hgt = float(np.interp(u, [a for a, b in spine_prof], [b for a, b in spine_prof]))
    i = int(u * (len(dd) - 1))
    p = Vector(dd[i])
    T = Vector(dd[min(i + 5, len(dd) - 1)] - dd[max(i - 5, 0)]).normalized()  # toward the tail
    hit = ray_down(0.0, p.y, p.z + 1.5)
    if hit[0] is not None:
        top = hit[0]
        nrm = hit[1]
        ax = (Vector((0, 0, 1)) * 0.75 + nrm * 0.25 + T * 0.75).normalized()
        base = top - ax * 0.03
        path = bez(base, base + ax * hgt * 0.5 + T * hgt * 0.05, base + ax * hgt + T * hgt * 0.22, 9)
        rad = [hgt * 0.32 * (1 - t) ** 1.1 + 0.002 for t in np.linspace(0, 1, 9)]
        so = tube('Spine', path, rad, sides=10, prof=(0.28, 1.0), upref=tuple(T))
        # rigid to the bone nearest the spine's base
        best, bd = 'pelvis', 1e9
        for nm, hd, tl, par, rr in BONES:
            if nm in ('root', 'jaw') or nm.endswith(('_l', '_r')):
                continue
            a, b = np.array(hd), np.array(tl)
            pp = np.array(base)
            t_ = np.clip(np.dot(pp - a, b - a) / max(np.dot(b - a, b - a), 1e-9), 0, 1)
            dist = np.linalg.norm(pp - (a + t_ * (b - a)))
            if dist < bd:
                best, bd = nm, dist
        set_attrs(so, 2, BI[best], (0, 1, 0), tip_along(so, base, ax))
        parts.append(so)
        k += 1
    s_pos += max(0.11, hgt * 1.05)
log('spines', k)

# --- teeth (upper and lower), crocodilian, visible when closed
for sd in (1, -1):
    for i, yy in enumerate(np.linspace(-2.84, -3.37, 11)):
        mz = float(np.interp(yy, [-3.4, -2.8], [2.425, 2.47]))
        hh = ray_in(yy, mz + 0.012, sd)
        if hh[0] is None:
            continue
        xs = abs(hh[0].x)
        ln = 0.012 + 0.016 * math.sin(i * 1.7) ** 2 + (0.012 if i in (2, 6) else 0.0)
        p0 = V(sd * (xs - 0.014), yy, mz + 0.045)
        path = [p0, p0 + V(0.002 * sd, 0, -0.045 - ln * 0.4), p0 + V(0.003 * sd, 0.004, -0.045 - ln)]
        to = tube('Tooth', path, [0.011, 0.007, 0.0005], sides=6)
        set_attrs(to, 2, BI['head'], (0, 1, 0), [0.9] * len(to.data.vertices))
        parts.append(to)
    for i, yy in enumerate(np.linspace(-2.9, -3.33, 8)):
        mz = float(np.interp(yy, [-3.4, -2.8], [2.41, 2.43]))
        hh = ray_in(yy, mz - 0.015, sd, JV)
        if hh[0] is None:
            continue
        xs = abs(hh[0].x)
        ln = 0.010 + 0.012 * math.cos(i * 2.1) ** 2 + (0.01 if i == 1 else 0.0)
        p0 = V(sd * (xs - 0.012), yy, mz - 0.042)
        path = [p0, p0 + V(0.002 * sd, 0, 0.042 + ln * 0.4), p0 + V(0.003 * sd, 0.003, 0.042 + ln)]
        to = tube('ToothL', path, [0.01, 0.006, 0.0005], sides=6)
        set_attrs(to, 2, BI['jaw'], (0, 1, 0), [0.9] * len(to.data.vertices))
        parts.append(to)

# --- claws on every toe
for sd, sfx in ((1, '_l'), (-1, '_r')):
    g = {k: mx(v, sd) for k, v in L.items()}
    for (palm, toes, ln, bone, cr) in ((g['palm'], (-0.45, 0.0, 0.45), (0.20, 0.25, 0.20), 'hand' + sfx, 0.029),
                                       (g['ball'], (-0.45, 0.0, 0.45), (0.24, 0.30, 0.24), 'foot' + sfx, 0.034)):
        for ang, l_ in zip(toes, ln):
            d = Vector((math.sin(ang) * sd, -math.cos(ang), 0))
            tip = palm + d * l_
            tip.z = 0.034
            p0 = tip - d * 0.03 + V(0, 0, 0.012)
            path = bez(p0, tip + d * 0.05 + V(0, 0, 0.025), tip + d * 0.10 - V(0, 0, 0.034), 10)
            rad = [cr * (1 - t) ** 0.9 + 0.0008 for t in np.linspace(0, 1, 10)]
            co = tube('Claw', path, rad, sides=14, prof=(0.75, 1.0))
            set_attrs(co, 2, BI[bone], (0, 1, 0), tip_along(co, p0, path[-1] - p0))
            parts.append(co)
    # hallux claw
    p0 = g['ball'] + V(-0.04 * sd, 0.15, 0.0)
    path = bez(p0, p0 + V(-0.01 * sd, 0.05, 0.0), p0 + V(-0.015 * sd, 0.08, -0.03), 8)
    co = tube('Claw', path, [0.02 * (1 - t) + 0.0008 for t in np.linspace(0, 1, 8)], sides=8)
    set_attrs(co, 2, BI['foot' + sfx], (0, 1, 0), tip_along(co, p0, path[-1] - p0))
    parts.append(co)

# --- wing fingers (bony struts) and the thumb claw
for sd, sfx in ((1, '_l'), (-1, '_r')):
    w = {k: mx(v, sd) for k, v in W.items()}
    for k in range(1, 5):
        a, b = w['knuckle'], w['tip%d' % k]
        n = 18
        path = [a.lerp(b, t) for t in np.linspace(0, 1, n)]
        n = 40
        path = [a.lerp(b, t) for t in np.linspace(0, 1, n)]
        rad = [0.048 * (1 - t) + 0.012 * t + 0.01 * math.exp(-((t - 0.0) / 0.05) ** 2)
               + sum(0.011 * (1 - 0.5 * kc) * math.exp(-((t - kc) / 0.025) ** 2) for kc in (0.30, 0.56, 0.78))
               for t in np.linspace(0, 1, n)]
        rad[-1] = 0.004
        fo = tube('Finger', path, rad, sides=14, prof=(1.0, 0.8))
        set_attrs(fo, 2, BI['wing_finger%d%s' % (k, sfx)])
        parts.append(fo)
    # thumb claw at the wrist, pointing forward
    p0 = w['wrist'] + V(0.02 * sd, -0.02, 0.02)
    path = bez(p0, p0 + V(0.02 * sd, -0.12, 0.03), p0 + V(0.05 * sd, -0.22, -0.03), 10)
    co = tube('Thumb', path, [0.035 * (1 - t) ** 0.8 + 0.001 for t in np.linspace(0, 1, 10)], sides=10, prof=(0.8, 1.0))
    set_attrs(co, 2, BI['wing_wrist' + sfx], (0, 1, 0), tip_along(co, p0, path[-1] - p0))
    parts.append(co)

# --- leaf-shaped tail fin (vertical)
te = J['tail_end']
fin_rings = []
ylo, yhi = te.y - 0.62, te.y + 0.10
for t in np.linspace(0, 1, 30):
    y = ylo + (yhi - ylo) * t
    # follow the tail centreline height
    idx = np.argmin(np.abs(_tail_dense[:, 1] - min(y, _tail_dense[-1, 1])))
    zc = _tail_dense[idx, 2] if y <= _tail_dense[-1, 1] else _tail_dense[-1, 2] + (y - _tail_dense[-1, 1]) * 0.05
    hh = 0.17 * math.sin(math.pi * t ** 0.72) ** 0.9 + 1e-4
    th = 0.006 + 0.02 * (1 - t) ** 2
    ring = []
    for j in range(20):
        a = 2 * math.pi * j / 20
        # lens: sharper top/bottom edges
        cz = math.sin(a)
        cx = math.cos(a) * (abs(math.cos(a)) ** 0.3)
        ring.append(Vector((cx * th, y, zc + cz * hh * (1.0 if cz > 0 else 0.8))))
    fin_rings.append(ring)
fin = loft('Fin', fin_rings)
set_attrs(fin, 2, BI['tail_12'], (0, 0, 1))
parts.append(fin)

# --- wing membranes (Coons patch between body and finger 4, fans between fingers)
WING_NT = 36   # samples along each finger (shared between panels)
WING_NS = 70   # samples along the leading edge of the inner panel
WING_NV = 24   # samples across each outer panel


def polyline_at(pts, s):
    pts = [Vector(p) for p in pts]
    seg = [(pts[i + 1] - pts[i]).length for i in range(len(pts) - 1)]
    tot = sum(seg)
    d = s * tot
    for i, l_ in enumerate(seg):
        if d <= l_ or i == len(seg) - 1:
            return pts[i].lerp(pts[i + 1], min(d / l_, 1.0))
        d -= l_


def membrane(sd):
    w = {k: mx(v, sd) for k, v in W.items()}
    K = w['knuckle']
    verts, faces = [], []
    # inner panel: Coons patch
    att = w['attach'].copy()
    hit = ray_down(att.x, att.y)
    if hit[0] is not None:
        att.z = hit[0].z - 0.03
    shoulder = w['shoulder']

    def C0(s):
        return polyline_at([shoulder, w['elbow'], w['wrist'], K], s)

    T4 = w['tip4']
    mid_le = C0(0.5)
    def C1(s):
        p = att.lerp(T4, s)
        perp = Vector((-(T4 - att).y, (T4 - att).x, 0)).normalized()
        if perp.dot(mid_le - p) < 0:
            perp = -perp
        return p + perp * (0.21 * (T4 - att).length * math.sin(math.pi * s) ** 0.9)

    def D0(t):
        p = shoulder.lerp(att, t)
        h = ray_down(p.x, p.y)
        if h[0] is not None and 0 < t < 1:
            zt = h[0].z - 0.03
            p.z = zt
        return p

    def D1(t):
        return K.lerp(T4, t)

    P00, P10, P01, P11 = C0(0), C0(1), C1(0), C1(1)
    NS, NT = WING_NS, WING_NT
    # --- skin weights for the membrane, blended from its boundaries (Coons-style) so
    # it folds like a fan: leading edge -> arm bones, flank -> spine, finger 4 -> finger 4.
    sfx = '_l' if sd > 0 else '_r'
    arm_segs = [('wing_shoulder' + sfx, shoulder, w['elbow']), ('wing_elbow' + sfx, w['elbow'], w['wrist']),
                ('wing_wrist' + sfx, w['wrist'], K)]
    sp_segs = [(n, J[n], J[t]) for n, t in (('chest', 'neck_01'), ('spine_03', 'chest'), ('spine_02', 'spine_03'),
                                            ('spine_01', 'spine_02'), ('pelvis', 'spine_01'))]

    def idw(p, segs, flat=False, pw=4, eps=0.03):
        out = {}
        for nm, a, b in segs:
            if flat:
                a, b, q = Vector((0, a.y, 0)), Vector((0, b.y, 0)), Vector((0, p.y, 0))
            else:
                q = p
            ab = b - a
            t_ = max(0.0, min(1.0, (q - a).dot(ab) / max(ab.dot(ab), 1e-9)))
            d = (q - (a + ab * t_)).length
            out[nm] = 1.0 / (d + eps) ** pw
        tot = sum(out.values())
        return {k: v / tot for k, v in out.items()}

    def wadd(*terms):
        out = {}
        for f, d in terms:
            for k, v in d.items():
                out[k] = out.get(k, 0.0) + f * v
        out = {k: max(v, 0.0) for k, v in out.items()}
        tot = sum(out.values()) or 1.0
        return {k: v / tot for k, v in out.items() if v / tot > 0.004}

    F4 = {'wing_finger4' + sfx: 1.0}
    WA = lambda s_: idw(C0(s_), arm_segs)
    WB = lambda t_: idw(shoulder.lerp(att, t_), sp_segs, flat=True)
    WC = lambda s_: wadd((1 - s_, WB(1.0)), (s_, F4))
    Q00 = wadd((0.5, WA(0)), (0.5, WB(0)))
    Q10 = wadd((0.5, WA(1)), (0.5, F4))
    Q01 = WB(1.0)
    Q11 = F4
    vw = []
    d0 = [D0(j / NT) for j in range(NT + 1)]
    base = len(verts)
    for i in range(NS + 1):
        s = i / NS
        c0, c1 = C0(s), C1(s)
        for j in range(NT + 1):
            t = j / NT
            p = (c0 * (1 - t) + c1 * t + d0[j] * (1 - s) + D1(t) * s
                 - (P00 * (1 - s) * (1 - t) + P10 * s * (1 - t) + P01 * (1 - s) * t + P11 * s * t))
            p.z -= 0.13 * math.sin(math.pi * s) * math.sin(math.pi * t) ** 1.3  # sag / billow
            verts.append(p)
            vw.append(wadd((1 - t, WA(s)), (t, WC(s)), (1 - s, WB(t)), (s, F4),
                           (-(1 - s) * (1 - t), Q00), (-s * (1 - t), Q10), (-(1 - s) * t, Q01), (-s * t, Q11)))
    for i in range(NS):
        for j in range(NT):
            a = base + i * (NT + 1) + j
            faces.append((a, a + NT + 1, a + NT + 2, a + 1))
    # outer panels (fans from the knuckle)
    for (ka, kb) in ((4, 3), (3, 2), (2, 1)):
        Ta, Tb = w['tip%d' % ka], w['tip%d' % kb]
        base = len(verts)
        for i in range(WING_NV + 1):
            v = i / WING_NV
            e = Ta.lerp(Tb, v)
            e = e + (K - e).normalized() * (0.28 * (Tb - Ta).length * math.sin(math.pi * v) ** 0.85)
            for j in range(NT + 1):
                u = j / NT
                p = K.lerp(e, u)
                p.z -= 0.10 * math.sin(math.pi * u) ** 0.8 * math.sin(math.pi * v)
                verts.append(p)
                fw = wadd((1 - v, {'wing_finger%d%s' % (ka, sfx): 1.0}), (v, {'wing_finger%d%s' % (kb, sfx): 1.0}))
                k_ = min(1.0, u / 0.1)
                k_ = k_ * k_ * (3 - 2 * k_)
                vw.append(wadd((k_, fw), (1 - k_, {'wing_wrist' + sfx: 1.0})))
        for i in range(WING_NV):
            for j in range(NT):
                a = base + i * (NT + 1) + j
                faces.append((a, a + NT + 1, a + NT + 2, a + 1))
    ob = mesh_obj('Membrane', verts, faces)
    groups = {}
    for vi, d in enumerate(vw):
        for nm, wt in d.items():
            if nm not in groups:
                groups[nm] = ob.vertex_groups.new(name=nm)
            groups[nm].add([vi], wt, 'REPLACE')
    bm = bmesh.new()
    bm.from_mesh(ob.data)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-4)
    bmesh.ops.dissolve_degenerate(bm, dist=1e-5, edges=bm.edges)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    # make normals face up (+Z) for consistency
    up = sum((f.normal.z for f in bm.faces))
    if up < 0:
        bmesh.ops.reverse_faces(bm, faces=bm.faces)
    bm.to_mesh(ob.data)
    bm.free()
    # UVs: planar from above, mirrored (both wings share texture space)
    me = ob.data
    uvl = me.uv_layers.new(name='UVMap')
    for poly in me.polygons:
        for li in poly.loop_indices:
            co = me.vertices[me.loops[li].vertex_index].co
            uvl.data[li].uv = (0.01 + 0.98 * abs(co.x) / 7.8, 0.01 + 0.98 * (co.y + 1.35) / 4.45)
    # thickness: ~30 mm along the bones, ~6 mm in the middle of each panel
    segs = [(shoulder, w['elbow']), (w['elbow'], w['wrist']), (w['wrist'], K), (shoulder, att)] + \
        [(K, w['tip%d' % k]) for k in range(1, 5)]
    P = np.array([v.co[:] for v in me.vertices])
    dmin = np.full(len(P), 9.0)
    for a_, b_ in segs:
        a_, b_ = np.array(a_), np.array(b_)
        ab = b_ - a_
        t_ = np.clip(((P - a_) @ ab) / (ab @ ab), 0, 1)
        dmin = np.minimum(dmin, np.linalg.norm(P - (a_ + t_[:, None] * ab), axis=1))
    thick_w = np.clip(1 - (dmin - 0.02) / 0.35, 0, 1) ** 1.5
    tg = ob.vertex_groups.new(name='thick')
    for vi, wt in enumerate(thick_w):
        tg.add([vi], float(wt), 'REPLACE')
    m = ob.modifiers.new('sol', 'SOLIDIFY')
    m.thickness = 0.03
    m.offset = 0.0
    m.use_rim = True
    m.use_even_offset = True
    m.vertex_group = 'thick'
    m.thickness_vertex_group = 0.2
    apply_mods(ob)
    tgi = ob.vertex_groups['thick'].index
    mthin = []
    for v in ob.data.vertices:
        wt = 0.0
        for g in v.groups:
            if g.group == tgi:
                wt = g.weight
        mthin.append(1.0 - wt)
    ob.vertex_groups.remove(ob.vertex_groups['thick'])
    set_attrs(ob, 1)
    if 'mthin' not in ob.data.attributes:
        ob.data.attributes.new('mthin', 'FLOAT', 'POINT')
    ob.data.attributes['mthin'].data.foreach_set('value', mthin)
    return ob


for sd in (1, -1):
    parts.append(membrane(sd))
log('parts built', len(parts))

# ---------------------------------------------------------------------------
# Materials (procedural, for baking)
# ---------------------------------------------------------------------------
M_BODY = bpy.data.materials.new('M_Dragon_Body')
M_WING = bpy.data.materials.new('M_Dragon_Wing')
for ob in [body, jaw] + parts:
    ob.data.materials.clear()
    ob.data.materials.append(M_WING if ob.name.startswith('Membrane') else M_BODY)

# ---------------------------------------------------------------------------
# Join, ground, scale to spec
# ---------------------------------------------------------------------------
allobs = [body, jaw] + parts
for ob in allobs:
    ob.select_set(True)
bpy.context.view_layer.objects.active = body
with bpy.context.temp_override(active_object=body, selected_editable_objects=allobs, selected_objects=allobs):
    bpy.ops.object.join()
dragon = body
dragon.name = 'SK_Dragon_Mesh'
dragon.data.name = 'SK_Dragon_Mesh'
me = dragon.data
co = np.zeros(len(me.vertices) * 3)
me.vertices.foreach_get('co', co)
co = co.reshape(-1, 3)
length = co[:, 1].max() - co[:, 1].min()
k_scale = TARGET_LEN / length
zmin = co[:, 2].min()
log('raw length %.3f span %.3f zmin %.3f -> scale %.4f' % (length, co[:, 0].max() - co[:, 0].min(), zmin, k_scale))
co[:, 2] -= zmin
co *= k_scale
me.vertices.foreach_set('co', co.ravel())
me.update()
for key in list(J.keys()):
    J[key] = (J[key] - Vector((0, 0, zmin))) * k_scale
for D in (L, W):
    for key in list(D.keys()):
        D[key] = (D[key] - Vector((0, 0, zmin))) * k_scale
BONES = bone_defs()
span = co[:, 0].max() - co[:, 0].min()
LEN_FINAL = co[:, 1].max() - co[:, 1].min()
log('final length %.3f m, span %.3f m, height %.3f m' % (LEN_FINAL, span, co[:, 2].max()))

# triangulate (stable tangents for baking + UE), smooth shading
m = dragon.modifiers.new('tri', 'TRIANGULATE')
m.quad_method = 'BEAUTY'
apply_mods(dragon)
me = dragon.data
me.polygons.foreach_set('use_smooth', [True] * len(me.polygons))
me.update()
NTRI = len(me.polygons)
log('triangles', NTRI, 'verts', len(me.vertices))

# ---------------------------------------------------------------------------
# Per-vertex helpers: nearest-bone data -> belly / scale-size attributes, weights
# ---------------------------------------------------------------------------
co = np.zeros(len(me.vertices) * 3)
me.vertices.foreach_get('co', co)
co = co.reshape(-1, 3)
NV = len(co)
wclass = np.zeros(NV, int)
me.attributes['wclass'].data.foreach_get('value', wclass)
rbone = np.zeros(NV, int)
me.attributes['rbone'].data.foreach_get('value', rbone)

heads = np.array([b[1] for b in BONES], float)
tails = np.array([b[2] for b in BONES], float)
radii = np.array([bone_radius(b[0]) for b in BONES])


def seg_dist_all(P):
    ab = tails - heads  # (B,3)
    ap = P[:, None, :] - heads[None]  # (N,B,3)
    t = np.clip((ap * ab[None]).sum(-1) / np.maximum((ab * ab).sum(-1), 1e-9)[None], 0, 1)
    C = heads[None] + t[..., None] * ab[None]
    d = np.linalg.norm(P[:, None, :] - C, axis=-1)
    return d, C, t


D, CP, TT = seg_dist_all(co)

side_l = np.array([n.endswith('_l') for n in BONE_NAMES])
side_r = np.array([n.endswith('_r') for n in BONE_NAMES])
is_wing = np.array([n.startswith('wing_') for n in BONE_NAMES])
is_limb = np.array([n.split('_')[0] in ('upperarm', 'forearm', 'hand', 'thigh', 'calf', 'foot') for n in BONE_NAMES])
body_ok = np.array([n not in ('root', 'jaw') and not n.startswith('wing_finger') for n in BONE_NAMES])
spine_like = np.array([n in ('pelvis', 'spine_01', 'spine_02', 'spine_03', 'chest') for n in BONE_NAMES])

# edges for smoothing
edges = np.zeros(len(me.edges) * 2, int)
me.edges.foreach_get('vertices', edges)
edges = edges.reshape(-1, 2)


def smooth_field(F, iters, mask=None, pin=None):
    F = F.copy()
    for _ in range(iters):
        acc = np.zeros_like(F)
        cnt = np.zeros(len(F))
        np.add.at(acc, edges[:, 0], F[edges[:, 1]])
        np.add.at(acc, edges[:, 1], F[edges[:, 0]])
        np.add.at(cnt, edges[:, 0], 1)
        np.add.at(cnt, edges[:, 1], 1)
        newF = (F + acc) / (1 + cnt)[:, None] if F.ndim == 2 else (F + acc) / (1 + cnt)
        if mask is not None:
            newF[~mask] = F[~mask]
        if pin is not None:
            newF[pin] = F[pin]
        F = newF
    return F


# ---- skin weights
W8 = np.zeros((NV, len(BONES)))
x = co[:, 0]
allow_side = np.ones((NV, len(BONES)), bool)
allow_side[:, side_l] = (x > -0.03)[:, None]
allow_side[:, side_r] = (x < 0.03)[:, None]

# body skin: normalised distance
bmask = wclass == 0
cand = body_ok[None, :] & allow_side
Dn = D / radii[None]
score = np.where(cand, 1.0 / np.maximum(Dn, 0.05) ** 6, 0.0)
W8[bmask] = score[bmask]

# membrane: plain distance to wing bones; body bones measured from the flank
mmask = wclass == 1
gname = {g.index: g.name for g in dragon.vertex_groups}
for vi in np.where(mmask)[0]:
    for g in me.vertices[vi].groups:
        W8[vi, BI[gname[g.group]]] = g.weight
while dragon.vertex_groups:
    dragon.vertex_groups.remove(dragon.vertex_groups[0])

# rigid parts
rmask = wclass == 2
W8[rmask] = 0
W8[np.where(rmask)[0], rbone[rmask]] = 1.0

W8 /= np.maximum(W8.sum(1, keepdims=True), 1e-12)
# smooth body + membrane weights (membrane verts lying on a finger stay pinned)
W8 = smooth_field(W8, 6, mask=bmask, pin=rmask | mmask)
# keep top 4
idx = np.argsort(-W8, axis=1)
keep = np.zeros_like(W8, bool)
np.put_along_axis(keep, idx[:, :4], True, axis=1)
W8 = np.where(keep & (W8 > 0.01), W8, 0)
W8 /= np.maximum(W8.sum(1, keepdims=True), 1e-12)

# ---- belly and scale-size attributes (body material)
near = np.argmin(np.where(cand, Dn, 1e9), axis=1)
vec = co - CP[np.arange(NV), near]
vn = vec / np.maximum(np.linalg.norm(vec, axis=1, keepdims=True), 1e-6)
limbv = is_limb[near] | is_wing[near]
belly = np.clip((-vn[:, 2] - 0.15) / 0.55, 0, 1)
inner = np.clip(-vn[:, 0] * np.sign(co[:, 0] + 1e-9), 0, 1)
belly_limb = np.clip(0.5 * inner + 0.6 * np.clip(-vn[:, 2], 0, 1), 0, 1)
belly = np.where(limbv, belly_limb, belly)
belly[wclass != 0] = np.where(rbone[wclass != 0] == BI['jaw'], np.clip(-vn[wclass != 0][:, 2] * 2, 0, 1), 0)
rad_local = np.linalg.norm(vec, axis=1)
scl = np.clip((0.40 - rad_local) / 0.30, 0, 1) * 0.8 + belly * 0.3
headish = np.isin(near, [BI['head'], BI['neck_04']])
scl = np.where(headish, np.maximum(scl, 0.85), scl)
mask_arr = np.zeros(NV * 4)
me.attributes['mask'].data.foreach_get('color', mask_arr)
mask_arr = mask_arr.reshape(-1, 4)


def gauss_pts(pts, r):
    out = np.zeros(NV)
    for p in pts:
        d = np.linalg.norm(co - np.array(p), axis=1)
        out = np.maximum(out, np.exp(-(d / r) ** 2))
    return out


both = lambda v: [mx(v, 1), mx(v, -1)]
joints = []
for k in ('elbow', 'wrist', 'knee', 'ankle', 'palm', 'ball'):
    joints += both(L[k])
for k in ('elbow', 'wrist', 'knuckle'):
    joints += both(W[k])
joint = gauss_pts(joints, 0.16)
scl = np.clip(np.maximum(scl, 0.95 * joint), 0, 1)
scl = np.where(limbv, np.clip(scl + 0.45 * inner + 0.2, 0, 1), scl)   # fine scales on inner limbs
# skin folds: armpits, groin, wing roots, inner elbows, back of knees, throat
folds = []
for v in both(L['shoulder'] + Vector((0, 0.16, -0.18))) + both(L['hip'] + Vector((0, -0.2, -0.2))) \
        + both(W['shoulder'] + Vector((0.05, 0.1, 0.0))) + both(L['elbow'] + Vector((0, -0.08, 0.02))) \
        + both(L['knee'] + Vector((0, 0.1, -0.02))) + both(L['wrist'] + Vector((0, -0.05, 0))) \
        + both(L['ankle'] + Vector((0, -0.06, 0))) + both(Vector((0.3, -1.35, 1.55)) * k_scale) \
        + both(L['palm'] + Vector((0, 0.03, 0.05))) + both(L['ball'] + Vector((0, 0.05, 0.06))):
    folds.append(v)
fold = gauss_pts(folds, 0.2)
neckish = np.isin(near, [BI['neck_01'], BI['neck_02'], BI['neck_03'], BI['neck_04']])
fold = np.maximum(fold, neckish * np.clip(belly * 1.5 + 0.35, 0, 1) * 0.9)
fold = np.where(wclass == 0, fold, 0)
scute = np.clip((belly - 0.62) / 0.25, 0, 1) * (~limbv) * (~headish)
# scale flow direction (toward the tail on the trunk, toward the toes on limbs)
dirs = tails - heads
dirs /= np.maximum(np.linalg.norm(dirs, axis=1, keepdims=True), 1e-9)
fwd_chain = np.array([n in ('root', 'pelvis', 'spine_01', 'spine_02', 'spine_03', 'chest', 'neck_01', 'neck_02',
                            'neck_03', 'neck_04', 'head', 'jaw') for n in BONE_NAMES])
fdir = np.where(fwd_chain[:, None], -dirs, dirs)
flow = fdir[near]
dmask = ((mask_arr[:, 0] < 0.05) & (mask_arr[:, 1] < 0.05) & (wclass != 1)).astype(float)
dmask = np.where(mask_arr[:, 2] > 0.5, 0.35, dmask)
memb = (wclass == 1).astype(float)
# body-surface coordinates for the osteoderm / scute grids: u along the centreline (nose->tail),
# v around the body from the dorsal midline, vb from the ventral midline (metres)
chain = ['head', 'neck_04', 'neck_03', 'neck_02', 'neck_01', 'chest', 'spine_03', 'spine_02', 'spine_01', 'pelvis'] + \
    ['tail_%02d' % i for i in range(1, 13)]
blen = np.linalg.norm(tails - heads, axis=1)
cum = {}
acc = 0.0
for nm in chain:
    cum[nm] = acc
    acc += blen[BI[nm]]
is_chain = np.zeros(len(BONES), bool)
cum_arr = np.zeros(len(BONES))
for nm in chain:
    is_chain[BI[nm]] = True
    cum_arr[BI[nm]] = cum[nm]
tt = TT[np.arange(NV), near]
uu = cum_arr[near] + np.where(fwd_chain[near], 1 - tt, tt) * blen[near]
dch = fdir[near]
upv = np.array([0, 0, 1.0])[None] - dch * dch[:, 2:3]
upv /= np.maximum(np.linalg.norm(upv, axis=1, keepdims=True), 1e-6)
sidev = np.cross(dch, upv)
ang = np.arctan2((vec * sidev).sum(1), (vec * upv).sum(1))
vv = ang * rad_local
vb = (np.pi - np.abs(ang)) * np.sign(ang) * rad_local
onchain = is_chain[near] & (wclass == 0)
dorsal = np.clip((1.05 - np.abs(ang)) / 0.35, 0, 1) * onchain * (~headish)
psize = np.clip(radii[near] * 0.30, 0.03, 0.125)

# macro sculpt on the game mesh itself (silhouette + bake): ribs under the flank skin
def sstep(x, a, b):
    t = np.clip((x - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)


k_ = k_scale
ribmask = (onchain & np.isin(near, [BI['chest'], BI['spine_03'], BI['spine_02'], BI['spine_01']])).astype(float)
ribmask *= sstep(co[:, 1], -1.25 * k_, -1.0 * k_) * (1 - sstep(co[:, 1], 0.05 * k_, 0.35 * k_))
ribmask *= sstep(np.abs(ang), 0.55, 0.9) * (1 - sstep(np.abs(ang), 2.0, 2.4))
ribmask = smooth_field(ribmask, 4)
phase = (co[:, 1] - 0.4 * (co[:, 2] - 1.45 * k_)) / (0.15 * k_)
ridge = (0.5 + 0.5 * np.cos(2 * np.pi * phase)) ** 2.5
vnorm = np.zeros(NV * 3)
me.vertex_normals.foreach_get('vector', vnorm)
vnorm = vnorm.reshape(-1, 3)
co = co + vnorm * (ribmask * (ridge - 0.35) * 0.014)[:, None]
me.vertices.foreach_set('co', co.ravel())
me.update()
scute = scute * onchain
fld = smooth_field(np.stack([belly, scl, fold, scute, flow[:, 0], flow[:, 1], flow[:, 2], uu, dorsal, psize], 1), 12,
                   mask=(wclass == 0))
for nm, arr in (('dorsal', fld[:, 8]), ('psize', fld[:, 9])):
    if nm not in me.attributes:
        me.attributes.new(nm, 'FLOAT', 'POINT')
    me.attributes[nm].data.foreach_set('value', arr.astype(float).tolist())
if 'bodyuv' not in me.attributes:
    me.attributes.new('bodyuv', 'FLOAT_VECTOR', 'POINT')
me.attributes['bodyuv'].data.foreach_set('vector', np.stack([fld[:, 7], vv, vb], 1).astype(float).ravel().tolist())
hm = np.where(headish | (rbone == BI['jaw']), 1.0, 0.0) * (wclass != 1)
if 'headm' not in me.attributes:
    me.attributes.new('headm', 'FLOAT', 'POINT')
me.attributes['headm'].data.foreach_set('value', smooth_field(hm, 6).astype(float).tolist())
flow = fld[:, 4:7] / np.maximum(np.linalg.norm(fld[:, 4:7], axis=1, keepdims=True), 1e-6)
for nm, arr in (('belly', fld[:, 0]), ('scl', fld[:, 1]), ('fold', fld[:, 2]), ('scute', fld[:, 3]), ('dmask', dmask),
                ('memb', memb)):
    if nm not in me.attributes:
        me.attributes.new(nm, 'FLOAT', 'POINT')
    me.attributes[nm].data.foreach_set('value', arr.astype(float).tolist())
if 'flow' not in me.attributes:
    me.attributes.new('flow', 'FLOAT_VECTOR', 'POINT')
me.attributes['flow'].data.foreach_set('vector', flow.astype(float).ravel().tolist())
me.update()

# ---------------------------------------------------------------------------
# UVs: body faces smart-projected into their own 0-1, wing faces keep planar map
# ---------------------------------------------------------------------------
if 'UVMap' not in me.uv_layers:
    me.uv_layers.new(name='UVMap')
me.uv_layers.active = me.uv_layers['UVMap']
bpy.context.view_layer.objects.active = dragon
dragon.select_set(True)
bpy.ops.object.mode_set(mode='EDIT')
bm = bmesh.from_edit_mesh(me)
hl = bm.verts.layers.int.get('hasuv')
for f in bm.faces:
    lofted = hl is not None and all(v[hl] for v in f.verts)
    f.select = (f.material_index == 0) and not lofted
    f.hide_set(f.material_index != 0 or lofted)
bmesh.update_edit_mesh(me)
bpy.ops.mesh.select_all(action='SELECT')
bpy.ops.uv.smart_project(angle_limit=math.radians(38), island_margin=0.002, area_weight=0.0,
                         correct_aspect=True, scale_to_bounds=False)
bpy.ops.mesh.reveal(select=False)
bm = bmesh.from_edit_mesh(me)
for f in bm.faces:
    f.select = (f.material_index == 0)
    f.hide_set(f.material_index != 0)
bmesh.update_edit_mesh(me)
bpy.ops.mesh.select_all(action='SELECT')
bpy.ops.uv.select_all(action='SELECT')
bpy.ops.uv.average_islands_scale()
bpy.ops.uv.pack_islands(rotate=True, margin=0.0025)
bpy.ops.mesh.reveal(select=False)
bpy.ops.object.mode_set(mode='OBJECT')
log('UVs done')

# ---------------------------------------------------------------------------
# Procedural shader graphs
# ---------------------------------------------------------------------------


class NB:
    def __init__(self, mat=None, tree=None):
        if mat is not None:
            mat.use_nodes = True
            self.nt = mat.node_tree
        else:
            self.nt = tree
        self.nt.nodes.clear()
        self.x = 0

    def vm(self, op, a, b=None, scale=None):
        nd = self.n('ShaderNodeVectorMath', operation=op)
        for i, v in enumerate((a, b)):
            if v is None:
                continue
            if isinstance(v, (tuple, list)):
                nd.inputs[i].default_value = v
            else:
                self.l(v, nd.inputs[i])
        if scale is not None:
            if isinstance(scale, (int, float)):
                nd.inputs['Scale'].default_value = scale
            else:
                self.l(scale, nd.inputs['Scale'])
        return nd.outputs['Value'] if op in ('DOT_PRODUCT', 'LENGTH', 'DISTANCE') else nd.outputs['Vector']

    def sep(self, v):
        nd = self.n('ShaderNodeSeparateXYZ')
        self.l(v, nd.inputs[0])
        return nd.outputs

    def n(self, tp, **kw):
        nd = self.nt.nodes.new(tp)
        nd.location = (self.x, 0)
        self.x += 60
        for k, v in kw.items():
            if k.startswith('i_'):
                key = k[2:]
                inp = nd.inputs[int(key)] if key.isdigit() else nd.inputs[key.replace('__', ' ')]
                inp.default_value = v
            else:
                setattr(nd, k, v)
        return nd

    def l(self, a, b):
        self.nt.links.new(a, b)

    def math(self, op, a, b=None, clamp=False):
        nd = self.n('ShaderNodeMath', operation=op, use_clamp=clamp)
        for i, v in enumerate((a, b)):
            if v is None:
                continue
            if isinstance(v, (int, float)):
                nd.inputs[i].default_value = v
            else:
                self.l(v, nd.inputs[i])
        return nd.outputs[0]

    def mix(self, fac, a, b, dtype='RGBA'):
        nd = self.n('ShaderNodeMix', data_type=dtype, clamp_factor=True)
        ins = {'FLOAT': (0, 2, 3), 'RGBA': (0, 6, 7)}[dtype]
        for i, v in zip(ins, (fac, a, b)):
            if isinstance(v, (int, float)):
                nd.inputs[i].default_value = v
            elif isinstance(v, tuple):
                nd.inputs[i].default_value = v
            else:
                self.l(v, nd.inputs[i])
        return nd.outputs[{'FLOAT': 0, 'RGBA': 2}[dtype]]

    def attr(self, name):
        return self.n('ShaderNodeAttribute', attribute_name=name)

    def smooth(self, v, a, b):
        nd = self.n('ShaderNodeMapRange', interpolation_type='SMOOTHSTEP', clamp=True)
        nd.inputs['From Min'].default_value = a
        nd.inputs['From Max'].default_value = b
        self.l(v, nd.inputs['Value'])
        return nd.outputs['Result']

    def vor(self, vec, scale, feature='DISTANCE_TO_EDGE', rand=1.0):
        nd = self.n('ShaderNodeTexVoronoi', feature=feature)
        nd.inputs['Scale'].default_value = scale
        nd.inputs['Randomness'].default_value = rand
        self.l(vec, nd.inputs['Vector'])
        return nd

    def noise(self, vec, scale, detail=3.0, rough=0.5):
        nd = self.n('ShaderNodeTexNoise')
        nd.inputs['Scale'].default_value = scale
        nd.inputs['Detail'].default_value = detail
        nd.inputs['Roughness'].default_value = rough
        self.l(vec, nd.inputs['Vector'])
        return nd


def lin(r, g, b):  # sRGB 0-255 -> linear
    f = lambda c: (c / 255.0) / 12.92 if c / 255.0 <= 0.04045 else ((c / 255.0 + 0.055) / 1.055) ** 2.4
    return (f(r), f(g), f(b), 1.0)


BACK = lin(66, 78, 92)       # slate blue-grey
BACK2 = lin(42, 50, 62)      # darker dorsal mottling
BELLY = lin(150, 110, 68)    # warm bronze
BELLY2 = lin(118, 82, 48)    # amber-brown
HORN0 = lin(42, 38, 34)
HORN1 = lin(186, 174, 146)
EYE = lin(236, 150, 30)


def named_attr(b, name, dtype='FLOAT'):
    nd = b.n('GeometryNodeInputNamedAttribute', data_type=dtype)
    nd.inputs['Name'].default_value = name
    return nd.outputs['Attribute']


def build_detail_nodes(level):
    """Geometry-nodes displacement that turns the subdivided game mesh into the
    high-res sculpt: imbricate (overlapping) scales sized by region, belly scutes,
    tubercles, skin folds at the joints/throat, keratin growth rings on horns and claws."""
    ng = bpy.data.node_groups.new('DragonDetail_L%d' % level, 'GeometryNodeTree')
    ng.interface.new_socket('Geometry', in_out='INPUT', socket_type='NodeSocketGeometry')
    ng.interface.new_socket('Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')
    b = NB(tree=ng)
    gin = b.n('NodeGroupInput')
    gout = b.n('NodeGroupOutput')
    # linear subdivision + positional blur (much faster than Catmull-Clark here, same smooth result)
    sub = b.n('GeometryNodeSubdivideMesh')
    sub.inputs['Level'].default_value = level
    b.l(gin.outputs[0], sub.inputs['Mesh'])
    blur = b.n('GeometryNodeBlurAttribute', data_type='FLOAT_VECTOR')
    blur.inputs['Iterations'].default_value = 2 + 2 * level
    b.l(b.n('GeometryNodeInputPosition').outputs[0], blur.inputs['Value'])
    smp = b.n('GeometryNodeSetPosition')
    b.l(sub.outputs[0], smp.inputs['Geometry'])
    b.l(blur.outputs['Value'], smp.inputs['Position'])
    pos = b.n('GeometryNodeInputPosition').outputs[0]
    nrm = b.n('GeometryNodeInputNormal').outputs[0]
    scl = named_attr(b, 'scl')
    scute = named_attr(b, 'scute')
    fold = named_attr(b, 'fold')
    dmask = named_attr(b, 'dmask')
    tip = named_attr(b, 'tip')
    flow = named_attr(b, 'flow', 'FLOAT_VECTOR')
    maskc = named_attr(b, 'mask', 'FLOAT_COLOR')
    kmask = b.sep(maskc)[1]
    pstr = b.vm('MULTIPLY', pos, (1.0, 0.8, 1.0))

    def layer(vec, sc, edge_w=0.22):
        ve = b.vor(vec, sc, 'DISTANCE_TO_EDGE')
        vf = b.vor(vec, sc, 'F1')
        e = b.smooth(ve.outputs['Distance'], 0.0, edge_w)
        o = b.vm('SCALE', b.vm('SUBTRACT', vec, vf.outputs['Position']), scale=sc)
        t = b.vm('DOT_PRODUCT', o, flow)
        tilt = b.math('ADD', 0.62, b.math('MULTIPLY', t, 0.95))
        tilt = b.math('MAXIMUM', tilt, 0.12)
        h = b.math('MULTIPLY', e, tilt)
        cell = b.sep(vf.outputs['Color'])[0]
        return h, cell

    hL, cL = layer(pstr, 7.5)
    hM, cM = layer(pstr, 14.0)
    hF, cF = layer(pstr, 30.0)
    psc = b.vm('MULTIPLY', pos, (0.42, 1.35, 1.0))
    hS, cS = layer(psc, 11.0, 0.22)
    buv = b.sep(named_attr(b, 'bodyuv', 'FLOAT_VECTOR'))
    psize = named_attr(b, 'psize')
    dorsal = named_attr(b, 'dorsal')
    headm = named_attr(b, 'headm')
    jit_n = b.noise(pos, 4.0, 2.0, 0.5).outputs['Fac']

    def grid(u, v, ln, wd, keel_amt):
        u = b.math('ADD', u, b.math('MULTIPLY', b.math('SUBTRACT', jit_n, 0.5), b.math('MULTIPLY', ln, 0.5)))
        ru = b.math('DIVIDE', u, ln)
        row = b.math('FLOOR', ru)
        fu = b.math('SUBTRACT', ru, row)
        par = b.math('FLOORED_MODULO', row, 2.0)
        rv = b.math('ADD', b.math('DIVIDE', v, wd), b.math('MULTIPLY', par, 0.5))
        col = b.math('FLOOR', rv)
        fv = b.math('SUBTRACT', rv, col)
        du = b.math('MULTIPLY', b.math('MINIMUM', fu, b.math('SUBTRACT', 1.0, fu)), ln)
        dv = b.math('MULTIPLY', b.math('MINIMUM', fv, b.math('SUBTRACT', 1.0, fv)), wd)
        de = b.math('MINIMUM', du, dv)
        plat = b.math('DIVIDE', de, b.math('MULTIPLY', b.math('MINIMUM', ln, wd), 0.2))
        plat = b.smooth(plat, 0.0, 1.0)
        kc = b.math('SUBTRACT', 1.0, b.math('MULTIPLY', b.math('ABSOLUTE', b.math('SUBTRACT', fv, 0.5)), 2.0))
        keel = b.math('MULTIPLY', b.math('POWER', kc, 3.0), fu)
        h = b.math('MULTIPLY', plat, b.math('ADD', b.math('ADD', 0.5, b.math('MULTIPLY', fu, 0.4)),
                                             b.math('MULTIPLY', keel, keel_amt)))
        cv = b.n('ShaderNodeCombineXYZ')
        b.l(row, cv.inputs[0])
        b.l(col, cv.inputs[1])
        wn = b.n('ShaderNodeTexWhiteNoise', noise_dimensions='3D')
        b.l(cv.outputs[0], wn.inputs['Vector'])
        return h, wn.outputs['Value']

    hD, cD = grid(buv[0], buv[1], b.math('MULTIPLY', psize, 0.9), psize, 1.4)
    hV, cV = grid(buv[0], buv[2], b.math('MULTIPLY', psize, 0.55), b.math('MULTIPLY', psize, 0.55), 0.0)
    # head: fine irregular scales plus sensory pits
    hH, cH = layer(pos, 58.0, 0.25)
    pits = b.vor(pos, 26.0, 'F1')
    pit = b.smooth(pits.outputs['Distance'], 0.12, 0.0)
    wA = b.math('MULTIPLY', scl, 2.0, clamp=True)
    wB = b.math('SUBTRACT', b.math('MULTIPLY', scl, 2.0), 1.0, clamp=True)
    H = b.mix(wB, b.mix(wA, hL, hM, 'FLOAT'), hF, 'FLOAT')
    C = b.mix(wB, b.mix(wA, cL, cM, 'FLOAT'), cF, 'FLOAT')
    amp = b.mix(wB, b.mix(wA, 0.009, 0.0058, 'FLOAT'), 0.003, 'FLOAT')
    H = b.mix(dorsal, H, hD, 'FLOAT')
    C = b.mix(dorsal, C, cD, 'FLOAT')
    amp = b.mix(dorsal, amp, b.math('MULTIPLY', psize, 0.16), 'FLOAT')
    sc2 = b.math('MULTIPLY', scute, 1.0)
    H = b.mix(sc2, H, hV, 'FLOAT')
    C = b.mix(sc2, C, cV, 'FLOAT')
    amp = b.mix(sc2, amp, 0.0028, 'FLOAT')
    H = b.mix(headm, H, b.math('SUBTRACT', hH, b.math('MULTIPLY', pit, 0.6)), 'FLOAT')
    C = b.mix(headm, C, cH, 'FLOAT')
    amp = b.mix(headm, amp, 0.0022, 'FLOAT')
    disp = b.math('MULTIPLY', H, amp)
    # tubercles / pebbling on each scale
    tub = b.noise(pos, 160.0, 2.0, 0.5).outputs['Fac']
    disp = b.math('ADD', disp, b.math('MULTIPLY', b.math('SUBTRACT', tub, 0.5), 0.0007))
    # skin folds: creases running across the local body axis, broken up by noise
    ph = b.vm('DOT_PRODUCT', pos, flow)
    nz = b.noise(pos, 3.5, 3.0, 0.5).outputs['Fac']
    ph = b.math('ADD', b.math('MULTIPLY', ph, 75.0), b.math('MULTIPLY', nz, 9.0))
    wv = b.math('MULTIPLY', b.math('SINE', ph), 0.5)
    wv = b.math('POWER', b.math('ADD', wv, 0.5), 4.0)
    nz2 = b.noise(pos, 9.0, 2.0, 0.5).outputs['Fac']
    foldw = b.math('MULTIPLY', b.math('MULTIPLY', fold, wv), b.smooth(nz2, 0.3, 0.6))
    disp = b.math('SUBTRACT', disp, b.math('MULTIPLY', foldw, 0.009))
    disp = b.math('MULTIPLY', disp, dmask)
    # keratin: growth rings + longitudinal striation
    # keratin: irregular growth rings, lengthwise fibrous grooves, chipped worn tips
    karound = named_attr(b, 'karound')
    kn1 = b.noise(pos, 6.0, 2.0).outputs['Fac']
    rings = b.math('MULTIPLY', b.math('SINE', b.math('ADD', b.math('MULTIPLY', tip, 110.0), b.math('MULTIPLY', kn1, 5.0))), 0.0011)
    kn2 = b.noise(pos, 20.0, 2.0).outputs['Fac']
    gr = b.math('SINE', b.math('ADD', b.math('MULTIPLY', karound, 2 * math.pi * 18), b.math('MULTIPLY', kn2, 1.5)))
    grooves = b.math('MULTIPLY', b.math('POWER', b.math('ADD', b.math('MULTIPLY', gr, 0.5), 0.5), 2.0), -0.0009)
    chip = b.math('MULTIPLY', b.smooth(b.noise(pos, 45.0, 2.0).outputs['Fac'], 0.55, 0.7), b.smooth(tip, 0.7, 0.95))
    kd = b.math('ADD', b.math('MULTIPLY', b.math('ADD', rings, grooves), b.math('SUBTRACT', 1.0, b.math('MULTIPLY', tip, 0.6))),
                b.math('MULTIPLY', chip, -0.002))
    kdisp = b.math('MULTIPLY', kd, kmask)
    disp = b.math('ADD', disp, kdisp)
    # membrane: raised blood vessels + shallow stretch striations (matches the HP_Wing shader pattern)
    memb = named_attr(b, 'memb')
    ps = b.sep(pos)
    mv = b.n('ShaderNodeCombineXYZ')
    b.l(b.math('ABSOLUTE', ps[0]), mv.inputs[0])
    b.l(ps[1], mv.inputs[1])
    mvv = mv.outputs[0]
    mnz = b.noise(mvv, 0.8, 3.0, 0.5)
    mw = b.n('ShaderNodeMix', data_type='VECTOR', clamp_factor=True)
    mw.inputs[0].default_value = 0.25
    b.l(mvv, mw.inputs[4])
    b.l(mnz.outputs['Color'], mw.inputs[5])
    mwv = mw.outputs[1]
    mv1 = b.vor(mwv, 1.4, 'DISTANCE_TO_EDGE')
    vm1 = b.math('SUBTRACT', 1.0, b.smooth(mv1.outputs['Distance'], 0.0, 0.024))
    mv2 = b.vor(b.vm('ADD', mwv, (3.1, 7.7, 0.0)), 4.0, 'DISTANCE_TO_EDGE')
    vm2 = b.math('MULTIPLY', b.math('SUBTRACT', 1.0, b.smooth(mv2.outputs['Distance'], 0.0, 0.02)), 0.5)
    mwave = b.n('ShaderNodeTexWave', wave_type='BANDS', bands_direction='Y', wave_profile='SIN')
    mwave.inputs['Scale'].default_value = 9.0
    mwave.inputs['Distortion'].default_value = 7.0
    mwave.inputs['Detail'].default_value = 3.0
    mwave.inputs['Detail Scale'].default_value = 1.5
    b.l(mwv, mwave.inputs['Vector'])
    mstri = b.smooth(mwave.outputs['Fac'], 0.7, 1.0)
    mthin = named_attr(b, 'mthin')
    mdisp = b.math('SUBTRACT', b.math('MULTIPLY', b.math('MAXIMUM', vm1, vm2), 0.0045), b.math('MULTIPLY', mstri, 0.0009))
    # slack folds in the inner panel / near the body, fading out toward the wingtip
    slack = b.n('ShaderNodeTexWave', wave_type='BANDS', bands_direction='X', wave_profile='SIN')
    slack.inputs['Scale'].default_value = 2.2
    slack.inputs['Distortion'].default_value = 4.0
    slack.inputs['Detail'].default_value = 1.0
    b.l(mvv, slack.inputs['Vector'])
    sl = b.math('POWER', slack.outputs['Fac'], 3.0)
    slm = b.math('MULTIPLY', mthin, b.math('SUBTRACT', 1.0, b.smooth(b.math('ABSOLUTE', ps[0]), 1.2, 4.0)))
    slnz = b.noise(mvv, 1.3, 2.0).outputs['Fac']
    slack_z = b.math('MULTIPLY', b.math('MULTIPLY', b.math('MULTIPLY', sl, slm), b.smooth(slnz, 0.3, 0.65)), -0.009)
    slack_z = b.math('MULTIPLY', slack_z, memb)   # applied along world Z so both membrane shells move together
    disp = b.math('ADD', disp, b.math('MULTIPLY', mdisp, memb))

    st1 = b.n('GeometryNodeStoreNamedAttribute', data_type='FLOAT', domain='POINT')
    st1.inputs['Name'].default_value = 'hgt'
    b.l(smp.outputs[0], st1.inputs['Geometry'])
    b.l(b.math('MULTIPLY', H, dmask), st1.inputs['Value'])
    st2 = b.n('GeometryNodeStoreNamedAttribute', data_type='FLOAT', domain='POINT')
    st2.inputs['Name'].default_value = 'cell'
    b.l(st1.outputs[0], st2.inputs['Geometry'])
    b.l(C, st2.inputs['Value'])
    st3 = b.n('GeometryNodeStoreNamedAttribute', data_type='FLOAT', domain='POINT')
    st3.inputs['Name'].default_value = 'foldw'
    b.l(st2.outputs[0], st3.inputs['Geometry'])
    b.l(foldw, st3.inputs['Value'])
    sp = b.n('GeometryNodeSetPosition')
    b.l(st3.outputs[0], sp.inputs['Geometry'])
    zoff = b.n('ShaderNodeCombineXYZ')
    b.l(slack_z, zoff.inputs[2])
    b.l(b.vm('ADD', b.vm('SCALE', nrm, scale=disp), zoff.outputs[0]), sp.inputs['Offset'])
    b.l(sp.outputs[0], gout.inputs[0])
    return ng


def build_hp_body_material(mat):
    """Full-detail skin shader for the high-res mesh (baked down to the game mesh)."""
    b = NB(mat)
    tc = b.n('ShaderNodeTexCoord')
    oc = tc.outputs['Object']
    A = lambda n: b.attr(n).outputs['Fac']
    belly, hgt, cell, foldw, tip, scute = A('belly'), A('hgt'), A('cell'), A('foldw'), A('tip'), A('scute')
    mask = b.sep(b.attr('mask').outputs['Vector'])
    mR, mG, mB = mask[0], mask[1], mask[2]
    geo = b.n('ShaderNodeNewGeometry')
    point = geo.outputs['Pointiness']
    ao = b.n('ShaderNodeAmbientOcclusion', samples=16)
    ao.inputs['Distance'].default_value = 0.04
    aoF = ao.outputs['AO']
    cav = b.smooth(point, 0.505, 0.44)      # 1 in creases
    edge = b.smooth(point, 0.515, 0.6)      # 1 on sharp convex edges
    # countershading with a broken, mottled transition
    nz = b.noise(oc, 2.6, 5.0, 0.62).outputs['Fac']
    bf = b.math('ADD', belly, b.math('MULTIPLY', b.math('SUBTRACT', nz, 0.5), 0.5))
    bf = b.smooth(bf, 0.25, 0.85)
    blot = b.noise(oc, 0.9, 3.0, 0.55).outputs['Fac']
    back = b.mix(b.smooth(blot, 0.35, 0.7), BACK2, BACK)
    # dorsal iridescence: per-scale hue drift between teal and violet-grey
    iri = b.mix(b.smooth(cell, 0.2, 0.8), lin(56, 82, 92), lin(74, 68, 98))
    back = b.mix(b.math('MULTIPLY', b.math('SUBTRACT', 1.0, bf), 0.32), back, iri)
    bel = b.mix(b.smooth(cell, 0.15, 0.95), BELLY2, BELLY)
    bel = b.mix(b.math('MULTIPLY', scute, 0.35), bel, lin(168, 128, 80))
    col = b.mix(bf, back, bel)
    # per-scale value jitter; raised scale centres lighter, interstitial skin darker
    jit = b.math('ADD', 0.86, b.math('MULTIPLY', cell, 0.26))
    crev = b.math('MULTIPLY', jit, b.math('ADD', 0.55, b.math('MULTIPLY', b.math('MINIMUM', hgt, 1.0), 0.5)))
    g3 = b.n('ShaderNodeCombineColor')
    for i in range(3):
        b.l(crev, g3.inputs[i])
    mul = b.n('ShaderNodeMix', data_type='RGBA', blend_type='MULTIPLY', clamp_factor=True)
    mul.inputs[0].default_value = 1.0
    b.l(col, mul.inputs[6])
    b.l(g3.outputs[0], mul.inputs[7])
    col = mul.outputs[2]
    # dirt in crevices and folds, wear on plate edges
    dirt = b.math('ADD', b.math('MULTIPLY', b.math('SUBTRACT', 1.0, aoF), 0.9), b.math('MULTIPLY', cav, 0.45))
    dirt = b.math('ADD', dirt, b.math('MULTIPLY', foldw, 0.6))
    dn = b.noise(oc, 6.0, 3.0, 0.6).outputs['Fac']
    dirt = b.math('MULTIPLY', dirt, b.math('ADD', 0.5, dn), clamp=True)
    col = b.mix(b.math('MULTIPLY', dirt, 0.75), col, lin(34, 30, 26))
    wear = b.math('MULTIPLY', b.math('MAXIMUM', edge, b.smooth(hgt, 0.85, 1.25)), 0.35)
    col = b.mix(wear, col, lin(118, 118, 112))
    # keratin (horns, claws, spines, teeth): dark base -> pale worn tip, streaky
    kst = b.noise(b.vm('MULTIPLY', oc, (60, 60, 60)), 1.0, 3.0, 0.6).outputs['Fac']
    kcol = b.mix(b.smooth(tip, 0.2, 0.95), HORN0, HORN1)
    kcol = b.mix(b.math('MULTIPLY', kst, 0.35), kcol, lin(70, 58, 44))
    kar = b.attr('karound').outputs['Fac']
    kgr = b.math('SINE', b.math('ADD', b.math('MULTIPLY', kar, 2 * math.pi * 18), b.math('MULTIPLY', b.noise(oc, 20.0, 2.0).outputs['Fac'], 1.5)))
    kcol = b.mix(b.math('MULTIPLY', b.smooth(kgr, 0.2, 1.0), 0.3), kcol, lin(30, 27, 24))   # fibre streaks
    kcol = b.mix(b.math('MULTIPLY', cav, 0.6), kcol, lin(20, 18, 16))
    col = b.mix(mG, col, kcol)
    # tail fin: darker membrane tone
    col = b.mix(b.math('MULTIPLY', mB, 0.75), col, lin(34, 37, 48))
    # eye: amber iris with radial fibres, dark limbal ring, vertical slit pupil
    eq = b.sep(b.attr('eyeq').outputs['Vector'])
    ex, ey, ez = eq[0], eq[1], eq[2]
    er = b.math('SQRT', b.math('ADD', b.math('MULTIPLY', ex, ex), b.math('MULTIPLY', ey, ey)))
    fib_v = b.n('ShaderNodeCombineXYZ')
    b.l(b.math('DIVIDE', ex, b.math('MAXIMUM', er, 0.02)), fib_v.inputs[0])
    b.l(b.math('DIVIDE', ey, b.math('MAXIMUM', er, 0.02)), fib_v.inputs[1])
    b.l(b.math('MULTIPLY', er, 0.6), fib_v.inputs[2])
    fib = b.noise(fib_v.outputs[0], 7.0, 4.0, 0.65).outputs['Fac']
    iris = b.mix(b.smooth(fib, 0.3, 0.75), lin(150, 70, 12), lin(250, 172, 40))
    iris = b.mix(b.smooth(er, 0.35, 0.05), iris, lin(255, 214, 90))      # gold flare round the pupil
    iris = b.mix(b.smooth(er, 0.62, 0.86), iris, lin(40, 18, 6))         # limbal ring
    slit_w = b.math('MULTIPLY', 0.12, b.math('SQRT', b.math('MAXIMUM', b.math('SUBTRACT', 1.0,
                    b.math('POWER', b.math('DIVIDE', ey, 0.8), 2.0)), 0.0)))
    pupil = b.math('MULTIPLY', b.smooth(b.math('SUBTRACT', b.math('ABSOLUTE', ex), slit_w), 0.025, -0.02),
                   b.math('GREATER_THAN', ez, 0.0))
    iris = b.mix(pupil, iris, (0.003, 0.003, 0.003, 1.0))
    eye = b.smooth(mR, 0.5, 0.9)
    col = b.mix(eye, col, iris)
    # roughness: matte worn scale tops, dirty crevices, glossier belly, polished keratin, wet eyes
    rough = b.math('SUBTRACT', 0.70, b.math('MULTIPLY', b.math('MINIMUM', hgt, 1.2), 0.2))   # glossy caps, matte crevices
    rough = b.math('ADD', rough, b.math('MULTIPLY', dirt, 0.2))
    rough = b.mix(b.math('MULTIPLY', bf, 0.6), rough, 0.42, 'FLOAT')
    rough = b.math('ADD', rough, b.math('MULTIPLY', b.math('SUBTRACT', cell, 0.5), 0.16))
    rough = b.math('ADD', rough, b.math('MULTIPLY', b.math('SUBTRACT', b.noise(oc, 12.0, 2.0).outputs['Fac'], 0.5), 0.12))
    rough = b.mix(mG, rough, b.math('ADD', 0.36, b.math('MULTIPLY', b.smooth(tip, 0.7, 1.0), 0.32)), 'FLOAT')  # matte worn tips
    rough = b.mix(eye, rough, 0.03, 'FLOAT')
    # micro detail on top of the displaced geometry
    grain = b.noise(oc, 420.0, 2.0, 0.5).outputs['Fac']
    hh = b.math('MULTIPLY', grain, b.math('SUBTRACT', 1.0, eye))
    bump = b.n('ShaderNodeBump')
    bump.inputs['Distance'].default_value = 0.0015
    bump.inputs['Strength'].default_value = 0.25
    b.l(hh, bump.inputs['Height'])
    bsdf = b.n('ShaderNodeBsdfPrincipled')
    b.l(col, bsdf.inputs['Base Color'])
    b.l(b.math('MINIMUM', b.math('MAXIMUM', rough, 0.02), 1.0), bsdf.inputs['Roughness'])
    b.l(bump.outputs['Normal'], bsdf.inputs['Normal'])
    out = b.n('ShaderNodeOutputMaterial')
    b.l(bsdf.outputs[0], out.inputs[0])
    # cavity output for the cavity bake (wired in on demand)
    em = b.n('ShaderNodeEmission')
    em.name = 'CAVITY_EMIT'
    cv = b.math('SUBTRACT', 1.0, b.math('MULTIPLY', b.math('ADD', cav, b.math('MULTIPLY', foldw, 0.5)), 0.8), clamp=True)
    b.l(cv, em.inputs['Color'])
    return b


def build_hp_wing_material(mat):
    b = NB(mat)
    tc = b.n('ShaderNodeTexCoord')
    oc = tc.outputs['Object']
    sp = b.sep(oc)
    ax = b.math('ABSOLUTE', sp[0])
    comb = b.n('ShaderNodeCombineXYZ')
    b.l(ax, comb.inputs[0])
    b.l(sp[1], comb.inputs[1])
    v = comb.outputs[0]
    nz = b.noise(v, 0.8, 3.0, 0.5)
    warp = b.n('ShaderNodeMix', data_type='VECTOR', clamp_factor=True)
    warp.inputs[0].default_value = 0.25
    b.l(v, warp.inputs[4])
    b.l(nz.outputs['Color'], warp.inputs[5])
    wv = warp.outputs[1]
    # branching blood vessels at two scales
    v1 = b.vor(wv, 1.4, 'DISTANCE_TO_EDGE')
    vmask = b.math('SUBTRACT', 1.0, b.smooth(v1.outputs['Distance'], 0.0, 0.016))
    v2 = b.vor(b.vm('ADD', wv, (3.1, 7.7, 0.0)), 4.0, 'DISTANCE_TO_EDGE')
    vmask2 = b.math('MULTIPLY', b.math('SUBTRACT', 1.0, b.smooth(v2.outputs['Distance'], 0.0, 0.02)), 0.5)
    allv = b.math('MAXIMUM', vmask, vmask2)
    # stretch striations roughly parallel to the trailing edge
    wave = b.n('ShaderNodeTexWave', wave_type='BANDS', bands_direction='Y', wave_profile='SIN')
    wave.inputs['Scale'].default_value = 9.0
    wave.inputs['Distortion'].default_value = 7.0
    wave.inputs['Detail'].default_value = 3.0
    wave.inputs['Detail Scale'].default_value = 1.5
    b.l(wv, wave.inputs['Vector'])
    stri = b.smooth(wave.outputs['Fac'], 0.7, 1.0)
    mott = b.noise(v, 1.5, 5.0, 0.62).outputs['Fac']
    base = b.mix(b.smooth(mott, 0.3, 0.75), lin(30, 28, 33), lin(54, 45, 45))
    ty = b.smooth(sp[1], 0.3, 2.8)
    base = b.mix(b.math('MULTIPLY', ty, 0.35), base, lin(84, 58, 42))
    mthin = b.attr('mthin').outputs['Fac']
    base = b.mix(b.math('MULTIPLY', mthin, 0.55), base, lin(92, 58, 42))            # thin = warmer
    base = b.mix(b.math('MULTIPLY', b.math('SUBTRACT', 1.0, mthin), 0.65), base, lin(17, 18, 23))  # thick = darker
    col = b.mix(b.math('MULTIPLY', allv, 0.8), base, lin(60, 24, 22))   # vessels read red-brown
    col = b.mix(b.math('MULTIPLY', stri, 0.25), col, lin(16, 17, 22))
    spot = b.noise(v, 22.0, 2.0, 0.5).outputs['Fac']
    col = b.mix(b.math('MULTIPLY', b.smooth(spot, 0.62, 0.75), 0.4), col, lin(20, 21, 26))
    wr = b.noise(v, 45.0, 3.0, 0.6).outputs['Fac']
    sc = b.vor(v, 140.0, 'DISTANCE_TO_EDGE')
    sch = b.smooth(sc.outputs['Distance'], 0.0, 0.25)
    hh = b.math('ADD', b.math('MULTIPLY', allv, 0.5), b.math('MULTIPLY', stri, 0.2))
    hh = b.math('ADD', hh, b.math('ADD', b.math('MULTIPLY', wr, 0.25), b.math('MULTIPLY', sch, 0.12)))
    bump = b.n('ShaderNodeBump')
    bump.inputs['Distance'].default_value = 0.004
    bump.inputs['Strength'].default_value = 0.55
    b.l(hh, bump.inputs['Height'])
    rough = b.math('ADD', 0.30, b.math('MULTIPLY', wr, 0.14))
    rough = b.math('ADD', rough, b.math('MULTIPLY', b.math('SUBTRACT', 1.0, mthin), 0.15))
    rough = b.mix(stri, rough, 0.5, 'FLOAT')
    bsdf = b.n('ShaderNodeBsdfPrincipled')
    b.l(col, bsdf.inputs['Base Color'])
    b.l(rough, bsdf.inputs['Roughness'])
    b.l(bump.outputs['Normal'], bsdf.inputs['Normal'])
    out = b.n('ShaderNodeOutputMaterial')
    b.l(bsdf.outputs[0], out.inputs[0])
    em = b.n('ShaderNodeEmission')
    em.name = 'CAVITY_EMIT'
    # translucency / thinness mask (white = thin, lets light through)
    b.l(b.math('MULTIPLY', b.math('POWER', mthin, 0.8), b.math('SUBTRACT', 1.0, b.math('MULTIPLY', allv, 0.6)), clamp=True),
        em.inputs['Color'])
    return b


HP_BODY = bpy.data.materials.new('HP_Body')
HP_WING = bpy.data.materials.new('HP_Wing')
build_hp_body_material(HP_BODY)
build_hp_wing_material(HP_WING)


def make_highres(levels):
    hp = link(bpy.data.objects.new('SK_Dragon_HighRes', dragon.data.copy()))
    hp.data.materials.clear()
    hp.data.materials.append(HP_BODY)
    hp.data.materials.append(HP_WING)
    g = hp.modifiers.new('detail', 'NODES')
    g.node_group = build_detail_nodes(levels)
    return hp


# ---------------------------------------------------------------------------
# Render engine setup
# ---------------------------------------------------------------------------
scene.render.engine = 'CYCLES'
try:
    prefs = bpy.context.preferences.addons['cycles'].preferences
    prefs.compute_device_type = 'METAL'
    prefs.get_devices()
    for dv in prefs.devices:
        dv.use = True
    scene.cycles.device = 'GPU'
    log('cycles device GPU (Metal)')
except Exception as e:  # pragma: no cover
    log('GPU unavailable, CPU', e)

# ---------------------------------------------------------------------------
# Bake (high-res detail -> game mesh)
# ---------------------------------------------------------------------------
TEX_FILES = {}


def new_img(name, res, noncolor):
    img = bpy.data.images.new(name, res, res, alpha=False, float_buffer=False)
    img.colorspace_settings.name = 'Non-Color' if noncolor else 'sRGB'
    return img


def bake_pass(kind, imgs, samples=1, **kw):
    for mat, img in imgs.items():
        nt = mat.node_tree
        nd = nt.nodes.get('BAKE_TARGET') or nt.nodes.new('ShaderNodeTexImage')
        nd.name = 'BAKE_TARGET'
        nd.image = img
        for n2 in nt.nodes:
            n2.select = False
        nt.nodes.active = nd
        nd.select = True
    scene.cycles.samples = samples
    t = time.time()
    bpy.ops.object.bake(type=kind, margin=24, margin_type='EXTEND', use_clear=True,
                        use_selected_to_active=True, cage_extrusion=0.016, max_ray_distance=0.045, **kw)
    log('baked', kind, '%.1fs' % (time.time() - t))


def pix(img):
    a = np.empty(img.size[0] * img.size[1] * 4, np.float32)
    img.pixels.foreach_get(a)
    return a.reshape(img.size[1], img.size[0], 4)


def save_png(arr, name, noncolor):
    h, w = arr.shape[:2]
    img = bpy.data.images.new(name, w, h, alpha=False, float_buffer=False)
    img.colorspace_settings.name = 'Non-Color' if noncolor else 'sRGB'
    img.pixels.foreach_set(arr.astype(np.float32).ravel())
    path = os.path.join(TEXDIR, name + '.png')
    img.filepath_raw = path
    img.file_format = 'PNG'
    img.save()
    TEX_FILES[name] = path
    bpy.data.images.remove(img)
    return path


def save_img(img, name):
    path = os.path.join(TEXDIR, name + '.png')
    img.filepath_raw = path
    img.file_format = 'PNG'
    img.save()
    TEX_FILES[name] = path


def set_cavity_output(on):
    for mat in (HP_BODY, HP_WING):
        nt = mat.node_tree
        out = [n for n in nt.nodes if n.type == 'OUTPUT_MATERIAL'][0]
        src = nt.nodes['CAVITY_EMIT'] if on else [n for n in nt.nodes if n.type == 'BSDF_PRINCIPLED'][0]
        nt.links.new(src.outputs[0], out.inputs[0])


def do_bake(hp):
    for o in scene.objects:
        o.select_set(o in (dragon, hp))
    bpy.context.view_layer.objects.active = dragon
    res = TEXRES
    mats = {'Body': M_BODY, 'Wing': M_WING}
    for m in mats.values():
        m.use_nodes = True
    imgs = {m: new_img('bk_%s_D' % k, res, False) for k, m in mats.items()}
    bake_pass('DIFFUSE', imgs, samples=4, pass_filter={'COLOR'})
    for k, m in mats.items():
        save_img(imgs[m], 'T_Dragon_%s_D' % k)
    imgs_r = {m: new_img('bk_%s_R' % k, res, True) for k, m in mats.items()}
    bake_pass('ROUGHNESS', imgs_r, samples=4)
    imgs_n = {m: new_img('bk_%s_N' % k, res, True) for k, m in mats.items()}
    bake_pass('NORMAL', imgs_n, samples=8, normal_space='TANGENT', normal_r='POS_X', normal_g='POS_Y',
              normal_b='POS_Z')
    imgs_ao = {m: new_img('bk_%s_AO' % k, res, True) for k, m in mats.items()}
    bake_pass('AO', imgs_ao, samples=opt('--ao-samples', 48, int))
    set_cavity_output(True)
    imgs_c = {m: new_img('bk_%s_C' % k, res, True) for k, m in mats.items()}
    bake_pass('EMIT', imgs_c, samples=4)
    set_cavity_output(False)
    for k, m in mats.items():
        n = pix(imgs_n[m]).copy()
        n[..., 1] = 1.0 - n[..., 1]     # OpenGL -> DirectX (UE convention)
        save_png(n, 'T_Dragon_%s_N' % k, True)
        cav = pix(imgs_c[m])[..., 0]
        save_png(np.stack([cav, cav, cav, np.ones_like(cav)], -1),
                 'T_Dragon_%s_%s' % (k, 'Cavity' if k == 'Body' else 'Translucency'), True)
        ao = pix(imgs_ao[m])[..., 0]
        ao = np.clip((0.3 + 0.7 * ao) * ((0.75 + 0.25 * cav) if k == 'Body' else 1.0), 0, 1)
        r = pix(imgs_r[m])[..., 0]
        arm = np.stack([ao, r, np.zeros_like(r), np.ones_like(r)], -1)
        save_png(arm, 'T_Dragon_%s_ARM' % k, True)
        for im in (imgs[m], imgs_r[m], imgs_n[m], imgs_ao[m], imgs_c[m]):
            bpy.data.images.remove(im)


def final_material(mat, key, translucent=False):
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()

    def tex(name, noncolor, y):
        img = bpy.data.images.load(TEX_FILES[name], check_existing=True)
        img.colorspace_settings.name = 'Non-Color' if noncolor else 'sRGB'
        nd = nt.nodes.new('ShaderNodeTexImage')
        nd.image = img
        nd.location = (-900, y)
        return nd
    d = tex('T_Dragon_%s_D' % key, False, 300)
    n = tex('T_Dragon_%s_N' % key, True, -300)
    a = tex('T_Dragon_%s_ARM' % key, True, 0)
    bsdf = nt.nodes.new('ShaderNodeBsdfPrincipled')
    out = nt.nodes.new('ShaderNodeOutputMaterial')
    out.location = (400, 0)
    sp = nt.nodes.new('ShaderNodeSeparateColor')
    sp.location = (-500, 0)
    nt.links.new(a.outputs[0], sp.inputs[0])
    # AO multiplied into colour for the Blender preview (UE uses the AO channel itself)
    mul = nt.nodes.new('ShaderNodeMix')
    mul.data_type = 'RGBA'
    mul.blend_type = 'MULTIPLY'
    mul.inputs[0].default_value = 1.0
    nt.links.new(d.outputs[0], mul.inputs[6])
    nt.links.new(sp.outputs[0], mul.inputs[7])
    nt.links.new(mul.outputs[2], bsdf.inputs['Base Color'])
    nt.links.new(sp.outputs[1], bsdf.inputs['Roughness'])
    nt.links.new(sp.outputs[2], bsdf.inputs['Metallic'])
    s2 = nt.nodes.new('ShaderNodeSeparateColor')
    nt.links.new(n.outputs[0], s2.inputs[0])
    inv = nt.nodes.new('ShaderNodeMath')
    inv.operation = 'SUBTRACT'
    inv.inputs[0].default_value = 1.0
    nt.links.new(s2.outputs[1], inv.inputs[1])
    c2 = nt.nodes.new('ShaderNodeCombineColor')
    nt.links.new(s2.outputs[0], c2.inputs[0])
    nt.links.new(inv.outputs[0], c2.inputs[1])
    nt.links.new(s2.outputs[2], c2.inputs[2])
    nm = nt.nodes.new('ShaderNodeNormalMap')
    nt.links.new(c2.outputs[0], nm.inputs['Color'])
    nt.links.new(nm.outputs[0], bsdf.inputs['Normal'])
    if translucent:
        # preview-only stand-in for UE's two-sided foliage / subsurface membrane
        tr = nt.nodes.new('ShaderNodeBsdfTranslucent')
        tint = nt.nodes.new('ShaderNodeMix')
        tint.data_type = 'RGBA'
        tint.blend_type = 'MULTIPLY'
        tint.inputs[0].default_value = 1.0
        nt.links.new(d.outputs[0], tint.inputs[6])
        tint.inputs[7].default_value = (9.0, 4.6, 3.0, 1.0)
        nt.links.new(tint.outputs[2], tr.inputs['Color'])
        nt.links.new(nm.outputs[0], tr.inputs['Normal'])
        mxs = nt.nodes.new('ShaderNodeMixShader')
        tm = tex('T_Dragon_Wing_Translucency', True, -600)
        tmm = nt.nodes.new('ShaderNodeMath')
        tmm.operation = 'MULTIPLY'
        tmm.inputs[1].default_value = 0.45
        nt.links.new(tm.outputs[0], tmm.inputs[0])
        nt.links.new(tmm.outputs[0], mxs.inputs[0])
        nt.links.new(bsdf.outputs[0], mxs.inputs[1])
        nt.links.new(tr.outputs[0], mxs.inputs[2])
        nt.links.new(mxs.outputs[0], out.inputs[0])
        return
    else:
        bsdf.inputs['Subsurface Weight'].default_value = 0.15
        bsdf.inputs['Subsurface Radius'].default_value = (1.0, 0.35, 0.2)
        bsdf.inputs['Subsurface Scale'].default_value = 0.025
        bsdf.inputs['Sheen Weight'].default_value = 0.12
        bsdf.inputs['Sheen Roughness'].default_value = 0.35
    nt.links.new(bsdf.outputs[0], out.inputs[0])


HP_LEVELS = opt('--hp-levels', 2, int)
if not QUICK:
    hp = make_highres(HP_LEVELS)
    dg = bpy.context.evaluated_depsgraph_get()
    hpe = hp.evaluated_get(dg)
    HP_TRIS = sum(len(p.vertices) - 2 for p in hpe.data.polygons)
    log('high-res detail mesh triangles', HP_TRIS)
    do_bake(hp)
    final_material(M_BODY, 'Body')
    final_material(M_WING, 'Wing', translucent=True)
    log('textures written')
    hp.hide_render = True
    hp.hide_set(True)
else:
    HP_TRIS = 0
    # quick look: put the detail shaders + displacement straight on the game mesh
    dragon.data.materials[0] = HP_BODY
    dragon.data.materials[1] = HP_WING
    g = dragon.modifiers.new('detail', 'NODES')
    g.node_group = build_detail_nodes(1)


# ---------------------------------------------------------------------------
# Armature + weights
# ---------------------------------------------------------------------------
arm_data = bpy.data.armatures.new('Armature')
arm = link(bpy.data.objects.new('Armature', arm_data))
arm_data.display_type = 'STICK'
bpy.context.view_layer.objects.active = arm
for o in scene.objects:
    o.select_set(o == arm)
bpy.ops.object.mode_set(mode='EDIT')
ebs = {}
for nm, hd, tl, par, rollref in BONES:
    eb = arm_data.edit_bones.new(nm)
    eb.head = hd
    eb.tail = tl
    eb.align_roll(Vector(rollref))
    eb.use_deform = True
    ebs[nm] = eb
for nm, hd, tl, par, rollref in BONES:
    if par:
        ebs[nm].parent = ebs[par]
        ebs[nm].use_connect = (Vector(ebs[par].tail) - Vector(hd)).length < 1e-5 and nm not in ('jaw',)
bpy.ops.object.mode_set(mode='OBJECT')
log('armature bones', len(arm_data.bones))

for nm in BONE_NAMES:
    dragon.vertex_groups.new(name=nm)
vgs = dragon.vertex_groups
nz_i, nz_j = np.nonzero(W8)
for bi in range(len(BONE_NAMES)):
    sel = nz_j == bi
    if not sel.any():
        continue
    vi = nz_i[sel]
    ww = W8[vi, bi]
    vg = vgs[BONE_NAMES[bi]]
    # group by quantised weight for speed
    q = np.round(ww * 1000).astype(int)
    for qv in np.unique(q):
        ids = vi[q == qv].tolist()
        vg.add(ids, qv / 1000.0, 'REPLACE')
dragon.parent = arm
mod = dragon.modifiers.new('Armature', 'ARMATURE')
mod.object = arm
log('weights assigned')

# remove helper attributes that are not needed in the game mesh
for nm in ('wclass', 'rbone', 'tip', 'belly', 'scl', 'mask', 'fold', 'scute', 'dmask', 'flow', 'eyeq', 'dorsal', 'psize', 'bodyuv', 'headm', 'memb', 'hasuv', 'mthin', 'karound'):
    if not QUICK and nm in me.attributes:
        me.attributes.remove(me.attributes[nm])

# ---------------------------------------------------------------------------
# Preview scene
# ---------------------------------------------------------------------------
HDRI_DIR = os.path.join(REPO, 'Data', 'Downloads', 'HDRI')
HDRIS = {'sunset': 'kloppenheim_06_puresky_4k.hdr', 'overcast': 'overcast_soil_puresky_4k.hdr'}
_HDRI_SUN = {}


def hdri_sun_azimuth(img):
    if img.name in _HDRI_SUN:
        return _HDRI_SUN[img.name]
    w, h = img.size
    px = np.empty(w * h * 4, np.float32)
    img.pixels.foreach_get(px)
    lum = px.reshape(h, w, 4)[..., :3].sum(-1)
    iy, ix = np.unravel_index(np.argmax(lum), lum.shape)
    u = (ix + 0.5) / w
    phi = (u - 0.5) * 2 * math.pi
    az = math.atan2(math.sin(phi), -math.cos(phi))
    _HDRI_SUN[img.name] = az
    log('HDRI', img.name, 'sun azimuth %.1f deg, elevation %.1f deg' % (math.degrees(az), ((iy + 0.5) / h - 0.5) * 180))
    return az


def setup_preview_scene():
    world = bpy.data.worlds.new('W')
    scene.world = world
    world.use_nodes = True
    nt = world.node_tree
    bg = nt.nodes['Background']
    tc = nt.nodes.new('ShaderNodeTexCoord')
    mp = nt.nodes.new('ShaderNodeMapping')
    mp.name = 'HDRI_MAP'
    env = nt.nodes.new('ShaderNodeTexEnvironment')
    env.name = 'HDRI'
    nt.links.new(tc.outputs['Generated'], mp.inputs['Vector'])
    nt.links.new(mp.outputs['Vector'], env.inputs['Vector'])
    nt.links.new(env.outputs['Color'], bg.inputs['Color'])
    bg.inputs['Strength'].default_value = 1.0
    # rim light (third point of the key/fill/rim setup; key = HDRI sun, fill = sky)
    rl = bpy.data.lights.new('Rim', 'AREA')
    rl.size = 4.0
    rl.color = (0.85, 0.92, 1.0)
    link(bpy.data.objects.new('Rim', rl))
    gm = bpy.data.meshes.new('Ground')
    gm.from_pydata([(-80, -80, 0), (80, -80, 0), (80, 80, 0), (-80, 80, 0)], [], [(0, 1, 2, 3)])
    go = link(bpy.data.objects.new('Ground', gm))
    gmat = bpy.data.materials.new('Ground')
    b = NB(gmat)
    oc = b.n('ShaderNodeTexCoord').outputs['Object']
    n1 = b.noise(oc, 0.35, 6.0, 0.6).outputs['Fac']
    n2 = b.noise(oc, 6.0, 8.0, 0.65).outputs['Fac']
    gcol = b.mix(b.smooth(n1, 0.35, 0.7), lin(70, 64, 52), lin(104, 96, 80))
    gcol = b.mix(b.math('MULTIPLY', b.smooth(n2, 0.45, 0.75), 0.6), gcol, lin(52, 60, 36))
    gb = b.n('ShaderNodeBump')
    gb.inputs['Strength'].default_value = 0.6
    gb.inputs['Distance'].default_value = 0.05
    b.l(b.math('ADD', n2, b.math('MULTIPLY', n1, 2.0)), gb.inputs['Height'])
    gbs = b.n('ShaderNodeBsdfPrincipled')
    b.l(gcol, gbs.inputs['Base Color'])
    gbs.inputs['Roughness'].default_value = 0.92
    b.l(gb.outputs['Normal'], gbs.inputs['Normal'])
    b.l(gbs.outputs[0], b.n('ShaderNodeOutputMaterial').inputs[0])
    gm.materials.append(gmat)
    cam = bpy.data.cameras.new('Cam')
    co_ = link(bpy.data.objects.new('Cam', cam))
    scene.camera = co_
    scene.render.resolution_x = 1600
    scene.render.resolution_y = 1000
    scene.cycles.samples = SAMPLES
    scene.cycles.use_denoising = True
    scene.view_settings.view_transform = 'AgX'
    scene.view_settings.look = 'AgX - Medium High Contrast'
    return co_, go


def shoot(cam, loc, target, name, lens=50, ortho=None, fstop=0, hdri='sunset', key_offset=55, rim=1.0,
          ground=True, exposure=0.0):
    cam.location = Vector(loc)
    d = Vector(target) - Vector(loc)
    cam.rotation_euler = d.to_track_quat('-Z', 'Y').to_euler()
    if ortho:
        cam.data.type = 'ORTHO'
        cam.data.ortho_scale = ortho
    else:
        cam.data.type = 'PERSP'
        cam.data.lens = lens
    cam.data.dof.use_dof = fstop > 0
    if fstop > 0:
        cam.data.dof.focus_distance = d.length
        cam.data.dof.aperture_fstop = fstop
    cam.data.clip_end = 500
    nt = scene.world.node_tree
    img = bpy.data.images.load(os.path.join(HDRI_DIR, HDRIS[hdri]), check_existing=True)
    nt.nodes['HDRI'].image = img
    az_tex = hdri_sun_azimuth(img)
    a_cam = math.atan2(loc[1] - target[1], loc[0] - target[0])
    A = a_cam + math.radians(key_offset)
    nt.nodes['HDRI_MAP'].inputs['Rotation'].default_value = (0, 0, az_tex - A)
    # rim: behind the subject, opposite the camera, raised
    rimo = bpy.data.objects['Rim']
    back_dir = Vector((-d.x, -d.y, 0)).normalized() * -1
    rimo.location = Vector(target) + back_dir * 7 + Vector((0, 0, 5))
    rimo.rotation_euler = (Vector(target) - rimo.location).to_track_quat('-Z', 'Y').to_euler()
    rimo.data.energy = 2500 * rim
    bpy.data.objects['Ground'].hide_render = not ground
    scene.view_settings.exposure = exposure
    scene.render.filepath = os.path.join(PREV, name)
    bpy.ops.render.render(write_still=True)
    log('rendered', name)


def set_pose(rots):
    pb = arm.pose.bones
    for b in pb:
        b.rotation_mode = 'XYZ'
        b.rotation_euler = (0, 0, 0)
    for nm, (rx, ry, rz) in rots.items():
        for sfx, sgn in (('_l', 1), ('_r', -1)) if nm.endswith('_X') else (('', 1),):
            bn = nm[:-2] + sfx if nm.endswith('_X') else nm
            pb[bn].rotation_euler = (math.radians(rx), math.radians(ry * sgn), math.radians(rz * sgn))
    bpy.context.view_layer.update()


FOLD = {
    'wing_shoulder_X': (25, 0, 50),
    'wing_elbow_X': (0, 0, -115),
    'wing_wrist_X': (0, 0, 95),
    'wing_finger1_X': (0, 0, 10),
    'wing_finger2_X': (0, 0, 2),
    'wing_finger3_X': (0, 0, -6),
    'wing_finger4_X': (0, 0, -14),
}
POSE_TEST = dict(FOLD)
POSE_TEST.update({
    'neck_02': (0, 0, 12), 'neck_03': (8, 0, 12), 'neck_04': (8, 0, 10), 'head': (-10, 0, 8),
    'jaw': (-22, 0, 0),
    'tail_03': (0, 0, 10), 'tail_05': (0, 0, 14), 'tail_07': (0, 0, 16), 'tail_09': (0, 0, 16),
    'upperarm_l': (-25, 0, 0), 'forearm_l': (40, 0, 0), 'hand_l': (-20, 0, 0),
    'thigh_r': (20, 0, 0), 'calf_r': (-20, 0, 0),
})

SHOTS = opt('--shots', 'all')
SHOTS = None if SHOTS == 'all' else set(SHOTS.split(','))


def want(n):
    return SHOTS is None or n in SHOTS


FLIGHT = {
    'root': (6, 0, 0),
    'wing_shoulder_X': (14, 0, -6), 'wing_elbow_X': (0, 0, 4), 'wing_wrist_X': (0, 0, -3),
    'wing_finger1_X': (2, 0, 0), 'wing_finger2_X': (0, 0, 2), 'wing_finger3_X': (0, 0, 3), 'wing_finger4_X': (0, 0, 4),
    'neck_01': (-18, 0, 0), 'neck_02': (-10, 0, 0), 'neck_03': (-4, 0, 0), 'head': (14, 0, 0),
    'upperarm_X': (-60, 0, 0), 'forearm_X': (95, 0, 0), 'hand_X': (-35, 0, 0),
    'thigh_X': (-55, 0, 0), 'calf_X': (-35, 0, 0), 'foot_X': (-55, 0, 0),
    'tail_01': (10, 0, 0), 'tail_02': (4, 0, 0), 'tail_05': (0, 0, 4), 'tail_08': (0, 0, -5),
}

if not NO_RENDER:
    cam, ground = setup_preview_scene()
    set_pose({})
    if want('front34'):
        shoot(cam, (8.8, -11.5, 4.2), (0.6, -0.3, 1.6), 'dragon_front34.png', lens=45, fstop=11)
    if want('front34_overcast'):
        shoot(cam, (8.8, -11.5, 4.2), (0.6, -0.3, 1.6), 'dragon_front34_overcast.png', lens=45, hdri='overcast', rim=0.4)
    if want('side'):
        shoot(cam, (30, 0.9, 1.9), (0, 0.9, 1.9), 'dragon_side.png', ortho=10.5, hdri='overcast', rim=0.4)
    if want('top'):
        shoot(cam, (0, 0.9, 40), (0, 0.9, 0), 'dragon_top.png', ortho=16.5, hdri='overcast', rim=0.0)
    if want('head'):
        shoot(cam, (-2.6, -5.8, 3.0), (0, -2.75, 2.45), 'dragon_head.png', lens=50, fstop=5.6)
    if want('closeup_head'):
        shoot(cam, (1.05, -3.55, 2.85), (0.05, -2.8, 2.55), 'dragon_closeup_head.png', lens=60, fstop=4.0)
    if want('closeup_head_overcast'):
        shoot(cam, (1.05, -3.55, 2.85), (0.05, -2.8, 2.55), 'dragon_closeup_head_overcast.png', lens=60, fstop=4.0,
              hdri='overcast', rim=0.3)
    if want('closeup_flank'):
        shoot(cam, (3.4, -2.6, 1.9), (0.3, -0.2, 1.3), 'dragon_closeup_flank.png', lens=50, fstop=5.6)
    if want('wing_under'):
        shoot(cam, (4.2, -3.2, 0.7), (3.6, 0.9, 2.15), 'dragon_wing_under.png', lens=32, key_offset=180)
    if want('folded'):
        set_pose(FOLD)
        shoot(cam, (-9.5, -10.5, 4.5), (0, 0.3, 1.5), 'dragon_folded.png', lens=45, fstop=11, key_offset=-55)
    if want('posetest'):
        set_pose(POSE_TEST)
        shoot(cam, (8, -11, 7), (0, 0.0, 1.6), 'dragon_posetest.png', lens=45, hdri='overcast', rim=0.4)
    if want('hero'):
        set_pose(FLIGHT)
        shoot(cam, (-8.5, -17.0, -1.2), (0, -0.4, 2.1), 'dragon_hero_flight.png', lens=42, key_offset=70,
              ground=False, rim=1.4)
    set_pose({})
    for o in ('Cam', 'Rim', 'Ground'):
        bpy.data.objects.remove(bpy.data.objects[o])

if QUICK:
    log('quick mode: done')
    sys.exit(0)

# ---------------------------------------------------------------------------
# Save .blend (Blender convention: faces -Y)
# ---------------------------------------------------------------------------
for b in arm.pose.bones:
    b.rotation_euler = (0, 0, 0)
    b.location = (0, 0, 0)
    b.scale = (1, 1, 1)
for img in list(bpy.data.images):
    if img.name.startswith('bk_') or img.users == 0:
        bpy.data.images.remove(img)
blend_path = os.path.join(OUT, 'SK_Dragon.blend')
bpy.context.preferences.filepaths.save_version = 0
bpy.ops.wm.save_as_mainfile(filepath=blend_path, compress=True)
log('saved', blend_path)

# ---------------------------------------------------------------------------
# FBX export: rotate +90 deg about Z so the nose points along +X (UE forward)
# ---------------------------------------------------------------------------
Rz = Matrix.Rotation(math.radians(90), 4, 'Z')
dragon.parent = None
dragon.data.transform(Rz)
arm.data.transform(Rz)
dragon.parent = arm
dragon.matrix_parent_inverse.identity()
dragon.matrix_world = Matrix.Identity(4)
for o in scene.objects:
    o.select_set(o in (arm, dragon))
bpy.context.view_layer.objects.active = arm
fbx_path = os.path.join(OUT, 'SK_Dragon.fbx')
bpy.ops.export_scene.fbx(
    filepath=fbx_path, use_selection=True, object_types={'ARMATURE', 'MESH'},
    apply_unit_scale=True, apply_scale_options='FBX_SCALE_ALL', global_scale=1.0,
    axis_forward='-Z', axis_up='Y', bake_space_transform=False,
    use_mesh_modifiers=True, mesh_smooth_type='FACE', use_tspace=True, colors_type='NONE',
    add_leaf_bones=False, primary_bone_axis='Y', secondary_bone_axis='X',
    armature_nodetype='NULL', use_armature_deform_only=False,
    bake_anim=False, path_mode='RELATIVE', embed_textures=False)
log('exported', fbx_path)

# stats for the README
with open(os.path.join(OUT, 'build_stats.txt'), 'w') as f:
    f.write('blender %s\n' % bpy.app.version_string)
    f.write('triangles %d\nvertices %d\n' % (NTRI, NV))
    f.write('length_m %.3f\nspan_m %.3f\n' % (LEN_FINAL, span))
    f.write('bones %d\n%s\n' % (len(BONE_NAMES), ' '.join(BONE_NAMES)))
log('done')
