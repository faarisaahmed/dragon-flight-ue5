"""
Building parts for the village kit: gable roofs (turf or wooden shingle), timber
walls (laft logs, vertical boards), doors, windows and yard props.

House frame used by all parts: ridge along local X, walls at x = +-L/2 (gables)
and y = +-W/2 (eaves), ground at z = 0. The house recipes rotate the finished
mesh so the front door ends up facing +X.
"""
import math

from mathutils import Vector, Matrix, noise

from kit_geom import (V, MB, WOOD, GREY, STONE, TURF, GRASS, sweep, board, log, prism, rock, grid,
                      grass_tuft, scatter_on_faces, prof_rect, prof_circle, prof_wedge, frame_from,
                      line_pts, fbm, stone_course)


def smooth(t):
    t = min(max(t, 0.0), 1.0)
    return t * t * (3 - 2 * t)


# ---------------------------------------------------------------------------
# Roof

class GableRoof:
    """Deck-top surface of a pitched roof over a plan whose half-width may vary along
    the ridge (boat-shaped longhouses). The ridge (and less so the eaves) sags between
    the gables like an old roof; `bow` drops the ridge towards both ends; the deck is
    slightly uneven. a = position along the ridge (X), y across."""

    def __init__(self, L, W, He, Hr, oe=0.5, og=0.45, sag=0.12, rng=None, x0=0.0, hw_fn=None, bow=0.0,
                 og0=None):
        self.L, self.W, self.He, self.Hr0 = L, W, He, Hr
        self.oe, self.og, self.sag, self.bow = oe, og, sag, bow
        self.x0 = x0
        self.hw_fn = hw_fn
        self.seed = rng.random() * 100 if rng else 0.0
        self.a0 = x0 - L / 2 - (og if og0 is None else og0)
        self.a1 = x0 + L / 2 + og

    def hw(self, a):
        return self.hw_fn(a) if self.hw_fn else self.W / 2

    def ye(self, a):
        return self.hw(a) + self.oe

    def Hr(self, a):
        t = (a - self.x0) / (self.L / 2)
        return self.Hr0 - self.bow * min(1.2, t * t)

    def slope(self, a):
        return (self.Hr(a) - self.He) / self.hw(a)

    def z(self, a, y):
        b = abs(y) / self.hw(a)
        z = self.Hr(a) - (self.Hr(a) - self.He) * b
        t = (a - self.x0) / (self.L / 2)
        s = max(0.0, 1 - t * t)
        z -= self.sag * s * (1 - 0.65 * min(b, 1.0))
        z += 0.025 * noise.noise(V((a * 0.4 + self.seed, y * 0.4, self.seed)))
        return z

    def p(self, a, y, lift=0.0):
        return V((a, y, self.z(a, y))) + self.n(a, y) * lift

    def n(self, a, y):
        e = 0.05
        dz_da = (self.z(a + e, y) - self.z(a - e, y)) / (2 * e)
        dz_dy = (self.z(a, y + e) - self.z(a, y - e)) / (2 * e)
        return V((-dz_da, -dz_dy, 1)).normalized()

    def half_width_at(self, z, a=None):
        """Horizontal half-width of the deck at height z (for gable walls)."""
        a = self.x0 if a is None else a
        return max(0.0, (self.Hr(a) - z) / self.slope(a))


def roof_frame(mb, rf, rng, deck_t=0.045, purlins=3, mat=WOOD, purlin_mat=WOOD, sides=(-1, 1)):
    """Roof boards running ridge-to-eave (visible as the soffit and at the gables) and
    round purlins under them whose ends stick out of the gables."""
    a = rf.a0 + 0.02
    while a < rf.a1 - 0.05:
        w = 0.18 + rng.random() * 0.12
        ac = min(a + w / 2, rf.a1 - w / 2 - 0.01)
        ye = rf.ye(ac)
        for s in sides:
            ys = [s * ye * k / 6 for k in range(7)]
            ys[0] = s * 0.01
            pts = [rf.p(ac, y, -deck_t / 2) for y in ys]
            ups = [rf.n(ac, y) for y in ys]
            pts[-1] = pts[-1] + V((0, s * (rng.random() - 0.5) * 0.05, 0))
            sweep(mb, pts, prof_rect(w - 0.006, deck_t, 0.006), mat, ups=ups, rng=rng)
        a += w
    fr = [0.0]
    for k in range(1, purlins):
        fr += [k / purlins * 0.95, -k / purlins * 0.95]
    fr += [1.0, -1.0]  # wall plates
    for f in fr:
        if f * min(sides) < 0 and f * max(sides) < 0 or (len(sides) == 1 and f * sides[0] < 0):
            continue
        r = 0.11 + rng.random() * 0.03 if abs(f) < 0.99 else 0.1
        over = 0.25 + rng.random() * 0.2
        a0 = rf.a0 + 0.1 - rng.random() * 0.15 - (over if rf.og < 0.2 else 0)
        a1 = rf.a1 - 0.1 + rng.random() * 0.15 + (over if rf.og < 0.2 else 0)
        n_seg = max(4, int((a1 - a0) / 0.5))
        pts = []
        for i in range(n_seg + 1):
            aa = a0 + (a1 - a0) * i / n_seg
            y = f * rf.hw(aa) - (0.12 * f if abs(f) > 0.99 else 0)
            pts.append(V((aa, y, rf.z(aa, y) - deck_t - r * 0.92)))
        sweep(mb, pts, prof_circle(r, 10, rng, 0.05), purlin_mat, rng=rng)


def turf_roof(mb, rf, rng, thick=0.24, res=0.14, grass_density=40.0, eave_log=True, grass_h=0.22,
              bare=0.45, fringe_sides=(-1, 1), end_taper=(True, True)):
    """Turf over the deck: a lumpy sheet continuous over the ridge, curling down over a
    turf-retaining log at each eave and hanging in a ragged fringe, with grass blades."""
    seed = rng.random() * 50
    if eave_log:
        for s in (-1, 1):
            aa_list = [rf.a0 + 0.15 + (rf.a1 - rf.a0 - 0.3) * i / 30 for i in range(31)]
            pts = []
            for aa in aa_list:
                y = s * (rf.ye(aa) - 0.1)
                pts.append(V((aa, y, rf.z(aa, y) + 0.07)))
            sweep(mb, pts, prof_circle(0.075, 9, rng, 0.06), WOOD, rng=rng)
            a = rf.a0 + 0.4
            while a < rf.a1 - 0.3:
                ye = rf.ye(a)
                z0 = rf.z(a, s * ye)
                hp = [V((a, s * (ye - 0.02), z0 - 0.09)), V((a, s * (ye + 0.03), z0 - 0.02)),
                      V((a, s * (ye + 0.0), z0 + 0.1)), V((a, s * (ye - 0.06), z0 + 0.16))]
                sweep(mb, hp, prof_rect(0.05, 0.035, 0.008), WOOD, up=V((1, 0, 0)), rng=rng)
                a += 0.55 + rng.random() * 0.25
    nu = max(8, int((rf.a1 - rf.a0) / res))
    nv_half = max(6, int(rf.ye(rf.x0) / res))
    fracs = [j / nv_half for j in range(-nv_half, nv_half + 1)]

    def lump(a, y):
        return 1 + 0.35 * fbm(V((a * 0.6, y * 0.6, seed)), 3) + 0.25 * noise.noise(V((a * 2.5, y * 2.5, seed + 3)))

    def top(a, f):
        y = f * rf.ye(a)
        d0 = (a - rf.a0) / 0.3 if end_taper[0] else 9
        d1 = (rf.a1 - a) / 0.3 if end_taper[1] else 9
        e = 0.12 + 0.88 * smooth(min(1.0, d0, d1))
        t = thick * lump(a, y) * e + 0.08 * math.exp(-(y / 0.5) ** 2)
        return V((a, y, rf.z(a, y))) + rf.n(a, y) * t

    def fringe(s, k):
        def f(a):
            ye = rf.ye(a)
            ze = rf.z(a, s * ye)
            h = 0.12 + 0.12 * (noise.noise(V((a * 1.7, seed, s * 3))) * 0.5 + 0.5) + 0.06 * noise.noise(V((a * 6, seed, s)))
            if k == 0:
                return V((a, s * (ye - 0.04), ze - 0.02))
            if k == 1:
                return V((a, s * (ye + 0.04), ze - h))
            if k == 2:
                return V((a, s * (ye + 0.09), ze + 0.02))
            return V((a, s * (ye + 0.06), ze + thick * 0.75))
        return f
    rows = []
    if -1 in fringe_sides:
        rows += [fringe(-1, k) for k in range(4)]
    rows += [(lambda ff: (lambda a: top(a, ff)))(f) for f in fracs]
    if 1 in fringe_sides:
        rows += [fringe(1, k) for k in reversed(range(4))]
    avals = [rf.a0 + (rf.a1 - rf.a0) * i / nu for i in range(nu + 1)]
    cache = {}

    def P(i, j):
        if (i, j) not in cache:
            cache[(i, j)] = rows[j](avals[i])
        return cache[(i, j)]

    def UV(i, j):
        p = P(i, j)
        return (p.x, p.y + p.z * 0.3)
    f0 = len(mb.F)
    grid(mb, nu, len(rows) - 1, P, TURF, uv=UV)
    f1 = len(mb.F)
    # gable-end edge of the turf: a strip from the turf surface down to the deck
    for ii, want in ((0, -1.0), (nu, 1.0)):
        aa = avals[ii]
        tops = [P(ii, j) for j in range(len(rows))]
        deck = []
        for q in tops:
            yy = max(-rf.ye(aa), min(rf.ye(aa), q.y))
            deck.append(V((aa, yy, min(q.z, rf.z(aa, yy)))))
        ti = [mb.v(q) for q in tops]
        di = [mb.v(q) for q in deck]
        for j in range(len(rows) - 1):
            quad = [ti[j], ti[j + 1], di[j + 1], di[j]]
            a_, b_, c_ = (V(mb.V[k]) for k in quad[:3])
            nn = (b_ - a_).cross(c_ - a_)
            if nn.x * want < 0:
                quad.reverse()
            mb.face(tuple(quad), tuple((mb.V[k][1] * 0.5, mb.V[k][2] * 0.5) for k in quad), TURF)
    if grass_density > 0:
        pts = scatter_on_faces(mb, f0, f1, grass_density, rng)
        for p, n in pts:
            patch = noise.noise(V((p.x * 0.45, p.y * 0.45, seed + 9)))
            if patch < -bare and rng.random() < 0.85:
                continue
            at_edge = abs(p.y) > rf.ye(p.x) - 0.05
            tall = 0.6 + 0.9 * (patch * 0.5 + 0.5) ** 1.5
            h = grass_h * (1.5 if at_edge else 1.0) * tall
            grass_tuft(mb, p - n * 0.015, n, rng, height=h, blades=rng.randint(5, 9), spread=0.06,
                       lean=0.55 + rng.random() * 0.5 if not at_edge else 0.8, droop=0.7 if at_edge else rng.random() * 0.4)
        for s in fringe_sides:
            a = rf.a0
            while a < rf.a1:
                p = fringe(s, 2)(a)
                grass_tuft(mb, p, V((0, s, -0.6)), rng, height=0.25 + rng.random() * 0.2, blades=6, lean=0.9,
                           droop=1.0)
                a += 0.05 + rng.random() * 0.08
    return f0, f1


def shingle_roof(mb, rf, rng, w=0.14, length=0.5, expo=0.17, mat=GREY, shape='round', ridge_mat=WOOD,
                 sides=(-1, 1), ridge=True):
    """Overlapping rows of riven wooden shingles from eave to ridge, each slightly
    different in width and angle, plus ridge boards."""
    for s in sides:
        nrows = int(math.hypot(rf.ye(rf.x0), rf.Hr(rf.x0) - rf.He) / expo) + 2
        for j in range(nrows):
            a = rf.a0 - 0.03 + (j % 2) * w * 0.5 - rng.random() * w * 0.3
            while a < rf.a1 + 0.02:
                ww = w * (0.75 + rng.random() * 0.5)
                ac = a + ww / 2
                if ac > rf.a1:
                    break
                ye = rf.ye(ac) + 0.04
                slope_len = math.hypot(ye, rf.Hr(ac) - rf.He + rf.oe * rf.slope(ac))
                d0 = j * expo
                yb = s * (ye - d0 * ye / slope_len)
                if s * yb < 0.04:
                    a += ww
                    continue
                ll = length * (0.9 + rng.random() * 0.2)
                y_top = s * max(0.0, abs(yb) - ll * ye / slope_len)
                n = rf.n(ac, yb)
                pb = rf.p(ac, yb) + n * (0.035 + rng.random() * 0.012)
                pt = rf.p(ac, y_top) + n * 0.008
                down = (pb - pt)
                Ls = down.length
                down.normalize()
                xd = V((-s, 0, (rng.random() - 0.5) * 0.06))
                Mf = frame_from(pb, xd, -down) @ Matrix.Rotation((rng.random() - 0.5) * 0.05, 4, 'Z')
                hw = ww / 2
                if shape == 'round':
                    arc_l = [(hw * math.cos(t), 0.55 * hw * (1 + math.sin(t))) for t in [-math.pi / 2 + math.pi / 2 * k / 4 for k in range(5)]]
                    arc_r = [(hw * math.cos(t), 0.55 * hw * (1 + math.sin(t))) for t in [math.pi + math.pi / 2 * k / 4 for k in range(4)]]
                    outline = arc_l + [(hw, Ls), (-hw, Ls)] + arc_r
                elif shape == 'point':
                    outline = [(0.0, 0.0), (hw, 0.6 * hw), (hw, Ls), (-hw, Ls), (-hw, 0.6 * hw)]
                else:
                    outline = [(-hw, 0.0), (hw, 0.0), (hw, Ls), (-hw, Ls)]
                prism(mb, outline, Mf, 0.014 + rng.random() * 0.006, (ridge_mat if rng.random() < 0.14 else mat), rng)
                a += ww + 0.004 + rng.random() * 0.01
    for s in ((-1, 1) if ridge else ()):
        pts, ups = [], []
        for i in range(31):
            aa = rf.a0 - 0.05 + (rf.a1 - rf.a0 + 0.1) * i / 30
            pts.append(rf.p(aa, s * 0.11, 0.07) + V((0, 0, 0.02)))
            ups.append(rf.n(aa, s * 0.5))
        sweep(mb, pts, prof_rect(0.26, 0.035, 0.006), ridge_mat, ups=ups, rng=rng)


def bargeboards(mb, rf, rng, w=0.32, t=0.05, carve='scroll', mat=WOOD, top_lift=0.3, ends=(True, True), ext=0.5):
    """Wind boards along both gable edges, crossing at the ridge; the crossed ends
    run on past the ridge and are cut into a finial (scroll / horn / plain)."""
    for gi, aa in enumerate((rf.a0 - 0.03, rf.a1 + 0.03)):
        if not ends[gi]:
            continue
        ye = rf.ye(aa)
        for s in (-1, 1):
            offs = V(((t + 0.004) * (1 if s > 0 else 0) * (1 if gi else -1), 0, 0))
            pe = V((aa, s * (ye + 0.03), rf.z(aa, s * ye) + top_lift - w / 2 - 0.02)) + offs
            pr = V((aa, 0.0, rf.z(aa, 0) + top_lift - w / 2 + 0.05)) + offs
            d = (pr - pe).normalized()
            e = ext * (0.9 + rng.random() * 0.2)
            pts = line_pts(pe, pr, 0.5, rng=rng, wobble=0.01)
            n_ext = 6
            perp = V((1, 0, 0)).cross(d)
            if perp.z < 0:
                perp = -perp
            for k in range(1, n_ext + 1):
                f = k / n_ext
                if carve == 'scroll':
                    curl = perp * (0.2 * f ** 2.0) - d * (0.05 * f ** 3)
                elif carve == 'horn':
                    curl = perp * (0.12 * f ** 1.6)
                else:
                    curl = V((0, 0, 0))
                pts.append(pr + d * e * f + curl)
            ntot = len(pts)
            nbase = ntot - n_ext
            base = prof_rect(w, t, 0.008)

            def pf(i, tt, nbase=nbase, ntot=ntot):
                if i < nbase:
                    return base
                f = (i - nbase + 1) / (ntot - nbase)
                k = max(0.18, 1 - f * 0.82) if carve != 'none' else max(0.6, 1 - f * 0.4)
                return [(a * k, b) for (a, b) in base]
            sweep(mb, pts, base, mat, up=V((1, 0, 0)), prof_fn=pf, rng=rng)


# ---------------------------------------------------------------------------
# Walls

def log_walls(mb, rf, L, W, z0, rng, d=0.23, crown=0.32, openings=(), mat=WOOD, gables=(True, True),
              x0=0.0):
    """Laft (corner-notched) log walls. Long walls at y=+-W/2 up to the eave plate, gable walls
    at x=+-L/2 continue up under the roof. Courses alternate half a log so the corners
    interlock and the log ends (crowns) stick out past each corner.
    openings: list of (wall, centre, width, z_bottom, z_top), wall in {'+y','-y','+x','-x'}."""
    r = d * 0.6

    def cut(wall, z, lo, hi):
        segs = [(lo, hi)]
        for (ow, c, w, zb, zt) in openings:
            if ow != wall or not (zb - r * 0.4 < z < zt + r * 0.4):
                continue
            new = []
            for (a, b) in segs:
                if c + w / 2 <= a or c - w / 2 >= b:
                    new.append((a, b))
                    continue
                if c - w / 2 > a + 0.05:
                    new.append((a, c - w / 2))
                if c + w / 2 < b - 0.05:
                    new.append((c + w / 2, b))
            segs = new
        return segs
    k = 0
    while True:
        zl = z0 + r * 0.8 + k * d          # long wall course
        zs = zl + d / 2                     # gable course
        done_long = zl > rf.He - r * 0.3
        hw = rf.half_width_at(zs + r * 0.8 + 0.05, x0 + L / 2) - 0.05
        if done_long and hw < 0.25:
            break
        if not done_long:
            for s in (-1, 1):
                wall = '+y' if s > 0 else '-y'
                lo = x0 - L / 2 - crown - (rng.random() - 0.3) * 0.18
                hi = x0 + L / 2 + crown + (rng.random() - 0.3) * 0.18
                for (a, b) in cut(wall, zl, lo, hi):
                    rr = r * (0.88 + rng.random() * 0.24)
                    log(mb, V((a, s * W / 2, zl + (rng.random() - 0.5) * 0.02)), V((b, s * W / 2, zl + (rng.random() - 0.5) * 0.02)),
                        rr, mat, rng, flat=0.12 + rng.random() * 0.1, taper=0.14 * (rng.random() - 0.5) * 2, rough=0.06,
                        n=12, wobble=0.025)
        for gi, s in enumerate((-1, 1)):
            if not gables[gi]:
                continue
            wall = '+x' if s > 0 else '-x'
            if zs < rf.He - r * 0.3:
                lo, hi = -W / 2 - crown - (rng.random() - 0.3) * 0.18, W / 2 + crown + (rng.random() - 0.3) * 0.18
            else:
                lo, hi = -hw, hw
            if hi - lo < 0.3:
                continue
            for (a, b) in cut(wall, zs, lo, hi):
                rr = r * (0.88 + rng.random() * 0.24)
                log(mb, V((x0 + s * L / 2, a, zs)), V((x0 + s * L / 2, b, zs)), rr, mat, rng, flat=0.12 + rng.random() * 0.1,
                    taper=0.14 * (rng.random() - 0.5) * 2, rough=0.06, n=12, wobble=0.025)
        k += 1


def board_wall(mb, p0, p1, z0, top, rng, out_n, mat=WOOD, t=0.035, openings=(), batten=True, post=0.17,
               sill=True, plate=True, ragged=0.03):
    """A vertical-board wall on a timber frame from plan point p0 to p1 (z from z0 up to
    top(s) where s is metres along the wall). Board-on-board: a back layer and narrower
    outer boards over the joints. out_n: outward normal (2D Vector).
    openings: list of (centre_s, width, z_bottom, z_top)."""
    p0, p1 = V((p0[0], p0[1], 0)), V((p1[0], p1[1], 0))
    d = p1 - p0
    Lw = d.length
    dn = d / Lw
    on = V((out_n[0], out_n[1], 0)).normalized()

    def blocked(s, w, zb, zt):
        for (c, ow, ozb, ozt) in openings:
            if abs(s - c) < (w + ow) / 2 and zt > ozb and zb < ozt:
                return (c, ow, ozb, ozt)
        return None
    if sill:
        log(mb, p0 + V((0, 0, z0 + 0.09)) - dn * 0.15, p1 + V((0, 0, z0 + 0.09)) + dn * 0.15, 0.11, mat, rng,
            flat=0.25, rough=0.04, n=8, taper=0.05)
    layers = [(0.0, 0.2, 0.34, 0.006), (t * 0.95, 0.09, 0.14, None)] if batten else [(0.0, 0.18, 0.3, 0.008)]
    for li, (off, wmin, wmax, gap) in enumerate(layers):
        s = post * 0.5 + (rng.random() * 0.15 if li else 0.0)
        if li:
            s = post * 0.5 + 0.17
        while s < Lw - post * 0.5:
            w = wmin + rng.random() * (wmax - wmin)
            sc = s + w / 2
            if sc + w / 2 > Lw - post * 0.4:
                w = (Lw - post * 0.4) - s
                if w < 0.06:
                    break
                sc = s + w / 2
            zb = z0 + 0.15 + (rng.random() - 0.5) * ragged
            zt = top(sc) - 0.02 - rng.random() * 0.03
            base = p0 + dn * sc + on * off
            o = blocked(sc, w, zb, zt)
            pieces = [(zb, zt)]
            if o:
                pieces = []
                if o[2] - zb > 0.08:
                    pieces.append((zb, o[2]))
                if zt - o[3] > 0.08:
                    pieces.append((o[3], zt))
            for (za, zz) in pieces:
                board(mb, base + V((0, 0, za)), base + V((0, 0, zz)), w - (gap or 0.0), t, mat,
                      up=on, rng=rng, wobble=0.004, warp=0.08 if li == 0 else 0.0, taper=0.0, chamfer=0.006)
            if li:
                s += w + 0.17 + rng.random() * 0.05
            else:
                s += w + (gap or 0.0) + rng.random() * 0.008
    if plate:
        zt = top(Lw / 2)
        log(mb, p0 - dn * 0.2 + on * 0.02 + V((0, 0, top(0) - 0.05)), p1 + dn * 0.2 + on * 0.02 + V((0, 0, top(Lw) - 0.05)),
            0.09, mat, rng, flat=0.2, n=8, rough=0.04)


def corner_post(mb, p, z0, z1, rng, w=0.2, mat=WOOD):
    board(mb, V((p[0], p[1], z0)), V((p[0], p[1], z1)), w, w, mat, up=V((1, 0, 0)), rng=rng, chamfer=0.02,
          wobble=0.01)


# ---------------------------------------------------------------------------
# Doors and windows

def door(mb, M, w, h, rng, mat=WOOD, frame_mat=WOOD, depth=0.25, ring=True, open_angle=0.0, lintel_ext=0.18,
         threshold=True):
    """Door in a wall. M: local frame at the bottom centre of the opening on the outer wall
    face; local X along the wall, local Y outward, Z up."""
    def P(x, y, z):
        return M @ V((x, y, z))
    X = (M.to_3x3() @ V((1, 0, 0))).normalized()
    Y = (M.to_3x3() @ V((0, 1, 0))).normalized()
    fw = 0.13
    for sx in (-1, 1):
        board(mb, P(sx * (w / 2 + fw / 2), 0.02, -0.05), P(sx * (w / 2 + fw / 2), 0.02, h + 0.02), fw, depth,
              frame_mat, up=Y, rng=rng, chamfer=0.015, wobble=0.005)
    board(mb, P(-w / 2 - fw - lintel_ext, 0.03, h + 0.1), P(w / 2 + fw + lintel_ext, 0.03, h + 0.1), 0.17,
          depth + 0.04, frame_mat, up=Y, rng=rng, chamfer=0.02, wobble=0.01, bend=lambda t: V((0, 0, -0.015 * math.sin(math.pi * t))))
    if threshold:
        log(mb, P(-w / 2 - fw, 0.04, 0.06), P(w / 2 + fw, 0.04, 0.06), 0.09, frame_mat, rng, flat=0.35,
            rough=0.03, n=8, taper=0.04)
    # leaf: vertical boards hinged on the -X side
    hinge = P(-w / 2, -0.05, 0)
    R = Matrix.Translation(hinge) @ Matrix.Rotation(open_angle, 4, M.to_3x3() @ V((0, 0, 1))) @ Matrix.Translation(-hinge)
    nb = max(3, int(round(w / 0.19)))
    bw = w / nb
    start = len(mb.V)
    for i in range(nb):
        x = -w / 2 + bw * (i + 0.5)
        hh = h - 0.02 + (rng.random() - 0.5) * 0.02
        board(mb, P(x, -0.05, 0.08), P(x, -0.05, hh), bw - 0.006, 0.045, mat, up=Y, rng=rng, chamfer=0.006,
              wobble=0.002, warp=0.05)
    # strap hinges / battens on the face (dark wood)
    for z in (0.35, h - 0.35):
        board(mb, P(-w / 2 + 0.02, -0.02, z), P(w / 2 - 0.04, -0.02, z), 0.07, 0.016, mat, up=Y, rng=rng,
              chamfer=0.004)
    if ring:
        c = P(w / 2 - 0.16, 0.0, h * 0.5)
        pts = [c + (X * math.cos(t) * 0.06) + V((0, 0, -0.06 + math.sin(t) * 0.06 - 0.0)) for t in
               [math.pi / 2 + 2 * math.pi * k / 14 for k in range(15)]]
        sweep(mb, pts, prof_circle(0.009, 6), mat, up=Y, rng=rng)
    if open_angle:
        mb.transform(R, start)
    # dark recess behind the leaf (the opening is closed off: back plate)
    back = [P(-w / 2 - 0.01, -depth + 0.02, 0.0), P(w / 2 + 0.01, -depth + 0.02, 0.0), P(w / 2 + 0.01, -depth + 0.02, h + 0.02), P(-w / 2 - 0.01, -depth + 0.02, h + 0.02)]
    idx = [mb.v(p) for p in back]
    mb.face(idx, ((0, 0), (1, 0), (1, 1), (0, 1)), mat)


def window(mb, M, w, h, rng, mat=WOOD, shutter=True, depth=0.25):
    """Small opening with a heavy frame and a board shutter (hung open)."""
    def P(x, y, z):
        return M @ V((x, y, z))
    Y = (M.to_3x3() @ V((0, 1, 0))).normalized()
    fw = 0.1
    for sx in (-1, 1):
        board(mb, P(sx * (w / 2 + fw / 2), 0.02, -fw), P(sx * (w / 2 + fw / 2), 0.02, h + fw), fw, depth, mat, up=Y,
              rng=rng, chamfer=0.012)
    for z in (-fw / 2, h + fw / 2):
        board(mb, P(-w / 2 - fw * 1.6, 0.025, z), P(w / 2 + fw * 1.6, 0.025, z), fw, depth + 0.02, mat, up=Y,
              rng=rng, chamfer=0.012)
    back = [P(-w / 2, -depth + 0.03, 0), P(w / 2, -depth + 0.03, 0), P(w / 2, -depth + 0.03, h), P(-w / 2, -depth + 0.03, h)]
    idx = [mb.v(p) for p in back]
    mb.face(idx, ((0, 0), (1, 0), (1, 1), (0, 1)), mat)
    if shutter:
        start = len(mb.V)
        nb = 3
        for i in range(nb):
            x = -w / 2 + w / nb * (i + 0.5)
            board(mb, P(x, 0.0, 0.0), P(x, 0.0, h), w / nb - 0.005, 0.035, mat, up=Y, rng=rng, chamfer=0.005)
        board(mb, P(-w / 2, 0.03, h * 0.25), P(w / 2, 0.03, h * 0.25), 0.07, 0.025, mat, up=Y, rng=rng)
        board(mb, P(-w / 2, 0.03, h * 0.75), P(w / 2, 0.03, h * 0.75), 0.07, 0.025, mat, up=Y, rng=rng)
        hinge = P(w / 2 + 0.02, 0.06, 0)
        R = Matrix.Translation(hinge) @ Matrix.Rotation(-1.9 - rng.random() * 0.4, 4, V((0, 0, 1))) @ Matrix.Translation(-hinge)
        T = Matrix.Translation(P(w + 0.04, 0.06, 0) - P(0, 0, 0)) if False else Matrix.Identity(4)
        mb.transform(R @ T, start)


# ---------------------------------------------------------------------------
# Props

def woodpile(mb, M, length, height, rng, depth_rows=2, piece_len=0.42, mat=WOOD):
    """Stacked split firewood. M: local frame, X along the pile, Y = piece direction
    (pile depth, back against a wall), Z up; origin at the front-left-bottom."""
    assert (M.to_3x3() @ V((0, 0, 1))).z > 0.5, "woodpile frame must have Z up (X x Y)"
    for row_d in range(depth_rows):
        y0 = row_d * (piece_len + 0.02)
        z = 0.0
        layer = 0
        while z < height:
            x = 0.06 + (layer % 2) * 0.06
            rowh = 0.0
            hgt_lim = height - 0.1 * (1 - smooth(1.0))
            while x < length - 0.06:
                r = 0.06 + rng.random() * 0.04
                ang = 1.2 + rng.random() * 1.4
                pl = piece_len * (0.85 + rng.random() * 0.3)
                yy = y0 + (rng.random() - 0.5) * 0.06
                c = M @ V((x, yy, z + r * 0.7))
                e = M @ V((x, yy + pl, z + r * 0.7))
                roll = rng.random() * 6.283
                Mr = M.to_3x3()
                upv = Mr @ V((math.cos(roll), 0, math.sin(roll)))
                sweep(mb, [c, e], prof_wedge(r, ang, 4, rng), mat, up=upv, rng=rng)
                x += r * 1.6 + 0.01
                rowh = max(rowh, r * 1.25)
            z += rowh
            layer += 1
            # pile slumps lower towards the ends
            length_eff = length
            if z > height * 0.7:
                height = height  # flat top, kept simple


def barrel(mb, p, rng, r=0.3, h=0.8, mat=WOOD, staves=16):
    p = V(p)
    for k in range(staves):
        a = 2 * math.pi * k / staves
        nrm = V((math.cos(a), math.sin(a), 0))
        pts, ups = [], []
        for i in range(7):
            t = i / 6
            rr = r * (0.86 + 0.14 * math.sin(math.pi * t))
            pts.append(p + nrm * rr + V((0, 0, h * t)))
            ups.append(nrm)
        sweep(mb, pts, prof_rect(2 * math.pi * r / staves * 0.97, 0.022, 0.003), mat, ups=ups, rng=rng)
    for t in (0.1, 0.33, 0.67, 0.9):
        rr = r * (0.86 + 0.14 * math.sin(math.pi * t)) + 0.013
        pts = [p + V((math.cos(a) * rr, math.sin(a) * rr, h * t)) for a in [2 * math.pi * k / 24 for k in range(25)]]
        ups = [V((math.cos(a), math.sin(a), 0)) for a in [2 * math.pi * k / 24 for k in range(25)]]
        sweep(mb, pts, prof_rect(0.045, 0.008, 0.002), mat, ups=ups, rng=rng, cap0=False, cap1=False)
    outline = [(math.cos(2 * math.pi * k / 16) * r * 0.85, math.sin(2 * math.pi * k / 16) * r * 0.85) for k in range(16)]
    prism(mb, outline, Matrix.Translation(p + V((0, 0, h - 0.04))), 0.025, mat, rng)


def ladder(mb, p0, p1, rng, w=0.45, mat=WOOD):
    p0, p1 = V(p0), V(p1)
    d = (p1 - p0)
    side = d.cross(V((0, 0, 1)))
    if side.length < 1e-3:
        side = V((1, 0, 0))
    side = side.normalized() * (w / 2)
    for s in (-1, 1):
        log(mb, p0 + side * s, p1 + side * s, 0.04, mat, rng, n=7, rough=0.06, taper=0.15)
    n = int(d.length / 0.32)
    for i in range(1, n):
        c = p0.lerp(p1, i / n)
        log(mb, c - side * 1.15, c + side * 1.15, 0.022, mat, rng, n=6, rough=0.05, taper=0.1)


def chopping_block(mb, p, rng, mat=WOOD):
    p = V(p)
    log(mb, p, p + V((0, 0, 0.5)), 0.22, mat, rng, n=14, rough=0.04, taper=-0.05, up=V((1, 0, 0)))
    # axe stuck in the top
    a = rng.random() * 6.28
    hd = V((math.cos(a), math.sin(a), 0))
    top = p + V((0, 0, 0.5))
    log(mb, top - hd * 0.02 + V((0, 0, 0.0)), top + hd * 0.55 + V((0, 0, 0.45)), 0.018, mat, rng, n=6, rough=0.02, taper=0)
    M = frame_from(top + V((0, 0, -0.05)) - hd * 0.04, hd, V((0, 0, 1)))
    prism(mb, [(-0.05, 0.0), (0.06, 0.0), (0.09, 0.12), (-0.05, 0.12)], M @ Matrix.Rotation(0.6, 4, 'Z') @ Matrix.Translation((0, 0, -0.01)),
          0.02, mat, rng)


def fish_rack(mb, M, length, rng, mat=WOOD, fish_mat=GREY, h=2.1, fish=True):
    """Hjell-style drying rack: pairs of leaning poles with crossbars, stockfish hung in pairs."""
    n = max(2, int(length / 1.8) + 1)
    P = lambda x, y, z: M @ V((x, y, z))
    for i in range(n):
        x = length * i / (n - 1)
        for s in (-1, 1):
            log(mb, P(x, s * 0.7, -0.1), P(x + (rng.random() - 0.5) * 0.1, -s * 0.05, h + 0.15), 0.05, mat, rng, n=7)
    for z in (h - 0.05, h * 0.62):
        for s in (-1, 1):
            yy = s * 0.7 * (1 - (z + 0.1) / (h + 0.25)) - s * 0.05 * ((z + 0.1) / (h + 0.25))
            log(mb, P(-0.2, yy, z), P(length + 0.2, yy, z), 0.035, mat, rng, n=7, wobble=0.02,
                bend=lambda t: V((0, 0, -0.04 * math.sin(math.pi * t))))
    if not fish:
        return
    for z in (h - 0.05, h * 0.62):
        x = 0.1
        while x < length - 0.1:
            for s in (-1, 1):
                yy = s * 0.7 * (1 - (z + 0.1) / (h + 0.25)) - s * 0.05 * ((z + 0.1) / (h + 0.25))
                top = P(x, yy + s * 0.04, z - 0.03)
                bot = P(x + (rng.random() - 0.5) * 0.05, yy + s * 0.07, z - 0.55 - rng.random() * 0.15)
                base = prof_circle(0.045, 8, None, 0, ry=0.016)
                pts = line_pts(top, bot, 0.08)

                def pf(i, t):
                    f = 0.25 + 0.75 * math.sin(math.pi * min(1.0, t * 1.15)) if t < 0.92 else 0.5 + (t - 0.92) * 6
                    return [(a * f, b * f) for (a, b) in base]
                sweep(mb, pts, base, fish_mat, up=(M.to_3x3() @ V((0, 1, 0))), prof_fn=pf, rng=rng)
            x += 0.16 + rng.random() * 0.06


def net_drape(mb, p0, p1, drop, rng, mesh=0.1, mat=WOOD, strand=0.011):
    """A fishing net hung over a rail from p0 to p1, sagging `drop` metres, knotted
    mesh of thin strands (real geometry)."""
    p0, p1 = V(p0), V(p1)
    d = p1 - p0
    L = d.length
    dn = d / L
    side = V((-dn.y, dn.x, 0)).normalized()
    nu = int(L / mesh)
    nv = int(drop / mesh)
    ph = rng.random() * 9

    def pos(i, j):
        u, v = i / nu, j / nv
        z = -drop * v * (0.75 + 0.25 * math.sin(math.pi * u))
        bunch = 0.15 * noise.noise(V((u * 4 + ph, v * 3, ph)))
        x = u * L + 0.04 * noise.noise(V((u * 9, v * 9, ph)))
        return p0 + dn * x + side * (0.03 + bunch * v + 0.08 * v * v) + V((0, 0, z * (1 - 0.15 * abs(noise.noise(V((u * 5, ph, 1)))))))
    prof = prof_rect(strand, strand, 0)
    for i in range(nu + 1):
        sweep(mb, [pos(i, j) for j in range(nv + 1)], prof, mat, up=side, rng=rng, cap0=False, cap1=False)
    for j in range(1, nv + 1):
        sweep(mb, [pos(i, j) for i in range(nu + 1)], prof, mat, up=V((0, 0, 1)), rng=rng, cap0=False, cap1=False)
    # floats along the top edge
    for i in range(0, nu + 1, 6):
        c = pos(i, 1)
        log(mb, c - V((0, 0, 0.04)), c + V((0, 0, 0.04)), 0.035, GREY, rng, n=8, rough=0.05, taper=0)


def lean_poles(mb, p, n, rng, mat=WOOD, h=2.4):
    """A few spare poles / oars leaning against a wall at p (wall normal ~ +x of frame)."""
    p = V(p)
    for i in range(n):
        q = p + V(((rng.random() - 0.5) * 0.2, i * 0.18, 0))
        log(mb, q + V((0.5 + rng.random() * 0.2, 0, 0)), q + V((0.0, (rng.random() - 0.5) * 0.2, h * (0.85 + rng.random() * 0.3))),
            0.03, mat, rng, n=7, taper=0.3)


def stone_scatter(mb, center, radius, count, rng, smin=0.15, smax=0.5):
    c = V(center)
    for _ in range(count):
        a = rng.random() * 6.283
        r = radius * math.sqrt(rng.random())
        s = smin + (smax - smin) * rng.random() ** 2
        rock(mb, c + V((math.cos(a) * r, math.sin(a) * r, s * 0.15)), (s, s * (0.6 + rng.random() * 0.4), s * 0.55), rng)


def ground_grass(mb, center, radius, rng, density=30, h=0.3, avoid=None):
    """Long grass tufts at the base of walls so the building sits in the ground."""
    c = V(center)
    n = int(density * math.pi * radius * radius)
    for _ in range(n):
        a = rng.random() * 6.283
        r = radius * math.sqrt(rng.random())
        p = c + V((math.cos(a) * r, math.sin(a) * r, -0.02))
        if avoid and avoid(p):
            continue
        grass_tuft(mb, p, V((0, 0, 1)), rng, height=h * (0.6 + rng.random() * 0.8), blades=rng.randint(5, 10), lean=0.35)


def wall_base_grass(mb, path, rng, out_n_fn, band=0.5, density=25, h=0.35):
    """Grass along the foot of a wall path (list of 2D points)."""
    for a, b in zip(path, path[1:]):
        a, b = V((a[0], a[1], 0)), V((b[0], b[1], 0))
        d = b - a
        L = d.length
        if L < 1e-3:
            continue
        on = out_n_fn(a, b)
        n = int(L * band * density)
        for _ in range(n):
            t = rng.random()
            o = band * rng.random() ** 1.5
            p = a + d * t + on * (o - 0.05) + V((0, 0, -0.02))
            grass_tuft(mb, p, V((0, 0, 1)), rng, height=h * (0.5 + rng.random()), blades=rng.randint(4, 9), lean=0.4)


# ---------------------------------------------------------------------------
# Turf walls (North Atlantic sod-and-stone)

def fillet_path(corners, radius, ds=0.06, closed=False):
    """Polyline through `corners` (2D/3D Vectors) with each interior corner rounded by
    `radius`, resampled at roughly ds spacing. Returns list of Vector (z=0)."""
    P = [V((c[0], c[1], 0)) for c in corners]
    n = len(P)
    pts = []
    idx = range(n) if closed else range(n)
    for i in idx:
        if not closed and (i == 0 or i == n - 1):
            pts.append(P[i])
            continue
        a, b, c = P[i - 1], P[i], P[(i + 1) % n]
        d0 = (b - a).normalized()
        d1 = (c - b).normalized()
        r = min(radius, (b - a).length * 0.45, (c - b).length * 0.45)
        p0 = b - d0 * r
        p2 = b + d1 * r
        for k in range(9):
            t = k / 8
            pts.append((p0 * (1 - t) ** 2) + (b * 2 * t * (1 - t)) + (p2 * t * t))
    if closed:
        pts.append(pts[0])
    out = [pts[0]]
    for a, b in zip(pts, pts[1:]):
        L = (b - a).length
        m = max(1, int(L / ds))
        for k in range(1, m + 1):
            out.append(a.lerp(b, k / m))
    return out


def turf_wall(mb, path, rng, H_fn, T_base=1.5, T_top=0.9, stone_h=0.55, dh=0.04, grass_top=55.0,
              grass_face=10.0, pattern=True, cap_ends=True, stones=True, seed=None, res_scale=1.0):
    """A battered sod wall swept along `path` (counter-clockwise for outward = right of
    travel). Courses of herringbone sod (klömbur) between flat strips (strengur) are cut
    into the outer face as real relief; drystone base courses; grassy rounded top.
    H_fn(p) gives the wall-top height at path point p."""
    seed = rng.random() * 100 if seed is None else seed
    n = len(path)
    # outward normals
    nrm = []
    for i in range(n):
        a = path[max(0, i - 1)]
        b = path[min(n - 1, i + 1)]
        d = (b - a).normalized()
        nrm.append(V((d.y, -d.x, 0)))
    # arc-length coordinate
    S = [0.0]
    for i in range(1, n):
        S.append(S[-1] + (path[i] - path[i - 1]).length)
    Hs = [H_fn(p) for p in path]
    Hmax = max(Hs)
    n_face = max(8, int(Hmax / dh))
    n_top = 7
    n_in = 5

    def relief(s, h):
        if not pattern or h < stone_h + 0.03:
            return 0.0
        hh = h - stone_h
        course_h = [0.3, 0.11]       # klömbur course, strengur strip
        cyc = sum(course_h)
        k = int(hh // cyc)
        q = hh - k * cyc
        if q < course_h[0]:
            # herringbone: slanted sods, direction alternates each course
            dirn = 1 if k % 2 == 0 else -1
            u = (s + dirn * q * 1.0) / 0.16
            f = u - math.floor(u)
            groove = math.exp(-((f - 0.0) / 0.12) ** 2) + math.exp(-((f - 1.0) / 0.12) ** 2)
            edge = math.exp(-(q / 0.025) ** 2) + math.exp(-((q - course_h[0]) / 0.025) ** 2)
            bulge = 0.02 * math.sin(math.pi * f)
            return -0.04 * min(1.0, groove) - 0.03 * min(1.0, edge) + bulge
        q2 = q - course_h[0]
        u = (s + k * 0.37) / 0.55
        f = u - math.floor(u)
        groove = math.exp(-(f / 0.04) ** 2) + math.exp(-((f - 1) / 0.04) ** 2)
        return 0.01 - 0.035 * min(1.0, groove) - 0.02 * math.exp(-((q2 - 0.055) / 0.08) ** 2) * 0
    rows_cache = {}

    def P(i, j):
        key = (i, j)
        if key in rows_cache:
            return rows_cache[key]
        p, nn, s, H = path[i], nrm[i], S[i], Hs[i]
        lumpy = 0.06 * fbm(V((s * 0.5, 0.3, seed)), 3)
        if j <= n_face:
            h = H * j / n_face
            f = h / H
            off = T_base / 2 + (T_top / 2 - T_base / 2) * f + 0.05 * math.sin(math.pi * f) + lumpy
            off += 0.03 * noise.noise(V((s * 1.3, h * 1.3, seed)))
            if h < stone_h:
                off -= 0.12   # turf core behind the stone base courses
            off += relief(s, h)
            q = p + nn * off + V((0, 0, h))
        elif j <= n_face + n_top:
            t = (j - n_face) / n_top            # 0..1 over the rounded top
            ang = math.pi * t
            r_out = T_top / 2 + lumpy
            off = math.cos(ang) * r_out
            z = H + math.sin(ang) * (0.18 + 0.06 * noise.noise(V((s, seed, 2)))) * (1 + 0.6 * math.sin(ang))
            q = p + nn * off + V((0, 0, z))
        else:
            t = (j - n_face - n_top) / n_in
            h = H * (1 - t)
            q = p - nn * (T_top / 2 + (T_base - T_top) * 0.1 * (1 - h / H)) + V((0, 0, h))
        rows_cache[key] = q
        return q
    nv = n_face + n_top + n_in

    def UV(i, j):
        q = P(i, j)
        return (S[i], q.z)
    f0 = len(mb.F)
    grid(mb, n - 1, nv, P, TURF, uv=UV)
    f1 = len(mb.F)
    if cap_ends:
        for i, flip in ((0, False), (n - 1, True)):
            pts = [P(i, j) for j in range(nv + 1)]
            idx = [mb.v(q) for q in pts]
            if flip:
                idx.reverse()
            mb.face(tuple(idx), tuple((mb.V[q][0] * 0.5 + mb.V[q][1] * 0.5, mb.V[q][2] * 0.5) for q in idx), TURF)
    if stones:
        outer = [path[i] + nrm[i] * (T_base / 2 - 0.02) for i in range(n)]
        # sparse polyline for the stone course
        step = max(1, int(0.25 / ((S[-1] / max(1, n - 1)) or 0.06)))
        sp = outer[::step] + ([outer[-1]] if (n - 1) % step else [])
        courses = max(1, int(stone_h / 0.24))
        for c in range(courses):
            for k in range(len(sp) - 1):
                a, b = sp[k], sp[k + 1]
                if rng.random() < 0.06:
                    continue
                mid = a.lerp(b, 0.5 + (rng.random() - 0.5) * 0.3)
                d = (b - a)
                if d.length < 1e-4:
                    continue
                yaw = math.atan2(d.y, d.x)
                sz = 0.32 + rng.random() * 0.3
                rock(mb, mid + V((0, 0, 0.12 + c * 0.22 + (rng.random() - 0.5) * 0.04)) - nrm[min(n - 1, k * step)] * c * 0.04,
                     (sz, 0.35 + rng.random() * 0.2, 0.2 + rng.random() * 0.1), rng, sub=2, yaw=yaw)
    if grass_top > 0:
        def filt(p, nn):
            return nn.z > 0.35 or rng.random() < grass_face / grass_top
        for p, nn in scatter_on_faces(mb, f0, f1, grass_top, rng, filt):
            top = nn.z > 0.35
            grass_tuft(mb, p - nn * 0.01, nn, rng, height=(0.22 if top else 0.14) * (0.6 + rng.random() * 0.9),
                       blades=rng.randint(5, 10) if top else rng.randint(3, 6), lean=0.6, droop=0.3 if not top else None)
    return f0, f1


def curved_board_wall(mb, pts, z0, top_fn, rng, side, openings=(), seg=1.4, mat=WOOD, sill=True, plate=True):
    """Vertical-board wall following a curved plan polyline `pts` (2D Vectors, dense).
    side=+1: outward is to the right of travel. openings: (x_centre_along_path_m, w, zb, zt)."""
    S = [0.0]
    for a, b in zip(pts, pts[1:]):
        S.append(S[-1] + (b - a).length)
    total = S[-1]
    nseg = max(1, int(round(total / seg)))
    knots = []
    j = 0
    for k in range(nseg + 1):
        target = total * k / nseg
        while j < len(S) - 2 and S[j + 1] < target:
            j += 1
        t = (target - S[j]) / max(1e-6, S[j + 1] - S[j])
        knots.append((pts[j].lerp(pts[j + 1], min(1, max(0, t))), target))
    for (a, sa), (b, sb) in zip(knots, knots[1:]):
        d = (b - a).normalized()
        on = V((d.y, -d.x, 0)) * side
        ops = [(c - sa, w, zb, zt) for (c, w, zb, zt) in openings if sa - w < c < sb + w]
        board_wall(mb, a, b, z0, lambda s_, a=a, b=b, L=(b - a).length: top_fn(a.lerp(b, s_ / L)), rng, on,
                   openings=ops, sill=False, plate=False, post=0.02, mat=mat)
    if sill:
        sp = [p + V((0, 0, z0 + 0.09)) for p in pts[::4]] + [pts[-1] + V((0, 0, z0 + 0.09))]
        sweep(mb, sp, prof_circle(0.11, 8, rng, 0.05, ry=0.085), mat, rng=rng)
    if plate:
        sp = [p + V((0, 0, top_fn(p) - 0.05)) for p in pts[::4]] + [pts[-1] + V((0, 0, top_fn(pts[-1]) - 0.05))]
        sweep(mb, sp, prof_circle(0.1, 8, rng, 0.05, ry=0.085), mat, rng=rng)


def smoke_louvre(mb, rf, a, rng, w=0.8, d=0.6, h=0.45, mat=WOOD):
    """Little boarded box with its own two-board roof over the smoke hole on the ridge."""
    zr = rf.z(a, 0) + 0.25
    for sy in (-1, 1):
        board(mb, V((a - w / 2, sy * d / 2, zr)), V((a + w / 2, sy * d / 2, zr)), h, 0.04, mat, up=V((0, sy, 0)), rng=rng)
    for sx in (-1, 1):
        board(mb, V((a + sx * w / 2, -d / 2, zr + h * 0.25)), V((a + sx * w / 2, d / 2, zr + h * 0.25)), h * 0.5, 0.04, mat,
              up=V((sx, 0, 0)), rng=rng)
    for sy in (-1, 1):
        p0 = V((a - w / 2 - 0.12, sy * 0.02, zr + h / 2 + 0.28))
        p1 = V((a + w / 2 + 0.12, sy * 0.02, zr + h / 2 + 0.28))
        off = V((0, sy * 0.26, -0.2))
        board(mb, p0 + off, p1 + off, 0.6, 0.035, mat, up=V((0, sy * 0.6, 0.8)).normalized(), rng=rng)


def boat(mb, M, rng, L=5.4, B=1.5, D=0.55, strakes=4, mat=WOOD, upturned=True):
    """Small clinker-built rowing boat (strakes as real overlapping planks, keel, stems).
    M: frame at the boat's centre on the ground; local X along the boat."""
    def hull(u, v, side):
        w = max(0.0, 1 - abs(u) ** 2.2) ** 0.55
        y = side * B / 2 * math.sin(v * math.pi / 2) * w
        z = D * (1 - math.cos(v * math.pi / 2)) * (0.65 + 0.35 * w) + 0.18 * abs(u) ** 2.5   # sheer rises to the stems
        return V((u * L / 2, y, z))
    pts_all = []
    for side in (-1, 1):
        for k in range(strakes):
            vc = (k + 0.5) / strakes
            n = 24
            pts, ups = [], []
            for i in range(n + 1):
                u = -0.985 + 1.97 * i / n
                p = hull(u, vc, side)
                p2 = hull(u, vc + 0.02, side)
                p1 = hull(u, vc - 0.02, side)
                t = V((1, 0, 0))
                dv = (p2 - p1).normalized()
                nrm = t.cross(dv) * side
                if nrm.length < 1e-4:
                    nrm = V((0, side, 0))
                pts.append(p + nrm.normalized() * 0.012 * k)
                ups.append(nrm.normalized())
            wid = (hull(0, (k + 1) / strakes, 1) - hull(0, k / strakes, 1)).length * 1.12

            def pf(i, tt, wid=wid):
                u = -0.985 + 1.97 * tt
                f = max(0.18, max(0.0, 1 - abs(u) ** 2.2) ** 0.55)
                return [(a * f, b) for (a, b) in prof_rect(wid, 0.018, 0.004)]
            if upturned:
                pts = [V((p.x, p.y, D + 0.25 - p.z)) for p in pts]
                ups = [V((u_.x, u_.y, -u_.z)) for u_ in ups]
            pts_all.append((pts, ups, pf, wid))
    start = len(mb.V)
    for pts, ups, pf, wid in pts_all:
        sweep(mb, pts, prof_rect(wid, 0.018, 0.004), mat, ups=ups, prof_fn=pf, rng=rng)
    # keel and stems
    kp = []
    for i in range(21):
        u = -1.0 + 2.0 * i / 20
        p = hull(u, 0.0, 1)
        kp.append(p)
    if upturned:
        kp = [V((p.x, p.y, D + 0.25 - p.z + 0.03)) for p in kp]
    sweep(mb, kp, prof_rect(0.06, 0.08, 0.01), mat, rng=rng, up=V((0, 1, 0)))
    for s in (-1, 1):
        a = hull(s * 1.0, 0.0, 1)
        b = hull(s * 1.0, 1.0, 1) + V((s * 0.08, 0, 0.15))
        if upturned:
            a = V((a.x, 0, D + 0.25 - a.z))
            b = V((b.x, 0, D + 0.25 - b.z))
        board(mb, a, b, 0.06, 0.06, mat, up=V((0, 1, 0)), rng=rng)
    # gunwale ribs (thwarts are hidden when upturned)
    mb.transform(M, start)
    if upturned:
        # blocks it rests on
        for x in (-L * 0.3, L * 0.3):
            for sy in (-1, 1):
                rock(mb, M @ V((x, sy * B * 0.42, 0.1)), (0.35, 0.3, 0.25), rng)


def carved_post(mb, p0, p1, r, rng, mat=WOOD, rings=5):
    """A post with turned/carved ring bands (entrance posts, market posts)."""
    p0, p1 = V(p0), V(p1)
    pts = line_pts(p0, p1, 0.06)
    base = prof_circle(r, 12)

    def pf(i, t):
        f = 1.0 + 0.12 * max(0.0, math.cos(t * rings * 2 * math.pi)) ** 6 - 0.05 * (t > 0.96)
        if t > 0.9:
            f *= 1 - (t - 0.9) * 2.5
        return [(a * f, b * f) for (a, b) in base]
    sweep(mb, pts, base, mat, prof_fn=pf, rng=rng, up=V((1, 0, 0)))


def flagstones(mb, center, radius, rng, size=0.7, avoid=None):
    """Irregular flat paving stones laid with grassy gaps over a disc."""
    c = V(center)
    step = size * 0.92
    n = int(radius / step) + 1
    for i in range(-n, n + 1):
        for j in range(-n, n + 1):
            x = i * step + (rng.random() - 0.5) * size * 0.35 + (j % 2) * step * 0.5
            y = j * step * 0.9 + (rng.random() - 0.5) * size * 0.35
            if x * x + y * y > radius * radius * (0.85 + 0.3 * rng.random()):
                continue
            p = c + V((x, y, 0))
            if avoid and avoid(p):
                continue
            s = size * (0.75 + rng.random() * 0.45)
            rock(mb, p + V((0, 0, -0.02)), (s, s * (0.7 + rng.random() * 0.3), 0.16), rng, sub=2, tilt=0.03, rough=0.15,
                 squash=0.45)
