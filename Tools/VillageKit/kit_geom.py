"""
Low-level geometry for the village kit: a flat mesh accumulator and the
primitives every building is assembled from (swept timbers, drystone rocks,
displaced turf sheets, grass blades, flat cut-outs).

Everything is built in metres, Z up. UVs are real-world scaled so a texture
tile always covers the same physical size (see MAT_TILE).
"""
import math
import random

import bmesh
from mathutils import Vector, Matrix, noise

# Material slots, in order. Names are what ends up in the glTF / UE.
MATS = ["M_Village_WoodTar", "M_Village_WoodGrey", "M_Village_Stone", "M_Village_Turf", "M_Village_Grass"]
WOOD, GREY, STONE, TURF, GRASS = range(5)
# (u, v) metres per texture tile. Wood grain runs along V; boards are swept with V
# along their length, and the grain is stretched 2x along the board (invisible).
MAT_TILE = {WOOD: (0.5, 1.0), GREY: (0.5, 1.0), STONE: (3.0, 3.0), TURF: (2.0, 2.0), GRASS: (1.0, 1.0)}

V = Vector


class MB:
    """Mesh accumulator: vertices, n-gon faces, per-corner UVs, per-face material."""

    def __init__(self):
        self.V = []
        self.F = []
        self.UV = []
        self.M = []

    def v(self, p):
        self.V.append((p[0], p[1], p[2]))
        return len(self.V) - 1

    def face(self, idx, uvs, mat):
        self.F.append(tuple(idx))
        self.UV.append(tuple(uvs))
        self.M.append(mat)

    def tris(self):
        return sum(len(f) - 2 for f in self.F)

    def extend(self, other):
        o = len(self.V)
        self.V += other.V
        self.F += [tuple(i + o for i in f) for f in other.F]
        self.UV += other.UV
        self.M += other.M

    def transform(self, mat, start=0):
        for i in range(start, len(self.V)):
            p = mat @ V(self.V[i])
            self.V[i] = (p.x, p.y, p.z)

    def deform(self, fn, start=0, stop=None):
        stop = len(self.V) if stop is None else stop
        for i in range(start, stop):
            self.V[i] = tuple(fn(V(self.V[i])))


def uvs_scale(mat):
    su, sv = MAT_TILE[mat]
    return 1.0 / su, 1.0 / sv


# ---------------------------------------------------------------------------
# Profiles (2D loops, CCW in the (side, up) plane)

def prof_rect(w, h, c=0.0):
    """Rectangle w x h with chamfered corners c (8 points) or sharp (4)."""
    a, b = w / 2, h / 2
    if c <= 0:
        return [(a, -b), (a, b), (-a, b), (-a, -b)]
    c = min(c, a * 0.45, b * 0.45)
    return [(a, -b + c), (a, b - c), (a - c, b), (-a + c, b), (-a, b - c), (-a, -b + c), (-a + c, -b), (a - c, -b)]


def prof_circle(r, n=10, rng=None, rough=0.0, ry=None):
    ry = r if ry is None else ry
    out = []
    ph = rng.random() * 6.28 if rng else 0.0
    for k in range(n):
        t = 2 * math.pi * k / n
        f = 1.0
        if rng and rough:
            f += rough * (math.sin(3 * t + ph) * 0.5 + math.sin(5 * t + 2 * ph) * 0.3 + (rng.random() - 0.5) * 0.6)
        out.append((math.cos(t) * r * f, math.sin(t) * ry * f))
    return out


def prof_wedge(r, ang, n=5, rng=None):
    """Split firewood: a pie slice of a log (bark arc plus two split faces)."""
    pts = [(0.0, 0.0)]
    a0 = -ang / 2
    for k in range(n + 1):
        t = a0 + ang * k / n
        rr = r * (1 + (rng.random() - 0.5) * 0.08 if rng else 1)
        pts.append((math.cos(t) * rr - r * 0.45, math.sin(t) * rr))
    return pts


# ---------------------------------------------------------------------------
# Sweeps (boards, logs, poles, staves)

def _frame(t, up):
    t = t.normalized()
    u = (up - t * up.dot(t))
    if u.length < 1e-6:
        u = V((0, 0, 1)) if abs(t.z) < 0.9 else V((1, 0, 0))
        u = (u - t * u.dot(t))
    u.normalize()
    s = u.cross(t)
    return t, u, s


def sweep(mb, pts, profile, mat, up=V((0, 0, 1)), ups=None, prof_fn=None, cap0=True, cap1=True,
          uv_off=None, rng=None, twist=None):
    """Sweep a closed 2D profile along a polyline. profile: list of (a, b) where a runs
    along the side axis and b along `up`. prof_fn(i, t) may return a per-ring profile with the
    same number of points. Returns the vertex index range."""
    n = len(pts)
    start = len(mb.V)
    if uv_off is None:
        uv_off = (rng.random() * 7.0, rng.random() * 7.0) if rng else (0.0, 0.0)
    su, sv = uvs_scale(mat)
    # cumulative length
    L = [0.0]
    for i in range(1, n):
        L.append(L[-1] + (pts[i] - pts[i - 1]).length)
    total = L[-1] if L[-1] > 0 else 1.0
    rings = []
    profs = []
    for i in range(n):
        if i == 0:
            t = pts[1] - pts[0]
        elif i == n - 1:
            t = pts[-1] - pts[-2]
        else:
            t = (pts[i + 1] - pts[i - 1])
        u0 = ups[i] if ups is not None else up
        t, u, s = _frame(t, u0)
        if twist is not None:
            a = twist(L[i] / total)
            ca, sa = math.cos(a), math.sin(a)
            s, u = s * ca + u * sa, -s * sa + u * ca
        prof = prof_fn(i, L[i] / total) if prof_fn else profile
        profs.append(prof)
        ring = [mb.v(pts[i] + s * a + u * b) for (a, b) in prof]
        rings.append(ring)
    m = len(profile)
    # perimeter coordinate per profile point (of the first ring)
    per = [0.0]
    for k in range(1, m + 1):
        a0, b0 = profile[k - 1]
        a1, b1 = profile[k % m]
        per.append(per[-1] + math.hypot(a1 - a0, b1 - b0))
    uo, vo = uv_off
    for i in range(n - 1):
        r0, r1 = rings[i], rings[i + 1]
        v0, v1 = L[i] * sv + vo, L[i + 1] * sv + vo
        for k in range(m):
            k1 = (k + 1) % m
            ua, ub = per[k] * su + uo, per[k + 1] * su + uo
            mb.face((r0[k], r0[k1], r1[k1], r1[k]), ((ua, v0), (ub, v0), (ub, v1), (ua, v1)), mat)
    if cap0:
        mb.face(tuple(reversed(rings[0])), tuple((a * su * 2 + uo, b * su * 2 + vo) for (a, b) in reversed(profs[0])), mat)
    if cap1:
        mb.face(tuple(rings[-1]), tuple((a * su * 2 + uo + 0.3, b * su * 2 + vo) for (a, b) in profs[-1]), mat)
    return start, len(mb.V)


def line_pts(p0, p1, seg_len=0.5, bend=None, rng=None, wobble=0.0):
    """Points from p0 to p1; bend(t) -> Vector offset (sag, bow); wobble adds hand-made irregularity."""
    p0, p1 = V(p0), V(p1)
    L = (p1 - p0).length
    n = max(1, int(math.ceil(L / seg_len)))
    out = []
    ph = rng.random() * 100 if rng else 0
    d = (p1 - p0).normalized()
    side = d.cross(V((0, 0, 1)))
    if side.length < 1e-4:
        side = d.cross(V((1, 0, 0)))
    side.normalize()
    upv = side.cross(d)
    for i in range(n + 1):
        t = i / n
        p = p0.lerp(p1, t)
        if bend:
            p = p + bend(t)
        if wobble and 0 < i < n:
            p = p + side * wobble * noise.noise(V((t * 2.3 + ph, ph, 0))) + upv * wobble * noise.noise(V((ph, t * 2.1 + ph, 5)))
        out.append(p)
    return out


def board(mb, p0, p1, w, t, mat, up=V((0, 0, 1)), rng=None, chamfer=0.008, seg_len=0.6, bend=None,
          wobble=0.0, taper=0.0, warp=0.0):
    """A sawn/hewn board from p0 to p1: width w along the side axis, thickness t along `up`.
    taper narrows the far end; warp cups it slightly. Irregular by default."""
    rng = rng or random.Random(0)
    pts = line_pts(p0, p1, seg_len, bend, rng, wobble)
    base = prof_rect(w, t, chamfer)
    wj = [(rng.random() - 0.5) * 0.04 * w for _ in range(len(pts))]

    def pf(i, tt):
        ww = 1.0 - taper * tt + wj[i] / w
        cu = warp * math.sin(math.pi * tt)
        return [(a * ww, b + cu * (1 - (2 * a / w) ** 2) * t) for (a, b) in base]
    return sweep(mb, pts, base, mat, up=up, prof_fn=pf, rng=rng)


def log(mb, p0, p1, r, mat, rng, up=V((0, 0, 1)), n=10, rough=0.06, seg_len=0.4, taper=0.12, bend=None,
        wobble=0.02, flat=0.0, r1=None):
    """A round or hewn log: radius r (r1 at the far end), slightly irregular along its length.
    flat squashes it vertically (stacked laft logs are hewn top/bottom)."""
    pts = line_pts(p0, p1, seg_len, bend, rng, wobble)
    r1 = r * (1 - taper) if r1 is None else r1
    ph = rng.random() * 50
    base = prof_circle(r, n, rng, rough)

    def pf(i, tt):
        rr = (r + (r1 - r) * tt) / r
        bump = 1 + 0.05 * noise.noise(V((tt * 6 + ph, ph, 1.7)))
        return [(a * rr * bump, b * rr * bump * (1 - flat)) for (a, b) in base]
    return sweep(mb, pts, base, mat, up=up, prof_fn=pf, rng=rng)


def prism(mb, outline, M, thick, mat, rng=None, uv_rot=0.0, bevel_top=0.0):
    """Extrude a 2D outline (CCW, in local XY of matrix M) by `thick` along local +Z.
    Used for carved ridge ends, shingles, door leaves. UVs: grain along local Y."""
    su, sv = uvs_scale(mat)
    uo, vo = ((rng.random() * 7, rng.random() * 7) if rng else (0, 0))
    n = len(outline)
    bot = [mb.v(M @ V((x, y, 0))) for (x, y) in outline]
    top = [mb.v(M @ V((x * (1 - bevel_top), y * (1 - bevel_top), thick))) for (x, y) in outline]
    uvb = [(x * su + uo, y * sv + vo) for (x, y) in outline]
    mb.face(tuple(reversed(bot)), tuple(reversed(uvb)), mat)
    mb.face(tuple(top), tuple(uvb), mat)
    per = 0.0
    for k in range(n):
        k1 = (k + 1) % n
        d = math.hypot(outline[k1][0] - outline[k][0], outline[k1][1] - outline[k][1])
        mb.face((bot[k], bot[k1], top[k1], top[k]),
                ((per * su + uo, vo), ((per + d) * su + uo, vo), ((per + d) * su + uo, thick * sv + vo), (per * su + uo, thick * sv + vo)), mat)
        per += d


def frame_from(origin, xdir, ydir):
    """4x4 matrix with local X along xdir, local Y along ydir (orthogonalised), Z = X x Y."""
    x = V(xdir).normalized()
    y = V(ydir)
    y = (y - x * y.dot(x)).normalized()
    z = x.cross(y)
    M = Matrix((
        (x.x, y.x, z.x, origin[0]),
        (x.y, y.y, z.y, origin[1]),
        (x.z, y.z, z.z, origin[2]),
        (0, 0, 0, 1)))
    return M


# ---------------------------------------------------------------------------
# Rocks

_ICO = {}


def _ico(sub):
    if sub not in _ICO:
        bm = bmesh.new()
        bmesh.ops.create_icosphere(bm, subdivisions=sub, radius=1.0)
        verts = [v.co.copy() for v in bm.verts]
        faces = [tuple(v.index for v in f.verts) for f in bm.faces]
        bm.free()
        _ICO[sub] = (verts, faces)
    return _ICO[sub]


def rock(mb, center, size, rng, mat=STONE, sub=2, rough=0.22, flat_bottom=True, yaw=None, tilt=0.08,
         squash=0.0):
    """A field stone: deformed icosphere, flattened faces like split basalt / beach cobbles.
    size = (sx, sy, sz) full dimensions."""
    verts, faces = _ico(sub)
    seed = V((rng.random() * 100, rng.random() * 100, rng.random() * 100))
    yaw = rng.random() * 6.283 if yaw is None else yaw
    R = Matrix.Rotation(yaw, 4, 'Z') @ Matrix.Rotation((rng.random() - 0.5) * tilt * 2, 4, 'X') @ Matrix.Rotation((rng.random() - 0.5) * tilt * 2, 4, 'Y')
    sx, sy, sz = size[0] / 2, size[1] / 2, size[2] / 2
    # a few random cut planes give the faceted look of split stone
    cuts = []
    for _ in range(rng.randint(2, 5)):
        nrm = V((rng.random() - 0.5, rng.random() - 0.5, (rng.random() - 0.5) * 0.6)).normalized()
        cuts.append((nrm, 0.62 + rng.random() * 0.3))
    base = len(mb.V)
    c = V(center)
    for p in verts:
        q = p.copy()
        f = 1 + rough * noise.noise(q * 1.3 + seed) + rough * 0.4 * noise.noise(q * 3.1 + seed * 1.7)
        q = q * f
        for nrm, d in cuts:
            h = q.dot(nrm)
            if h > d:
                q = q - nrm * (h - d) * 0.85
        if flat_bottom and q.z < -0.55:
            q.z = -0.55 - (q.z + 0.55) * 0.25
        if q.z > 0.7 - squash:
            q.z = (0.7 - squash) + (q.z - 0.7 + squash) * 0.3
        q = V((q.x * sx, q.y * sy, q.z * sz))
        q = R @ q
        mb.v(c + q)
    su, sv = uvs_scale(mat)
    uo, vo = rng.random() * 9, rng.random() * 9
    for f in faces:
        idx = tuple(base + i for i in f)
        a, b, cc = (V(mb.V[i]) for i in idx)
        n = (b - a).cross(cc - a)
        ax = max(range(3), key=lambda k: abs(n[k]))
        o = [k for k in range(3) if k != ax]
        uvs = tuple((mb.V[i][o[0]] * su + uo, mb.V[i][o[1]] * sv + vo) for i in idx)
        mb.face(idx, uvs, mat)


def stone_course(mb, pts_path, rng, h=0.35, depth=0.45, z0=0.0, courses=2, size=0.45, mat=STONE, sub=2,
                 closed=False, batter=0.04, inward=None, jitter=0.04):
    """Drystone footing / wall along a polyline: courses of irregular stones, each course
    offset half a stone. inward: 2D normal function for the path (default left of travel)."""
    segs = list(zip(pts_path, pts_path[1:] + ([pts_path[0]] if closed else [])))
    for c in range(courses):
        zc = z0 + c * h * 0.9 + h * 0.45
        shrink = 1 - c * batter
        off = (c % 2) * 0.5
        for (a, b) in segs:
            a, b = V(a), V(b)
            d = b - a
            L = d.length
            if L < 1e-3:
                continue
            dn = d / L
            nrm = V((-dn.y, dn.x, 0))
            x = -off * size * rng.random()
            while x < L:
                sl = size * (0.7 + rng.random() * 0.7)
                cx = x + sl / 2
                if cx > L + sl * 0.2:
                    break
                p = a + dn * min(cx, L) + nrm * ((rng.random() - 0.5) * jitter - c * batter * depth)
                p.z = zc + (rng.random() - 0.5) * h * 0.12
                yaw = math.atan2(dn.y, dn.x) + (rng.random() - 0.5) * 0.25
                rock(mb, p, (sl * 1.02, depth * shrink * (0.8 + rng.random() * 0.4), h * (0.85 + rng.random() * 0.4)),
                     rng, mat, sub, yaw=yaw)
                x += sl * 0.93


# ---------------------------------------------------------------------------
# Grids (turf sheets) and grass

def grid(mb, nu, nv, P, mat, close_u=False, uv=None, flip=False):
    """Quad grid of (nu+1) x (nv+1) points P(i, j) -> Vector. uv(i, j) -> (u, v) in metres
    (scaled by the material tile). Returns 2D list of vertex indices."""
    su, sv = uvs_scale(mat)
    cols = nu if close_u else nu + 1
    idx = [[mb.v(P(i, j)) for j in range(nv + 1)] for i in range(cols)]
    for i in range(nu):
        i1 = (i + 1) % cols
        for j in range(nv):
            q = (idx[i][j], idx[i1][j], idx[i1][j + 1], idx[i][j + 1])
            if uv:
                uq = (uv(i, j), uv(i + 1, j), uv(i + 1, j + 1), uv(i, j + 1))
            else:
                uq = tuple((V(mb.V[k]).x, V(mb.V[k]).y) for k in q)
            uq = tuple((a * su, b * sv) for (a, b) in uq)
            if flip:
                q = tuple(reversed(q))
                uq = tuple(reversed(uq))
            mb.face(q, uq, mat)
    return idx


def grass_tuft(mb, p, nrm, rng, height=0.25, blades=7, spread=0.05, lean=0.35, droop=None, mat=GRASS,
               gravity=V((0, 0, -1))):
    """A clump of grass blades rooted at p growing along nrm (blended with world up).
    Blade UV: u picks a colour column in the grass texture (green .. straw), v base->tip."""
    nrm = V(nrm).normalized()
    upw = (nrm * 0.55 + V((0, 0, 1)) * 0.45).normalized()
    col = rng.random()
    for _ in range(blades):
        ang = rng.random() * 6.283
        dirh = V((math.cos(ang), math.sin(ang), 0))
        root = p + dirh * spread * rng.random()
        h = height * (0.45 + rng.random() * 0.75)
        w = 0.004 + rng.random() * 0.006
        ln = lean * (0.4 + rng.random())
        if droop is not None:
            ln += droop
        d1 = (upw + dirh * ln * 0.5).normalized()
        d2 = (upw + dirh * ln * 1.6 + gravity * ln * 0.6).normalized()
        side = V((-dirh.y, dirh.x, 0))
        side = (side + V((rng.random() - 0.5, rng.random() - 0.5, 0)) * 0.6).normalized()
        m = root + d1 * h * 0.5
        tip = m + d2 * h * 0.5
        c = col + (rng.random() - 0.5) * 0.25
        c = min(max(c, 0.02), 0.98)
        a = mb.v(root - side * w)
        b = mb.v(root + side * w)
        cm = mb.v(m + side * w * 0.75)
        dm = mb.v(m - side * w * 0.75)
        t = mb.v(tip)
        mb.face((a, b, cm, dm), ((c, 0.02), (c + 0.004, 0.02), (c + 0.004, 0.5), (c, 0.5)), mat)
        mb.face((dm, cm, t), ((c, 0.5), (c + 0.004, 0.5), (c + 0.002, 0.98)), mat)


def scatter_on_faces(mb, f_start, f_stop, density, rng, filt=None):
    """Random points (p, normal) on faces [f_start, f_stop) of mb, `density` per m^2."""
    out = []
    for fi in range(f_start, f_stop):
        f = mb.F[fi]
        pts = [V(mb.V[i]) for i in f]
        if len(pts) < 3:
            continue
        n = (pts[1] - pts[0]).cross(pts[2] - pts[0])
        if len(pts) == 4:
            n2 = (pts[2] - pts[0]).cross(pts[3] - pts[0])
            area = (n.length + n2.length) / 2
            n = n + n2
        else:
            area = n.length / 2
        if area <= 0 or n.length == 0:
            continue
        n.normalize()
        k = density * area
        cnt = int(k) + (1 if rng.random() < k - int(k) else 0)
        for _ in range(cnt):
            u, v = rng.random(), rng.random()
            if len(pts) == 4:
                p = (pts[0].lerp(pts[1], u)).lerp(pts[3].lerp(pts[2], u), v)
            else:
                if u + v > 1:
                    u, v = 1 - u, 1 - v
                p = pts[0] + (pts[1] - pts[0]) * u + (pts[2] - pts[0]) * v
            if filt is None or filt(p, n):
                out.append((p, n))
    return out


def fbm(p, oct=3, seed=0.0):
    s, a, f = 0.0, 1.0, 1.0
    for _ in range(oct):
        s += a * noise.noise(p * f + V((seed, seed * 1.3, seed * 0.7)))
        a *= 0.5
        f *= 2.03
    return s
