"""
House recipes. Each recipe(rng) returns (model MB, dressing MB):
  model    -> exported (one mesh object, front door facing local +X, origin at ground centre)
  dressing -> preview-only ground grass so renders sit in a field (never exported)

Recipes build in the "house frame" of kit_parts (ridge along X) and finish with
face_front(), which rotates the mesh so the main door faces +X.
"""
import math

from mathutils import Matrix

from kit_geom import (V, MB, WOOD, GREY, STONE, TURF, GRASS, rock, stone_course, board, log, sweep,
                      prof_rect, prof_circle, frame_from, grass_tuft, line_pts, prism)
from kit_parts import (GableRoof, roof_frame, turf_roof, shingle_roof, bargeboards, log_walls, board_wall,
                       corner_post, door, window, woodpile, barrel, ladder, chopping_block, fish_rack,
                       net_drape, lean_poles, stone_scatter, wall_base_grass, ground_grass, fillet_path, turf_wall,
                       smooth, curved_board_wall, smoke_louvre, boat, carved_post, flagstones)


def face_front(mbs, door_dir):
    """Rotate so the door direction (house frame: '+x', '-x', '+y', '-y') becomes +X."""
    ang = {'+x': 0.0, '+y': -math.pi / 2, '-x': math.pi, '-y': math.pi / 2}[door_dir]
    R = Matrix.Rotation(ang, 4, 'Z')
    for mb in mbs:
        mb.transform(R)


def wall_frame(origin, outward):
    """Frame for doors/windows: X along the wall, Y = outward, Z up."""
    out = V(outward).normalized()
    return frame_from(origin, out.cross(V((0, 0, 1))), out)


def rect(L, W, x0=0.0, y0=0.0):
    return [V((x0 - L / 2, y0 - W / 2, 0)), V((x0 + L / 2, y0 - W / 2, 0)), V((x0 + L / 2, y0 + W / 2, 0)), V((x0 - L / 2, y0 + W / 2, 0))]


def rect_grass(dress, L, W, rng, band=0.7, density=30, h=0.4, field=14.0, keep_clear=()):
    """Preview-only dressing: tall grass along the wall foot and a rough pasture around."""
    pts = rect(L + 0.3, W + 0.3) + [rect(L + 0.3, W + 0.3)[0]]

    def on(a, b):
        d = (b - a).normalized()
        return V((d.y, -d.x, 0))
    wall_base_grass(dress, pts, rng, on, band=band, density=density, h=h)

    def avoid(p):
        if abs(p.x) < L / 2 + 0.2 and abs(p.y) < W / 2 + 0.2:
            return True
        return any((p - V((c[0], c[1], p.z))).length < c[2] for c in keep_clear)
    stone_scatter(dress, V((0, 0, 0)), field * 0.8, 25, rng, 0.1, 0.4)


# ---------------------------------------------------------------------------

def cottage_a(rng):
    """Small laft log cottage on a drystone footing, low turf roof with a crossed,
    scroll-cut wind board at each gable. Door in the long wall."""
    m, dress = MB(), MB()
    L, W, z0 = 8.0, 5.6, 0.42
    He = 2.75
    pitch = math.radians(27)
    rf = GableRoof(L, W, He, He + W / 2 * math.tan(pitch), oe=0.55, og=0.5, sag=0.12, rng=rng)
    stone_course(m, rect(L, W), rng, h=0.26, depth=0.5, z0=-0.12, courses=2, size=0.5, closed=True)
    for c in rect(L, W):  # big corner stones
        rock(m, c + V((0, 0, 0.12)), (0.75, 0.7, 0.5), rng, sub=3)
    door_c, door_w, door_h = 0.7, 0.9, 1.75
    openings = [('+y', door_c, door_w + 0.26, z0, z0 + door_h + 0.2),
                ('+y', -2.3, 0.75, z0 + 1.0, z0 + 1.6),
                ('+x', 0.9, 0.7, z0 + 1.05, z0 + 1.6)]
    log_walls(m, rf, L, W, z0, rng, d=0.23, crown=0.3, openings=openings)
    rlog = 0.23 * 0.6
    door(m, wall_frame(V((door_c, W / 2 + rlog * 0.9, z0)), V((0, 1, 0))), door_w, door_h, rng,
         depth=0.35)
    window(m, wall_frame(V((-2.3, W / 2 + rlog * 0.9, z0 + 1.05)), V((0, 1, 0))), 0.55, 0.45, rng)
    window(m, wall_frame(V((L / 2 + rlog * 0.9, 0.9, z0 + 1.1)), V((1, 0, 0))), 0.5, 0.42, rng,
           shutter=False)
    roof_frame(m, rf, rng)
    turf_roof(m, rf, rng, thick=0.22, grass_density=110, grass_h=0.2)
    bargeboards(m, rf, rng, carve='scroll')
    # yard
    woodpile(m, frame_from(V((-L / 2 - 0.25, -W / 2 + 0.3, 0.0)), V((0, 1, 0)), V((-1, 0, 0))), 3.6, 1.35, rng)
    chopping_block(m, V((door_c + 2.6, W / 2 + 2.0, 0)), rng)
    barrel(m, V((door_c + 1.15, W / 2 + 0.55, 0)), rng)
    ladder(m, V((L / 2 + 1.2, -1.2, 0.0)), V((L / 2 + 0.25, -1.4, He + 0.6)), rng)
    for i in range(4):  # door step stones
        rock(m, V((door_c + (i - 1.5) * 0.12, W / 2 + 0.55 + i * 0.05, 0.02)), (0.7, 0.5, 0.18), rng, sub=2)
    rect_grass(dress, L, W, rng, keep_clear=[(door_c, W / 2 + 1.2, 1.0)])
    face_front([m, dress], '+y')
    return m, dress


def cottage_b(rng):
    """Board-on-board timber cottage on a stone footing, steep roof of rounded riven
    shingles, door in the front gable, net drying on a rail along the side."""
    m, dress = MB(), MB()
    L, W, z0, He = 6.8, 5.0, 0.48, 2.55
    rf = GableRoof(L, W, He, He + W / 2 * math.tan(math.radians(46)), oe=0.45, og=0.42, sag=0.09, rng=rng)
    stone_course(m, rect(L, W), rng, h=0.25, depth=0.45, z0=-0.1, courses=2, size=0.45, closed=True)
    deck = 0.045

    def gable_top(x):
        return lambda s_: rf.z(x, -W / 2 + s_) - deck - 0.03
    door_y, dw, dh = -0.25, 0.9, 1.8
    t2 = 0.035 * 2
    board_wall(m, (-L / 2, -W / 2), (L / 2, -W / 2), z0, lambda s_: He - 0.04, rng, (0, -1),
               openings=[(L / 2 - 1.6, 0.6, z0 + 1.05, z0 + 1.5)])
    board_wall(m, (L / 2, W / 2), (-L / 2, W / 2), z0, lambda s_: He - 0.04, rng, (0, 1))
    board_wall(m, (L / 2, -W / 2), (L / 2, W / 2), z0, gable_top(L / 2), rng, (1, 0),
               openings=[(W / 2 + door_y, dw + 0.28, z0, z0 + dh + 0.2), (W / 2 + 0.1, 0.5, He + 0.5, He + 1.0)],
               plate=False)
    board_wall(m, (-L / 2, W / 2), (-L / 2, -W / 2), z0, lambda s_: rf.z(-L / 2, W / 2 - s_) - deck - 0.03, rng, (-1, 0),
               plate=False)
    for c in rect(L, W):
        corner_post(m, c, z0, He, rng)
    door(m, wall_frame(V((L / 2 + t2, door_y, z0)), V((1, 0, 0))), dw, dh, rng, depth=0.3)
    window(m, wall_frame(V((L / 2 + t2, 0.1, He + 0.5)), V((1, 0, 0))), 0.45, 0.42, rng)
    window(m, wall_frame(V((L / 2 - 1.6, -W / 2 - t2, z0 + 1.05)), V((0, -1, 0))), 0.5, 0.4, rng, shutter=True)
    roof_frame(m, rf, rng, deck_t=deck)
    shingle_roof(m, rf, rng, shape='round')
    bargeboards(m, rf, rng, carve='horn', top_lift=0.16, w=0.26)
    # net rail on two pegs along the -y wall, net over it
    yr = -W / 2 - 0.3
    for x in (-2.4, 0.4):
        board(m, V((x, -W / 2, 1.95)), V((x, yr - 0.05, 1.95)), 0.06, 0.06, WOOD, rng=rng)
    log(m, V((-2.8, yr, 2.0)), V((0.8, yr, 2.0)), 0.04, WOOD, rng, n=8)
    net_drape(m, V((-2.6, yr + 0.02, 2.04)), V((0.6, yr + 0.02, 2.04)), 1.5, rng)
    woodpile(m, frame_from(V((-L / 2 + 0.3, W / 2 + 0.15, 0.0)), V((1, 0, 0)), V((0, 1, 0))), 4.2, 1.5, rng)
    barrel(m, V((L / 2 + 0.55, door_y + 1.1, 0)), rng, r=0.28, h=0.75)
    barrel(m, V((L / 2 + 0.5, door_y + 1.75, 0)), rng, r=0.25, h=0.7)
    lean_poles(m, V((L / 2 + 0.15, -W / 2 + 0.4, 0)), 3, rng)
    for i in range(3):
        rock(m, V((L / 2 + 0.45 + i * 0.32, door_y + (rng.random() - 0.5) * 0.2, 0.0)), (0.6, 0.85, 0.22), rng)
    rect_grass(dress, L, W, rng, keep_clear=[(L / 2 + 1.5, door_y, 1.2)])
    face_front([m, dress], '+x')
    return m, dress


def turf_unit(m, rng, x_f, y0, L_in, W_in, Hw=1.7, pitch=40, T_base=1.6, T_top=0.9, sides=(True, True),
              front='door', carve='none', back=True, grass=80, coarse=False):
    """One Icelandic-style turf house: sod walls on three sides with rounded back corners,
    a turf roof running down onto them, and a tarred timber front gable at x = x_f.
    coarse: lighter sod-wall mesh for rows of several houses."""
    wk = dict(dh=0.05, grass_top=30, grass_face=3.0) if coarse else {}
    ds = 0.09 if coarse else 0.06
    cy = W_in / 2 + 0.55
    x_b = x_f - L_in - 0.55
    He = Hw + 0.1
    rf = GableRoof(x_f - x_b, 2 * cy, He, He + cy * math.tan(math.radians(pitch)), oe=T_top / 2 + 0.08,
                   og=0.4, og0=0.0, sag=0.1, rng=rng, x0=(x_f + x_b) / 2)
    # rf works in its own y; shift everything by y0 afterwards
    start = len(m.V)

    def H(p):
        hg = max(Hw, rf.z(x_b + 0.02, p.y) - 0.12)
        w = smooth((x_b + 1.3 - p.x) / 1.3)
        return Hw + (hg - Hw) * w
    if back and sides[0] and sides[1]:
        corners = [V((x_f, cy, 0)), V((x_b, cy, 0)), V((x_b, -cy, 0)), V((x_f, -cy, 0))]
        turf_wall(m, fillet_path(corners, 1.3, ds=ds), rng, H, T_base, T_top, **wk)
    else:
        if sides[1]:
            turf_wall(m, fillet_path([V((x_f, cy, 0)), V((x_b - 0.4, cy, 0))], 1.0, ds=ds), rng, lambda p: Hw, T_base, T_top, **wk)
        if sides[0]:
            turf_wall(m, fillet_path([V((x_b - 0.4, -cy, 0)), V((x_f, -cy, 0))], 1.0, ds=ds), rng, lambda p: Hw, T_base, T_top, **wk)
        if back:
            turf_wall(m, fillet_path([V((x_b, cy + 0.5, 0)), V((x_b, -cy - 0.5, 0))], 1.0, ds=ds), rng, H, T_base, T_top, **wk,
                      cap_ends=False)
    roof_frame(m, rf, rng)
    turf_roof(m, rf, rng, thick=0.26, grass_density=grass, grass_h=0.22, end_taper=(False, True), res=0.18 if coarse else 0.14)
    bargeboards(m, rf, rng, carve=carve, ends=(False, True), top_lift=0.34, ext=0.35 if carve == 'none' else 0.5)
    # timber front gable
    z0 = 0.28
    gy = W_in / 2 + 0.35
    xg = x_f - 0.08
    stone_course(m, [V((xg + 0.05, -gy - 0.2, 0)), V((xg + 0.05, gy + 0.2, 0))], rng, h=0.2, depth=0.35, z0=-0.08,
                 courses=1, size=0.4)
    dw, dh = 0.85, 1.7
    ops = []
    if front == 'door':
        ops.append((gy, dw + 0.28, z0, z0 + dh + 0.2))
    ops.append((gy + (0.0 if front != 'door' else 0.0), 0.5, He + 0.6, He + 1.05))
    if front == 'window':
        ops.append((gy, 0.6, z0 + 0.95, z0 + 1.45))
    board_wall(m, (xg, -gy), (xg, gy), z0, lambda s_: rf.z(x_f, -gy + s_) - 0.08, rng, (1, 0), openings=ops,
               plate=False, post=0.12)
    t2 = 0.07
    if front == 'door':
        door(m, wall_frame(V((xg + t2, 0.0, z0)), V((1, 0, 0))), dw, dh, rng, depth=0.3)
    else:
        window(m, wall_frame(V((xg + t2, 0.0, z0 + 0.95)), V((1, 0, 0))), 0.55, 0.5, rng)
    window(m, wall_frame(V((xg + t2, 0.0, He + 0.6)), V((1, 0, 0))), 0.42, 0.42, rng, shutter=False)
    m.transform(Matrix.Translation((0, y0, 0)), start)
    return rf, cy


def turf_a(rng):
    """Single turf house: thick herringbone-sod walls on a stone base, grass roof that runs
    down onto them, timber front gable with the door."""
    m, dress = MB(), MB()
    turf_unit(m, rng, 4.0, 0.0, 8.0, 4.6, carve='none')
    barrel(m, V((4.6, 1.6, 0)), rng, r=0.28, h=0.75)
    chopping_block(m, V((6.2, -2.4, 0)), rng)
    woodpile(m, frame_from(V((5.2, -4.6, 0.0)), V((-1, 0, 0)), V((0, -1, 0))), 2.0, 1.1, rng, depth_rows=1)
    for i in range(4):
        rock(m, V((4.4 + i * 0.35, (rng.random() - 0.5) * 0.3, 0.0)), (0.55, 0.9, 0.2), rng)
    rect_grass(dress, 9.6, 7.6, rng, band=0.0, keep_clear=[(5.5, 0, 1.4)])
    return m, dress


def turf_b(rng):
    """A row of three turf houses side by side (gables to the yard), sharing their sod
    walls, fronts slightly staggered: the main house has the door."""
    m, dress = MB(), MB()
    units = [(4.2, 9.5, 0.3, 'window', 1.6), (3.4, 7.2, 0.0, 'door', 1.75), (4.6, 10.0, 0.5, 'window', 1.65)]
    cys = [w / 2 + 0.55 for (w, *_r) in units]
    total = 2 * sum(cys)
    y = -total / 2
    for k, (w, L_in, xf, front, hw) in enumerate(units):
        cy = cys[k]
        yc = y + cy
        sides = (k == 0 or True, True) if k != 1 else (False, False)
        if k == 0:
            sides = (True, True)
        turf_unit(m, rng, 4.0 + xf, yc, L_in, w, Hw=hw, pitch=40 + k * 2, sides=sides, front=front,
                  back=True, carve='horn' if k == 1 else 'none', grass=14, coarse=True)
        y += 2 * cy
    barrel(m, V((5.3, 1.2, 0)), rng, r=0.28, h=0.75)
    fish_rack(m, frame_from(V((7.5, -6.0, 0)), V((0, 1, 0)), V((-1, 0, 0))), 4.0, rng)
    rect_grass(dress, 11.5, total + 1.5, rng, band=0.0, keep_clear=[(5.5, 0, 1.4)])
    return m, dress


def boat_hw(L, wmax, wend, x0=0.0):
    """Half-width of a boat-shaped (bow-sided) plan."""
    return lambda a: wmax / 2 - (wmax - wend) / 2 * min(1.0, ((a - x0) / (L / 2)) ** 2)


def longhouse_a(rng):
    """Bow-sided longhouse: curved stave walls on a stone sill with raking outer
    posts, a bowed ridge, pointed shingle roof with two smoke louvres; door in the long
    wall and a second in the end gable."""
    m, dress = MB(), MB()
    L, wmax, wend, z0, He = 22.0, 7.8, 5.8, 0.4, 2.6
    hw = boat_hw(L, wmax, wend)
    rf = GableRoof(L, wmax, He, He + wmax / 2 * math.tan(math.radians(40)), oe=0.55, og=0.45, sag=0.28, rng=rng,
                   hw_fn=hw, bow=0.75)
    n = 60
    xs = [-L / 2 + L * i / n for i in range(n + 1)]
    door_x, dw, dh = 2.5, 1.0, 1.72
    for sgn in (-1, 1):
        pts = [V((x, sgn * hw(x), 0)) for x in xs]
        if sgn > 0:
            pts = list(reversed(pts))
        foot = [p for p in pts[::3]] + [pts[-1]]
        stone_course(m, foot, rng, h=0.22, depth=0.42, z0=-0.08, courses=2, size=0.42)
        ops = []
        if sgn > 0:
            # door position measured along the reversed (+y) path from x = +L/2
            ops = [((L / 2 - door_x) * 1.004, dw + 0.3, z0, z0 + dh + 0.2)]
        curved_board_wall(m, pts, z0, lambda p: He - 0.03, rng, 1, openings=ops)
        # raking outer posts
        x = -L / 2 + 1.6
        while x < L / 2 - 1.0:
            if not (sgn > 0 and abs(x - door_x) < 1.2):
                foot_p = V((x, sgn * (hw(x) + 1.0), -0.1))
                top_p = V((x, sgn * (hw(x) + 0.12), He - 0.25))
                log(m, foot_p, top_p, 0.12, WOOD, rng, n=10, taper=0.15, up=V((1, 0, 0)))
                rock(m, foot_p + V((0, 0, 0.05)), (0.5, 0.45, 0.25), rng)
            x += 2.2
    for gx in (-1, 1):
        y0, y1 = (-hw(gx * L / 2), hw(gx * L / 2)) if gx > 0 else (hw(gx * L / 2), -hw(gx * L / 2))
        ops = [((y1 - y0) / 2 * (1 if gx > 0 else 1), 0.95, z0, z0 + 1.95)] if gx > 0 else []
        board_wall(m, (gx * L / 2, y0), (gx * L / 2, y1), z0, (lambda gx_, y0_: (lambda s_: rf.z(gx_ * L / 2, y0_ + s_ * (1 if gx_ > 0 else -1)) - 0.08))(gx, y0),
                   rng, (gx, 0), openings=ops, plate=False)
        stone_course(m, [V((gx * L / 2, -hw(gx * L / 2), 0)), V((gx * L / 2, hw(gx * L / 2), 0))], rng, h=0.22,
                     depth=0.42, z0=-0.08, courses=2, size=0.42)
    door(m, wall_frame(V((L / 2 + 0.07, 0.0, z0)), V((1, 0, 0))), 0.85, 1.8, rng, depth=0.3)
    yd = hw(door_x) + 0.07
    door(m, wall_frame(V((door_x, yd, z0)), V((0, 1, 0))), dw, dh, rng, depth=0.35, lintel_ext=0.3)
    roof_frame(m, rf, rng, purlins=4)
    shingle_roof(m, rf, rng, shape='point', w=0.15, expo=0.18)
    bargeboards(m, rf, rng, carve='scroll', top_lift=0.16, w=0.3, ext=0.7)
    smoke_louvre(m, rf, -3.0, rng)
    smoke_louvre(m, rf, 4.5, rng)
    # yard: woodpile, barrels, a boat-shaped log bench
    woodpile(m, frame_from(V((-6.0, hw(-6) + 1.25, 0.0)), V((1, 0, 0)), V((0, 1, 0))), 3.0, 1.3, rng, depth_rows=1)
    barrel(m, V((door_x + 1.5, yd + 0.6, 0)), rng)
    barrel(m, V((door_x + 2.1, yd + 0.4, 0)), rng, r=0.26, h=0.7)
    chopping_block(m, V((door_x - 3.0, yd + 2.4, 0)), rng)
    for i in range(4):
        rock(m, V((door_x + (rng.random() - 0.5) * 0.4, yd + 0.4 + i * 0.4, 0.0)), (0.8, 0.55, 0.2), rng)
    rect_grass(dress, L, wmax, rng, band=0.0, keep_clear=[(door_x, yd + 1.5, 1.6)])
    face_front([m, dress], '+y')
    return m, dress


def longhouse_b(rng):
    """Sod longhouse: bow-sided turf walls on drystone, rounded back end, long grass roof
    with a smoke louvre, tarred timber front gable with the door."""
    m, dress = MB(), MB()
    L_in, wmax_in, wend_in = 20.0, 6.0, 4.6
    T_base, T_top, Hw = 1.7, 1.0, 1.75
    x_f = L_in / 2 + 0.3
    x_b = x_f - L_in - 0.6
    x0 = (x_f + x_b) / 2
    Lr = x_f - x_b
    hwc = boat_hw(Lr * 1.05, wmax_in + 1.2, wend_in + 1.2, x0)   # wall centre line
    He = Hw + 0.1
    rf = GableRoof(Lr, wmax_in + 1.2, He, He + (wmax_in / 2 + 0.6) * math.tan(math.radians(38)), oe=T_top / 2 + 0.08,
                   og=0.4, og0=0.0, sag=0.2, rng=rng, x0=x0, hw_fn=hwc, bow=0.35)

    def H(p):
        hg = max(Hw, rf.z(x_b + 0.02, p.y) - 0.12)
        w = smooth((x_b + 1.5 - p.x) / 1.5)
        return Hw + (hg - Hw) * w
    n = 40
    side_p = [V((x_f - (x_f - x_b) * i / n, hwc(x_f - (x_f - x_b) * i / n), 0)) for i in range(n + 1)]
    corners = side_p + [V((x_b, -hwc(x_b), 0))] + [V((x_b + (x_f - x_b) * i / n, -hwc(x_b + (x_f - x_b) * i / n), 0)) for i in range(1, n + 1)]
    # replace the two back corner points with a fillet
    path = fillet_path([side_p[0], side_p[n // 2], V((x_b, hwc(x_b), 0)), V((x_b, -hwc(x_b), 0)),
                        V((x0, -hwc(x0), 0)), V((x_f, -hwc(x_f), 0))], 1.6, ds=0.09)
    # re-bow the straight runs of the fillet path onto the curved plan
    bowed = []
    for p in path:
        if p.x > x_b + 1.6:
            p = V((p.x, math.copysign(hwc(p.x), p.y), 0))
        bowed.append(p)
    turf_wall(m, bowed, rng, H, T_base, T_top, dh=0.05, grass_top=35)
    roof_frame(m, rf, rng, purlins=3)
    turf_roof(m, rf, rng, thick=0.28, grass_density=32, grass_h=0.24, end_taper=(False, True))
    bargeboards(m, rf, rng, carve='horn', ends=(False, True), top_lift=0.36)
    smoke_louvre(m, rf, x0 - 2.0, rng)
    z0 = 0.28
    gy = hwc(x_f) - 0.45
    xg = x_f - 0.08
    stone_course(m, [V((xg + 0.05, -gy - 0.2, 0)), V((xg + 0.05, gy + 0.2, 0))], rng, h=0.2, depth=0.35, z0=-0.08,
                 courses=1, size=0.4)
    ops = [(gy, 1.15, z0, z0 + 2.0), (gy - 1.4, 0.5, z0 + 1.0, z0 + 1.45), (gy, 0.5, He + 0.7, He + 1.15)]
    board_wall(m, (xg, -gy), (xg, gy), z0, lambda s_: rf.z(x_f, -gy + s_) - 0.08, rng, (1, 0), openings=ops,
               plate=False, post=0.12)
    door(m, wall_frame(V((xg + 0.07, 0.0, z0)), V((1, 0, 0))), 0.9, 1.8, rng, depth=0.3, open_angle=0.0)
    window(m, wall_frame(V((xg + 0.07, -1.4, z0 + 1.0)), V((1, 0, 0))), 0.45, 0.42, rng)
    window(m, wall_frame(V((xg + 0.07, 0.0, He + 0.7)), V((1, 0, 0))), 0.42, 0.42, rng, shutter=False)
    fish_rack(m, frame_from(V((x_f + 3.5, 3.0, 0)), V((0, 1, 0)), V((-1, 0, 0))), 3.6, rng)
    barrel(m, V((x_f + 0.7, -1.4, 0)), rng)
    woodpile(m, frame_from(V((x_f + 0.4, -gy - 1.9, 0.0)), V((1, 0, 0)), V((0, 1, 0))), 2.2, 1.0, rng, depth_rows=1)
    rect_grass(dress, Lr + 1.5, wmax_in + 3.0, rng, band=0.0, keep_clear=[(x_f + 1.5, 0, 1.5)])
    return m, dress


def hallhouse_a(rng):
    """Chieftain's hall: long, high and slightly bow-sided, board walls on a stone sill,
    steep pointed-shingle roof with three smoke louvres and long scroll-cut crossed wind
    boards, an open gallery (svalgang) on carved posts along the front, double doors."""
    m, dress = MB(), MB()
    L, wmax, wend, z0, He = 27.0, 9.4, 8.2, 0.5, 3.1
    hw = boat_hw(L, wmax, wend)
    rf = GableRoof(L, wmax, He, He + wmax / 2 * math.tan(math.radians(44)), oe=0.6, og=0.55, sag=0.2, rng=rng,
                   hw_fn=hw, bow=0.3)
    n = 70
    xs = [-L / 2 + L * i / n for i in range(n + 1)]
    dw, dh = 1.7, 2.3
    for sgn in (-1, 1):
        pts = [V((x, sgn * hw(x), 0)) for x in xs]
        if sgn > 0:
            pts = list(reversed(pts))
        stone_course(m, pts[::3] + [pts[-1]], rng, h=0.26, depth=0.5, z0=-0.1, courses=2, size=0.5)
        ops = []
        if sgn > 0:
            ops = [(L / 2 * 1.0005, dw + 0.36, z0, z0 + dh + 0.25), (L / 2 - 6.0, 0.6, z0 + 1.5, z0 + 2.0),
                   (L / 2 + 6.0, 0.6, z0 + 1.5, z0 + 2.0)]
        curved_board_wall(m, pts, z0, lambda p: He - 0.03, rng, 1, openings=ops)
    for gx in (-1, 1):
        y0, y1 = (-hw(gx * L / 2), hw(gx * L / 2)) if gx > 0 else (hw(gx * L / 2), -hw(gx * L / 2))
        ops = [((y1 - y0) / 2, 1.0, z0, z0 + 2.05), ((y1 - y0) / 2, 0.6, He + 1.2, He + 1.7)] if gx > 0 else [((y1 - y0) / 2, 0.6, He + 1.2, He + 1.7)]
        board_wall(m, (gx * L / 2, y0), (gx * L / 2, y1), z0,
                   (lambda gx_, y0_: (lambda s_: rf.z(gx_ * L / 2, y0_ + s_ * (1 if gx_ > 0 else -1)) - 0.08))(gx, y0),
                   rng, (gx, 0), openings=ops, plate=False)
        stone_course(m, [V((gx * L / 2, -hw(gx * L / 2), 0)), V((gx * L / 2, hw(gx * L / 2), 0))], rng, h=0.26,
                     depth=0.5, z0=-0.1, courses=2, size=0.5)
        window(m, wall_frame(V((gx * L / 2 + gx * 0.07, 0.0, He + 1.2)), V((gx, 0, 0))), 0.5, 0.45, rng, shutter=False)
    door(m, wall_frame(V((L / 2 + 0.07, 0.0, z0)), V((1, 0, 0))), 0.9, 1.9, rng, depth=0.3)
    yd = hw(0) + 0.07
    for sx in (-1, 1):   # double door
        door(m, wall_frame(V((sx * dw / 4, yd, z0)), V((0, 1, 0))), dw / 2 - 0.02, dh, rng, depth=0.4,
             lintel_ext=0.0 if sx < 0 else 0.4, ring=True, threshold=(sx > 0))
    for x in (-6.0, 6.0):
        window(m, wall_frame(V((x, hw(x) + 0.07, z0 + 1.5)), V((0, 1, 0))), 0.55, 0.45, rng)
    roof_frame(m, rf, rng, purlins=4)
    shingle_roof(m, rf, rng, shape='point', w=0.16, expo=0.19, length=0.55)
    bargeboards(m, rf, rng, carve='scroll', top_lift=0.18, w=0.36, ext=0.95)
    for a in (-7.0, 0.5, 7.5):
        smoke_louvre(m, rf, a, rng, w=1.0, d=0.7, h=0.5)
    # gallery: lean-to roof on carved posts along the front, 15 m long, 2.2 m deep
    gl, gd, gh = 15.0, 2.2, 2.35
    wall_y = hw(0) - 0.02
    rg = GableRoof(gl, 2 * gd, gh, He - 0.15, oe=0.35, og=0.3, sag=0.04, rng=rng)
    gs = len(m.V)
    roof_frame(m, rg, rng, purlins=2, sides=(1,))
    shingle_roof(m, rg, rng, shape='point', w=0.16, expo=0.19, length=0.55, sides=(1,), ridge=False)
    for gx in (-1, 1):   # end boards of the lean-to
        aa = gx * (gl / 2 + 0.3 + 0.03)
        pe = V((aa, gd + 0.35, rg.z(aa, gd + 0.35) + 0.08))
        pr = V((aa, 0.0, rg.z(aa, 0.0) + 0.08))
        board(m, pe, pr, 0.28, 0.05, WOOD, up=V((1, 0, 0)), rng=rng)
    x = -gl / 2 + 0.1
    while x <= gl / 2 + 0.01:
        carved_post(m, V((x, gd - 0.12, 0.0)), V((x, gd - 0.12, gh - 0.12)), 0.14, rng)
        rock(m, V((x, gd - 0.12, 0.02)), (0.55, 0.5, 0.2), rng)
        x += gl / 6
    log(m, V((-gl / 2 - 0.3, gd - 0.12, gh - 0.05)), V((gl / 2 + 0.3, gd - 0.12, gh - 0.05)), 0.13, WOOD, rng, flat=0.2, n=10)
    m.transform(Matrix.Translation((0, wall_y, 0)), gs)
    # paving under the gallery
    for i in range(int(gl / 0.65)):
        for j in range(3):
            xx = -gl / 2 + 0.3 + i * 0.65 + (rng.random() - 0.5) * 0.15 + (j % 2) * 0.3
            yy = wall_y + 0.45 + j * 0.62 + (rng.random() - 0.5) * 0.1
            sz = 0.55 + rng.random() * 0.25
            rock(m, V((xx, yy, -0.02)), (sz, sz * 0.85, 0.15), rng, tilt=0.03, rough=0.15, squash=0.45)
    # yard
    barrel(m, V((9.2, wall_y + 0.5, 0)), rng)
    barrel(m, V((9.8, wall_y + 0.75, 0)), rng, r=0.27, h=0.72)
    woodpile(m, frame_from(V((-12.0, hw(-12) + 1.0, 0.0)), V((1, 0, 0)), V((0, 1, 0))), 3.2, 1.4, rng, depth_rows=1)
    lean_poles(m, V((10.5, hw(10.5) + 0.12, 0)), 3, rng)
    rect_grass(dress, L, wmax + 4.5, rng, band=0.0, keep_clear=[(0, wall_y + 3, 2.0)])
    face_front([m, dress], '+y')
    return m, dress


def tall_a(rng):
    """Two-storey storehouse-dwelling (loft type): laft-log ground storey on corner
    stones, upper storey jettied out on protruding joists and clad in boards, steep
    shingle roof, ladder-stair to the upper door."""
    m, dress = MB(), MB()
    L, W, z0, h1 = 4.8, 4.2, 0.55, 2.25
    zf = z0 + h1
    for c in rect(L, W):
        for k in range(3):
            rock(m, c + V((0, 0, 0.1 + k * 0.2)), (0.6 - k * 0.08, 0.55 - k * 0.08, 0.22), rng, sub=2)
    stone_course(m, rect(L - 0.8, W - 0.8), rng, h=0.25, depth=0.4, z0=-0.1, courses=1, size=0.4, closed=True)
    dummy = GableRoof(L, W, zf, zf + 0.0015, rng=rng)
    dw, dh = 0.85, 1.7
    log_walls(m, dummy, L, W, z0, rng, d=0.22, crown=0.28,
              openings=[('+x', 0.0, dw + 0.26, z0, z0 + dh + 0.2), ('-y', -0.8, 0.5, z0 + 1.1, z0 + 1.5)])
    r = 0.22 * 0.6
    door(m, wall_frame(V((L / 2 + r * 0.9, 0.0, z0)), V((1, 0, 0))), dw, dh, rng, depth=0.32)
    window(m, wall_frame(V((-0.8, -W / 2 - r * 0.9, z0 + 1.1)), V((0, -1, 0))), 0.42, 0.36, rng)
    # joists: protruding log ends carry the jettied upper storey
    ov = 0.75
    Lu, Wu = L + 2 * ov, W + 2 * ov
    x = -Lu / 2 + 0.15
    while x < Lu / 2:
        log(m, V((x, -Wu / 2 - 0.08, zf + 0.06)), V((x, Wu / 2 + 0.08, zf + 0.06)), 0.1, WOOD, rng, flat=0.15, n=9)
        x += 0.62
    for sy in (-1, 1):
        log(m, V((-Lu / 2 - 0.1, sy * (Wu / 2 - 0.05), zf + 0.22)), V((Lu / 2 + 0.1, sy * (Wu / 2 - 0.05), zf + 0.22)), 0.1,
            WOOD, rng, flat=0.2, n=9)
    # curved brackets under the jetty corners
    for sx in (-1, 1):
        for sy in (-1, 1):
            p0 = V((sx * (L / 2 + 0.1), sy * (W / 2 + 0.1), zf - 0.9))
            p1 = V((sx * (Lu / 2 - 0.1), sy * (Wu / 2 - 0.1), zf))
            log(m, p0, p1, 0.07, WOOD, rng, n=8, bend=lambda t: V((0, 0, 0.12 * math.sin(math.pi * t))))
    zu = zf + 0.3
    He = zu + 2.0
    rf = GableRoof(Lu, Wu, He, He + Wu / 2 * math.tan(math.radians(42)), oe=0.4, og=0.35, sag=0.06, rng=rng)
    deck = 0.045
    udoor_x = -0.6
    board_wall(m, (-Lu / 2, -Wu / 2), (Lu / 2, -Wu / 2), zu, lambda s_: He - 0.04, rng, (0, -1),
               openings=[(Lu / 2 + udoor_x, 0.85 + 0.25, zu, zu + 1.75), (Lu / 2 + 1.6, 0.45, zu + 0.9, zu + 1.3)], sill=True)
    board_wall(m, (Lu / 2, Wu / 2), (-Lu / 2, Wu / 2), zu, lambda s_: He - 0.04, rng, (0, 1), sill=True)
    board_wall(m, (Lu / 2, -Wu / 2), (Lu / 2, Wu / 2), zu, lambda s_: rf.z(Lu / 2, -Wu / 2 + s_) - deck - 0.03, rng, (1, 0),
               openings=[(Wu / 2, 0.5, zu + 0.95, zu + 1.4), (Wu / 2, 0.42, He + 0.5, He + 0.9)], plate=False)
    board_wall(m, (-Lu / 2, Wu / 2), (-Lu / 2, -Wu / 2), zu, lambda s_: rf.z(-Lu / 2, Wu / 2 - s_) - deck - 0.03, rng,
               (-1, 0), plate=False)
    for c in rect(Lu, Wu):
        corner_post(m, c + V((0, 0, 0)), zu, He, rng, w=0.17)
    window(m, wall_frame(V((Lu / 2 + 0.07, 0.0, zu + 0.95)), V((1, 0, 0))), 0.45, 0.4, rng)
    window(m, wall_frame(V((Lu / 2 + 0.07, 0.0, He + 0.5)), V((1, 0, 0))), 0.38, 0.36, rng, shutter=False)
    door(m, wall_frame(V((udoor_x, -Wu / 2 - 0.07, zu)), V((0, -1, 0))), 0.85, 1.7, rng, depth=0.3, threshold=False)
    window(m, wall_frame(V((1.6, -Wu / 2 - 0.07, zu + 0.9)), V((0, -1, 0))), 0.42, 0.38, rng)
    roof_frame(m, rf, rng, deck_t=deck)
    shingle_roof(m, rf, rng, shape='round', w=0.13, expo=0.16)
    bargeboards(m, rf, rng, carve='scroll', top_lift=0.16, w=0.26, ext=0.55)
    # stair-ladder to the upper door
    ladder(m, V((udoor_x, -Wu / 2 - 1.5, 0.0)), V((udoor_x, -Wu / 2 - 0.12, zu + 0.05)), rng, w=0.6)
    barrel(m, V((L / 2 + 0.6, 1.2, 0)), rng, r=0.27, h=0.72)
    chopping_block(m, V((L / 2 + 2.2, -1.6, 0)), rng)
    rect_grass(dress, L + 0.6, W + 0.6, rng, band=0.0, keep_clear=[(L / 2 + 1.0, 0, 1.0)])
    face_front([m, dress], '+x')
    return m, dress


def tall_b(rng):
    """Narrow two-storey timber house: tarred board walls in two tiers with a drip board
    at the floor line, small windows on both floors, turf roof, door in the gable."""
    m, dress = MB(), MB()
    L, W, z0 = 6.2, 4.6, 0.5
    z1 = z0 + 2.45
    He = z1 + 2.3
    rf = GableRoof(L, W, He, He + W / 2 * math.tan(math.radians(33)), oe=0.5, og=0.45, sag=0.08, rng=rng)
    stone_course(m, rect(L, W), rng, h=0.26, depth=0.45, z0=-0.1, courses=2, size=0.45, closed=True)
    deck = 0.045
    dw, dh = 0.85, 1.8

    def gt(x, sgn):
        return lambda s_: rf.z(x, sgn * (-W / 2 + s_)) - deck - 0.03
    tiers = [(z0, lambda s_: z1), (z1 + 0.05, None)]
    walls = [((-L / 2, -W / 2), (L / 2, -W / 2), (0, -1), 'long'), ((L / 2, W / 2), (-L / 2, W / 2), (0, 1), 'long'),
             ((L / 2, -W / 2), (L / 2, W / 2), (1, 0), 'gp'), ((-L / 2, W / 2), (-L / 2, -W / 2), (-1, 0), 'gm')]
    for ti, (zb, top) in enumerate(tiers):
        for (a, b, on, kind) in walls:
            if top is None:
                tp = (lambda s_: He - 0.04) if kind == 'long' else (gt(L / 2, 1) if kind == 'gp' else gt(-L / 2, -1))
            else:
                tp = top
            ops = []
            if kind == 'gp':
                ops = [(W / 2, dw + 0.28, z0, z0 + dh + 0.2)] if ti == 0 else [(W / 2 - 0.9, 0.5, z1 + 0.9, z1 + 1.35),
                                                                            (W / 2 + 0.9, 0.5, z1 + 0.9, z1 + 1.35),
                                                                            (W / 2, 0.4, He + 0.4, He + 0.8)]
            if kind == 'long' and on[1] < 0:
                ops = [(L / 2 - 1.5, 0.55, (z0 if ti == 0 else z1) + 1.0, (z0 if ti == 0 else z1) + 1.45),
                       (L / 2 + 1.5, 0.55, (z0 if ti == 0 else z1) + 1.0, (z0 if ti == 0 else z1) + 1.45)]
            board_wall(m, a, b, zb, tp, rng, on, openings=ops, plate=(kind == 'long' and ti == 1), sill=True)
    for c in rect(L, W):
        corner_post(m, c, z0, He, rng, w=0.18)
    # drip board around the floor line
    for (a, b, on, kind) in walls:
        a3, b3 = V((a[0], a[1], z1 + 0.02)), V((b[0], b[1], z1 + 0.02))
        o = V((on[0], on[1], 0)) * 0.1
        d = (b3 - a3).normalized()
        board(m, a3 + o - d * 0.12, b3 + o + d * 0.12, 0.12, 0.035, WOOD, up=V((on[0], on[1], 0.6)).normalized(), rng=rng)
    t2 = 0.07
    door(m, wall_frame(V((L / 2 + t2, 0.0, z0)), V((1, 0, 0))), dw, dh, rng, depth=0.3)
    for y in (-0.9, 0.9):
        window(m, wall_frame(V((L / 2 + t2, y, z1 + 0.9)), V((1, 0, 0))), 0.45, 0.4, rng, shutter=(y > 0))
    window(m, wall_frame(V((L / 2 + t2, 0.0, He + 0.4)), V((1, 0, 0))), 0.36, 0.36, rng, shutter=False)
    for x in (-1.5, 1.5):
        for zz in (z0, z1):
            window(m, wall_frame(V((x, -W / 2 - t2, zz + 1.0)), V((0, -1, 0))), 0.5, 0.4, rng, shutter=(x > 0))
    roof_frame(m, rf, rng, deck_t=deck)
    turf_roof(m, rf, rng, thick=0.22, grass_density=90, grass_h=0.2)
    bargeboards(m, rf, rng, carve='horn', top_lift=0.3)
    ladder(m, V((L / 2 + 1.0, W / 2 - 0.6, 0.0)), V((L / 2 + 0.2, W / 2 - 0.6, He + 0.4)), rng)
    woodpile(m, frame_from(V((-L / 2 + 2.7, -W / 2 - 0.15, 0.0)), V((-1, 0, 0)), V((0, -1, 0))), 2.4, 1.3, rng, depth_rows=1)
    barrel(m, V((L / 2 + 0.6, -1.2, 0)), rng, r=0.28, h=0.75)
    rect_grass(dress, L, W, rng, band=0.0, keep_clear=[(L / 2 + 1, 0, 1)])
    face_front([m, dress], '+x')
    return m, dress


def shed_a(rng):
    """Boathouse (naust): drystone side and back walls, turf roof on a log frame, boarded
    front gable with a wide double door to the shore; an upturned boat and oars outside."""
    m, dress = MB(), MB()
    L, W, Hw = 8.5, 4.6, 1.5
    rf = GableRoof(L, W, Hw + 0.12, Hw + 0.12 + W / 2 * math.tan(math.radians(32)), oe=0.45, og=0.4, og0=0.15, sag=0.1,
                   rng=rng)
    # drystone walls: two faces of coursed stones with rubble between
    for sy in (-1, 1):
        for face in (-1, 1):
            y = sy * (W / 2 + face * 0.28)
            stone_course(m, [V((-L / 2 - 0.3, y, 0)), V((L / 2, y, 0))] if sy < 0 else [V((L / 2, y, 0)), V((-L / 2 - 0.3, y, 0))],
                         rng, h=0.26, depth=0.4, z0=-0.08, courses=6, size=0.5, batter=0.01)
    for face in (-1, 1):
        x = -L / 2 + face * 0.28
        stone_course(m, [V((x, W / 2 + 0.3, 0)), V((x, -W / 2 - 0.3, 0))], rng, h=0.26, depth=0.4, z0=-0.08, courses=6,
                     size=0.5, batter=0.01)
    # back gable above the stone wall: boards
    deck = 0.045
    board_wall(m, (-L / 2, W / 2 + 0.1), (-L / 2, -W / 2 - 0.1), Hw - 0.05,
               lambda s_: rf.z(-L / 2, W / 2 + 0.1 - s_) - deck - 0.03, rng, (-1, 0), plate=False, sill=True)
    # front gable with the double door
    z0 = 0.15
    dw, dh = 2.6, 1.75
    board_wall(m, (L / 2, -W / 2 - 0.35), (L / 2, W / 2 + 0.35), z0, lambda s_: rf.z(L / 2, -W / 2 - 0.35 + s_) - deck - 0.03,
               rng, (1, 0), openings=[(W / 2 + 0.35, dw + 0.3, z0, z0 + dh + 0.25)], plate=False)
    for sx in (-1, 1):
        door(m, wall_frame(V((L / 2 + 0.07, sx * dw / 4, z0)), V((1, 0, 0))), dw / 2 - 0.03, dh, rng, depth=0.3,
             ring=(sx > 0), lintel_ext=0.3 if sx > 0 else 0.0, threshold=False)
    roof_frame(m, rf, rng, purlins=2)
    turf_roof(m, rf, rng, thick=0.24, grass_density=90, grass_h=0.24)
    bargeboards(m, rf, rng, carve='none', top_lift=0.32, ends=(True, True), ext=0.3)
    boat(m, frame_from(V((L / 2 + 3.0, -2.6, 0)), V((1, 0.25, 0)), V((-0.25, 1, 0))), rng)
    lean_poles(m, V((L / 2 + 0.15, W / 2 - 0.2, 0)), 4, rng, h=3.0)
    net_drape(m, V((L / 2 + 0.2, -W / 2 - 0.3, 1.6)), V((L / 2 + 0.2, -W / 2 + 0.6, 1.6)), 1.3, rng)
    rect_grass(dress, L + 1.0, W + 1.4, rng, band=0.0, keep_clear=[(L / 2 + 2, 0, 1.6)])
    face_front([m, dress], '+x')
    return m, dress


def shed_b(rng):
    """Small laft storehouse raised on stacked stones (to keep out damp and vermin),
    rounded-shingle roof, door on a loose step stone."""
    m, dress = MB(), MB()
    L, W, z0 = 3.4, 3.0, 0.75
    for c in rect(L - 0.1, W - 0.1):   # stone pillars: a boulder base and a flat cap slab
        rock(m, c + V((0, 0, 0.2)), (0.8, 0.75, 0.5), rng, sub=3)
        rock(m, c + V((0, 0, 0.58)), (0.7, 0.62, 0.24), rng, sub=3, squash=0.35, tilt=0.03)
    He = z0 + 1.9
    rf = GableRoof(L, W, He, He + W / 2 * math.tan(math.radians(40)), oe=0.4, og=0.35, sag=0.04, rng=rng)
    dw, dh = 0.75, 1.45
    log_walls(m, rf, L, W, z0, rng, d=0.2, crown=0.25, openings=[('+x', 0.0, dw + 0.24, z0, z0 + dh + 0.2)])
    r = 0.2 * 0.6
    door(m, wall_frame(V((L / 2 + r * 0.9, 0.0, z0)), V((1, 0, 0))), dw, dh, rng, depth=0.3)
    roof_frame(m, rf, rng, purlins=2)
    shingle_roof(m, rf, rng, shape='round', w=0.13, expo=0.16, length=0.45)
    bargeboards(m, rf, rng, carve='horn', top_lift=0.15, w=0.24, ext=0.4)
    rock(m, V((L / 2 + 0.75, 0.0, 0.12)), (0.75, 0.9, 0.35), rng, sub=3)
    rock(m, V((L / 2 + 1.3, 0.1, 0.05)), (0.6, 0.7, 0.2), rng)
    barrel(m, V((L / 2 + 0.6, 1.4, 0)), rng, r=0.26, h=0.7)
    rect_grass(dress, L, W, rng, band=0.0, keep_clear=[(L / 2 + 1, 0, 0.9)])
    face_front([m, dress], '+x')
    return m, dress


def _stilt_platform(m, rng, PL, PW, zf, posts_x, posts_y):
    """Post-and-beam platform: posts from z=0, cross braces, beams, deck planks."""
    for x in posts_x:
        for y in posts_y:
            log(m, V((x, y, 0.0)), V((x + (rng.random() - 0.5) * 0.06, y + (rng.random() - 0.5) * 0.06, zf - 0.12)),
                0.13 + rng.random() * 0.02, WOOD, rng, n=10, taper=0.1, up=V((1, 0, 0)), wobble=0.03)
    for y in posts_y:   # braces in X
        for x0, x1 in zip(posts_x, posts_x[1:]):
            log(m, V((x0, y, 0.35)), V((x1, y, zf - 0.45)), 0.055, WOOD, rng, n=7, up=V((0, 0, 1)))
    for x in (posts_x[0], posts_x[-1]):
        for y0, y1 in zip(posts_y, posts_y[1:]):
            log(m, V((x, y0, zf - 0.45)), V((x, y1, 0.35)), 0.055, WOOD, rng, n=7)
    for y in posts_y:   # beams
        log(m, V((posts_x[0] - 0.35, y, zf - 0.12)), V((posts_x[-1] + 0.35, y, zf - 0.12)), 0.13, WOOD, rng, flat=0.2, n=10)
    x = -PL / 2
    while x < PL / 2:   # joists
        log(m, V((x, -PW / 2 - 0.05, zf + 0.04)), V((x, PW / 2 + 0.05, zf + 0.04)), 0.07, WOOD, rng, flat=0.2, n=8)
        x += 0.7
    y = -PW / 2 + 0.12
    while y < PW / 2:   # deck planks
        w = 0.18 + rng.random() * 0.1
        board(m, V((-PL / 2 - 0.1 - rng.random() * 0.08, y + w / 2, zf + 0.135)), V((PL / 2 + 0.1 + rng.random() * 0.08, y + w / 2, zf + 0.135)),
              w - 0.012, 0.04, GREY, rng=rng, chamfer=0.006, wobble=0.004)
        y += w


def stilt_a(rng):
    """Fishing hut on posts over the shore: braced log posts, plank deck with a front
    porch and rail, small tarred board house with a turf roof, ladder down, fish rack."""
    m, dress = MB(), MB()
    zf = 3.0
    PL, PW = 8.0, 6.2
    _stilt_platform(m, rng, PL, PW, zf, [-3.6, -1.2, 1.2, 3.6], [-2.8, 0.0, 2.8])
    zd = zf + 0.155
    L, W = 5.2, 4.4
    xc = -PL / 2 + L / 2 + 0.2
    He = zd + 2.2
    rf = GableRoof(L, W, He, He + W / 2 * math.tan(math.radians(32)), oe=0.45, og=0.45, sag=0.07, rng=rng, x0=xc)
    deck = 0.045
    dw, dh = 0.85, 1.75
    x0, x1 = xc - L / 2, xc + L / 2
    board_wall(m, (x0, -W / 2), (x1, -W / 2), zd, lambda s_: He - 0.04, rng, (0, -1),
               openings=[(L / 2 + 0.8, 0.55, zd + 0.95, zd + 1.4)])
    board_wall(m, (x1, W / 2), (x0, W / 2), zd, lambda s_: He - 0.04, rng, (0, 1))
    board_wall(m, (x1, -W / 2), (x1, W / 2), zd, lambda s_: rf.z(x1, -W / 2 + s_) - deck - 0.03, rng, (1, 0),
               openings=[(W / 2 - 0.4, dw + 0.28, zd, zd + dh + 0.2), (W / 2 + 1.1, 0.5, zd + 0.95, zd + 1.4)], plate=False)
    board_wall(m, (x0, W / 2), (x0, -W / 2), zd, lambda s_: rf.z(x0, W / 2 - s_) - deck - 0.03, rng, (-1, 0), plate=False)
    for c in rect(L, W, xc):
        corner_post(m, c, zd, He, rng, w=0.17)
    door(m, wall_frame(V((x1 + 0.07, -0.4, zd)), V((1, 0, 0))), dw, dh, rng, depth=0.3)
    window(m, wall_frame(V((x1 + 0.07, 1.1, zd + 0.95)), V((1, 0, 0))), 0.45, 0.4, rng)
    window(m, wall_frame(V((xc + 0.8, -W / 2 - 0.07, zd + 0.95)), V((0, -1, 0))), 0.5, 0.4, rng)
    roof_frame(m, rf, rng, deck_t=deck)
    turf_roof(m, rf, rng, thick=0.2, grass_density=90, grass_h=0.2)
    bargeboards(m, rf, rng, carve='scroll', top_lift=0.28)
    # rail around the porch (front part of the deck)
    rx0 = x1 + 0.1
    for (a, b) in [((rx0, -PW / 2 + 0.1), (PL / 2 - 0.05, -PW / 2 + 0.1)), ((PL / 2 - 0.05, -PW / 2 + 0.1), (PL / 2 - 0.05, 0.9)),
                   ((PL / 2 - 0.05, 2.1), (PL / 2 - 0.05, PW / 2 - 0.1)), ((PL / 2 - 0.05, PW / 2 - 0.1), (rx0, PW / 2 - 0.1))]:
        a3, b3 = V((a[0], a[1], zd)), V((b[0], b[1], zd))
        d = (b3 - a3)
        nposts = max(1, int(d.length / 1.2))
        for k in range(nposts + 1):
            p = a3.lerp(b3, k / nposts)
            log(m, p, p + V((0, 0, 1.0)), 0.045, WOOD, rng, n=7)
        log(m, a3 + V((0, 0, 1.0)), b3 + V((0, 0, 1.0)), 0.04, WOOD, rng, n=7)
        log(m, a3 + V((0, 0, 0.5)), b3 + V((0, 0, 0.5)), 0.03, WOOD, rng, n=6)
    ladder(m, V((PL / 2 + 1.1, 1.5, 0.0)), V((PL / 2 + 0.05, 1.5, zd + 0.9)), rng, w=0.55)
    barrel(m, V((PL / 2 - 0.6, -PW / 2 + 0.7, zd)), rng, r=0.27, h=0.72)
    net_drape(m, V((rx0 + 0.6, -PW / 2 + 0.12, zd + 1.0)), V((PL / 2 - 0.3, -PW / 2 + 0.12, zd + 1.0)), 0.9, rng)
    fish_rack(m, frame_from(V((x0 + 0.3, PW / 2 - 0.5, zd)), V((1, 0, 0)), V((0, 1, 0))), 2.2, rng, h=1.7)
    face_front([m, dress], '+x')
    return m, dress


def stilt_b(rng):
    """Laft-log cabin on a braced post platform (for steep ground / the water's edge),
    rounded-shingle roof, gallery at the front with steps down."""
    m, dress = MB(), MB()
    zf = 2.2
    PL, PW = 7.0, 5.6
    _stilt_platform(m, rng, PL, PW, zf, [-3.1, 0.0, 3.1], [-2.5, 0.0, 2.5])
    zd = zf + 0.155
    L, W = 4.8, 4.2
    xc = -PL / 2 + L / 2 + 0.3
    He = zd + 2.15
    rf = GableRoof(L, W, He, He + W / 2 * math.tan(math.radians(40)), oe=0.4, og=0.4, sag=0.05, rng=rng, x0=xc)
    dw, dh = 0.85, 1.7
    log_walls(m, rf, L, W, zd, rng, d=0.22, crown=0.28, x0=xc,
              openings=[('+x', 0.3, dw + 0.26, zd, zd + dh + 0.2), ('+x', -1.3, 0.5, zd + 1.0, zd + 1.45),
                        ('-y', 0.0, 0.55, zd + 1.0, zd + 1.45)])
    r = 0.22 * 0.6
    door(m, wall_frame(V((xc + L / 2 + r * 0.9, 0.3, zd)), V((1, 0, 0))), dw, dh, rng, depth=0.32)
    window(m, wall_frame(V((xc + L / 2 + r * 0.9, -1.3, zd + 1.0)), V((1, 0, 0))), 0.45, 0.4, rng)
    window(m, wall_frame(V((xc, -W / 2 - r * 0.9, zd + 1.0)), V((0, -1, 0))), 0.5, 0.4, rng)
    roof_frame(m, rf, rng)
    shingle_roof(m, rf, rng, shape='round', w=0.14, expo=0.17)
    bargeboards(m, rf, rng, carve='scroll', top_lift=0.16, w=0.26, ext=0.5)
    # stair down from the gallery
    sx0 = PL / 2
    for k in range(9):
        t = k / 8
        z = zd - t * zd
        x = sx0 + t * 2.6
        board(m, V((x, 1.3, z)), V((x, 2.3, z)), 0.28, 0.05, GREY, up=V((0, 0, 1)), rng=rng)
    for y in (1.25, 2.35):
        log(m, V((sx0, y, zd + 0.05)), V((sx0 + 2.75, y, -0.05)), 0.06, WOOD, rng, n=8)
        log(m, V((sx0, y, zd + 0.95)), V((sx0 + 2.75, y, 0.9)), 0.035, WOOD, rng, n=7)
        log(m, V((sx0 + 2.75, y, -0.05)), V((sx0 + 2.75, y, 0.95)), 0.04, WOOD, rng, n=7)
    for (a, b) in [((xc + L / 2 + 0.3, -PW / 2 + 0.1), (PL / 2 - 0.05, -PW / 2 + 0.1)), ((PL / 2 - 0.05, -PW / 2 + 0.1), (PL / 2 - 0.05, 1.2))]:
        a3, b3 = V((a[0], a[1], zd)), V((b[0], b[1], zd))
        nposts = max(1, int((b3 - a3).length / 1.1))
        for k in range(nposts + 1):
            p = a3.lerp(b3, k / nposts)
            log(m, p, p + V((0, 0, 0.95)), 0.045, WOOD, rng, n=7)
        log(m, a3 + V((0, 0, 0.95)), b3 + V((0, 0, 0.95)), 0.04, WOOD, rng, n=7)
    woodpile(m, frame_from(V((xc - L / 2 + 0.2, W / 2 + 0.5, zd)), V((1, 0, 0)), V((0, 1, 0))), 2.6, 1.0, rng, depth_rows=1)
    face_front([m, dress], '+x')
    return m, dress


def square_a(rng):
    """Village green / market place: irregular flagstones around a drystone well with a
    windlass, carved market posts with a small shingled stall, log benches, a trough."""
    m, dress = MB(), MB()
    def avoid(p):
        return p.length < 1.15
    flagstones(m, V((0, 0, 0)), 7.5, rng, size=0.75, avoid=avoid)
    # well: courses of stones in a ring
    for c in range(4):
        n = 14
        for k in range(n):
            a = 2 * math.pi * (k + 0.5 * (c % 2)) / n
            p = V((math.cos(a) * 0.95, math.sin(a) * 0.95, 0.12 + c * 0.2))
            rock(m, p, (0.48, 0.38, 0.24), rng, yaw=a + math.pi / 2)
    for k in range(10):
        a = 2 * math.pi * k / 10
        rock(m, V((math.cos(a) * 0.95, math.sin(a) * 0.95, 0.88)), (0.5, 0.42, 0.14), rng, yaw=a + math.pi / 2, squash=0.4)
    # dark water: a disc of planks? no - a lid of boards half open plus the shaft interior in shadow
    for sy in (-1, 1):
        log(m, V((0, sy * 1.0, 0.8)), V((0.0, sy * 1.0, 2.4)), 0.08, WOOD, rng, n=9)
    log(m, V((0, -1.25, 2.2)), V((0, 1.25, 2.2)), 0.09, WOOD, rng, n=10)
    log(m, V((0, -1.4, 2.2)), V((0.35, -1.4, 2.05)), 0.03, WOOD, rng, n=6)
    log(m, V((0.0, 0.0, 1.0)), V((0.0, 0.0, 2.15)), 0.01, WOOD, rng, n=5, taper=0)   # rope
    barrel(m, V((0, 0, 0.7)), rng, r=0.16, h=0.3, staves=12)
    for i in range(4):   # half lid
        y = -0.6 + i * 0.3
        board(m, V((0.05, y, 0.97)), V((0.85, y, 0.97)), 0.28, 0.04, GREY, rng=rng)
    # market stall: four carved posts, shingle roof, plank counter
    sx, sy0 = 4.2, -3.0
    for dx in (-1.2, 1.2):
        for dy in (-0.8, 0.8):
            carved_post(m, V((sx + dx, sy0 + dy, 0)), V((sx + dx, sy0 + dy, 2.3 - 0.25 * (dy > 0))), 0.1, rng, rings=4)
    sr = GableRoof(2.6, 1.8, 2.05, 2.55, oe=0.3, og=0.25, sag=0.03, rng=rng, x0=0)
    st = len(m.V)
    roof_frame(m, sr, rng, purlins=1)
    shingle_roof(m, sr, rng, shape='point', w=0.13, expo=0.16, length=0.45)
    m.transform(Matrix.Translation((sx, sy0, 0)), st)
    y = sy0 - 0.75
    for k in range(3):
        board(m, V((sx - 1.3, y + k * 0.24, 0.95)), V((sx + 1.3, y + k * 0.24, 0.95)), 0.23, 0.05, GREY, rng=rng)
    barrel(m, V((sx + 1.7, sy0 + 0.4, 0)), rng, r=0.26, h=0.7)
    barrel(m, V((sx + 1.6, sy0 - 0.4, 0)), rng, r=0.24, h=0.66)
    # benches: split logs on stones
    for (cx, cy, ang) in ((-3.5, 2.5, 0.4), (-1.0, 4.2, -0.2), (2.4, 3.6, 0.1)):
        d = V((math.cos(ang), math.sin(ang), 0))
        c = V((cx, cy, 0))
        for s in (-1, 1):
            rock(m, c + d * s * 0.8 + V((0, 0, 0.15)), (0.4, 0.35, 0.35), rng)
        pts = [c - d * 1.15 + V((0, 0, 0.42)), c + d * 1.15 + V((0, 0, 0.42))]
        half = [(math.cos(t) * 0.16, math.sin(t) * 0.16 - 0.02) for t in [math.pi * k / 7 - math.pi for k in range(8)]]
        sweep(m, pts, [(a, -b) for (a, b) in half][::-1], WOOD, rng=rng)
    # trough: hollowed log
    log(m, V((-4.5, -2.5, 0.25)), V((-2.6, -2.9, 0.25)), 0.26, WOOD, rng, n=12, taper=0.05)
    # tall carved post (meeting post) with a cross arm
    carved_post(m, V((-1.8, -4.8, 0)), V((-1.8, -4.8, 4.2)), 0.16, rng, rings=7)
    log(m, V((-2.5, -4.8, 3.4)), V((-1.1, -4.8, 3.4)), 0.06, WOOD, rng, n=8)
    face_front([m, dress], '+x')
    return m, dress


def porch(m, rng, x, y_wall, depth=2.0, width=2.6, eave=2.1, ridge=3.2, shingles=True):
    """Small gabled entrance porch on posts projecting from a long wall at y_wall (+y side)."""
    sub = MB()
    # built with its ridge along local X (= outward), then turned to point along +y
    rf = GableRoof(depth, width, eave, ridge, oe=0.3, og=0.35, og0=0.0, sag=0.03, rng=rng, x0=depth / 2)
    roof_frame(sub, rf, rng, purlins=1)
    if shingles:
        shingle_roof(sub, rf, rng, shape='point', w=0.13, expo=0.16, length=0.45)
    else:
        turf_roof(sub, rf, rng, thick=0.18, grass_density=80, end_taper=(False, True))
    bargeboards(sub, rf, rng, carve='scroll', ends=(False, True), top_lift=0.16 if shingles else 0.28, w=0.26, ext=0.55)
    for sy in (-1, 1):
        carved_post(sub, V((depth - 0.15, sy * (width / 2 - 0.12), 0)), V((depth - 0.15, sy * (width / 2 - 0.12), eave - 0.1)), 0.11, rng, rings=4)
        rock(sub, V((depth - 0.15, sy * (width / 2 - 0.12), 0.02)), (0.45, 0.42, 0.18), rng)
        log(sub, V((-0.05, sy * (width / 2 - 0.12), eave - 0.06)), V((depth + 0.1, sy * (width / 2 - 0.12), eave - 0.06)), 0.1, WOOD, rng,
            flat=0.2, n=9)
    log(sub, V((depth - 0.15, -width / 2 - 0.1, eave - 0.06)), V((depth - 0.15, width / 2 + 0.1, eave - 0.06)), 0.1, WOOD, rng, flat=0.2, n=9)
    # bench seats along the porch sides
    for sy in (-1, 1):
        board(sub, V((0.2, sy * (width / 2 - 0.3), 0.45)), V((depth - 0.35, sy * (width / 2 - 0.3), 0.45)), 0.3, 0.05, GREY, rng=rng)
        for xx in (0.35, depth - 0.5):
            board(sub, V((xx, sy * (width / 2 - 0.3), 0.0)), V((xx, sy * (width / 2 - 0.3), 0.43)), 0.25, 0.06, WOOD,
                  up=V((1, 0, 0)), rng=rng)
    sub.transform(Matrix.Translation((x, y_wall, 0)) @ Matrix.Rotation(math.pi / 2, 4, 'Z'))
    m.extend(sub)


def hallhouse_b(rng):
    """Long laft-log hall: hewn log walls clamped by paired upright boards, turf roof with
    two smoke louvres, gabled entrance porch on carved posts in the long wall."""
    m, dress = MB(), MB()
    L, W, z0, He = 25.0, 8.4, 0.5, 3.0
    rf = GableRoof(L, W, He, He + W / 2 * math.tan(math.radians(30)), oe=0.6, og=0.55, sag=0.22, rng=rng)
    stone_course(m, rect(L, W), rng, h=0.26, depth=0.5, z0=-0.1, courses=2, size=0.5, closed=True)
    dw, dh = 1.1, 1.95
    door_x = 1.5
    ops = [('+y', door_x, dw + 0.28, z0, z0 + dh + 0.2)]
    for x in (-8.5, -4.5, 6.0, 9.5):
        ops.append(('+y', x, 0.6, z0 + 1.3, z0 + 1.8))
    ops.append(('+x', 0.0, 0.95, z0, z0 + 1.95))
    log_walls(m, rf, L, W, z0, rng, d=0.25, crown=0.35, openings=ops)
    r = 0.25 * 0.6
    # paired clamp boards (log walls this long are joined between them)
    for sy in (-1, 1):
        for x in (-6.5, 4.0):
            for f in (-1, 1):
                board(m, V((x + f * 0.12, sy * (W / 2 + r + 0.03), z0 + 0.1)), V((x + f * 0.12, sy * (W / 2 + r + 0.03), He - 0.1)),
                      0.12, 0.06, WOOD, up=V((0, sy, 0)), rng=rng)
    door(m, wall_frame(V((door_x, W / 2 + r * 0.9, z0)), V((0, 1, 0))), dw, dh, rng, depth=0.4)
    door(m, wall_frame(V((L / 2 + r * 0.9, 0.0, z0)), V((1, 0, 0))), 0.85, 1.85, rng, depth=0.35)
    for x in (-8.5, -4.5, 6.0, 9.5):
        window(m, wall_frame(V((x, W / 2 + r * 0.9, z0 + 1.3)), V((0, 1, 0))), 0.55, 0.45, rng, shutter=(x > 0))
    roof_frame(m, rf, rng, purlins=4)
    turf_roof(m, rf, rng, thick=0.25, grass_density=40, grass_h=0.22)
    bargeboards(m, rf, rng, carve='scroll', top_lift=0.32, w=0.34, ext=0.8)
    smoke_louvre(m, rf, -5.0, rng)
    smoke_louvre(m, rf, 5.5, rng)
    porch(m, rng, door_x, W / 2 + r, depth=2.2, width=2.8, eave=2.2, ridge=3.4, shingles=False)
    woodpile(m, frame_from(V((-11.0, W / 2 + 1.2, 0.0)), V((1, 0, 0)), V((0, 1, 0))), 3.5, 1.4, rng, depth_rows=1)
    barrel(m, V((door_x + 2.2, W / 2 + 0.6, 0)), rng)
    barrel(m, V((door_x + 2.8, W / 2 + 0.5, 0)), rng, r=0.27, h=0.72)
    chopping_block(m, V((-7.0, W / 2 + 3.0, 0)), rng)
    rect_grass(dress, L, W, rng, band=0.0, keep_clear=[(door_x, W / 2 + 3, 1.8)])
    face_front([m, dress], '+y')
    return m, dress


def square_b(rng):
    """Small landing place by the boats: a stone-ringed hearth, a fish-drying rack, a
    hitching rail, a standing stone, stacked barrels and a rough flagged patch."""
    m, dress = MB(), MB()
    flagstones(m, V((0.5, 0.5, 0)), 5.5, rng, size=0.8, avoid=lambda p: (p - V((0, 0, p.z))).length < 1.1)
    for k in range(13):   # hearth ring
        a = 2 * math.pi * k / 13
        rock(m, V((math.cos(a) * 0.85, math.sin(a) * 0.85, 0.08)), (0.4, 0.32, 0.28), rng, yaw=a)
    for k in range(7):    # charred logs in the hearth
        a = rng.random() * 6.28
        c = V(((rng.random() - 0.5) * 0.4, (rng.random() - 0.5) * 0.4, 0.07))
        d = V((math.cos(a), math.sin(a), 0.15)) * 0.35
        log(m, c - d, c + d, 0.05, WOOD, rng, n=7)
    # tripod over the hearth with a hanging pot (barrel-shaped)
    for k in range(3):
        a = 2 * math.pi * k / 3
        log(m, V((math.cos(a) * 1.3, math.sin(a) * 1.3, 0)), V((0, 0, 2.0)), 0.04, WOOD, rng, n=7)
    log(m, V((0, 0, 1.97)), V((0, 0, 0.75)), 0.008, WOOD, rng, n=5, taper=0)
    barrel(m, V((0, 0, 0.45)), rng, r=0.2, h=0.3, staves=12)
    fish_rack(m, frame_from(V((-5.0, -3.0, 0)), V((1, 0.3, 0)), V((-0.3, 1, 0))), 5.0, rng)
    # hitching / mooring rail
    for x in (2.5, 4.5, 6.5):
        log(m, V((x, -4.5, -0.1)), V((x, -4.5, 1.1)), 0.09, WOOD, rng, n=9)
    log(m, V((2.2, -4.5, 1.0)), V((6.8, -4.5, 1.0)), 0.065, WOOD, rng, n=9)
    rock(m, V((-3.8, 3.5, 0.9)), (0.8, 0.55, 2.0), rng, sub=3, tilt=0.12, rough=0.18)
    for i, (x, y) in enumerate(((4.0, 2.5), (4.6, 2.9), (4.25, 2.7))):
        barrel(m, V((x, y, 0.0 if i < 2 else 0.74)), rng, r=0.27, h=0.72)
    boat(m, frame_from(V((3.0, 5.6, 0)), V((1, -0.2, 0)), V((0.2, 1, 0))), rng)
    for (cx, cy, ang) in ((-2.0, -1.8, 0.6), (1.8, -1.6, -0.5)):
        d = V((math.cos(ang), math.sin(ang), 0))
        c = V((cx, cy, 0))
        log(m, c - d * 1.0 + V((0, 0, 0.2)), c + d * 1.0 + V((0, 0, 0.2)), 0.2, WOOD, rng, n=12, taper=0.06)
    face_front([m, dress], '+x')
    return m, dress


RECIPES = {
    'cottage_a': cottage_a,
    'cottage_b': cottage_b,
    'turf_a': turf_a,
    'turf_b': turf_b,
    'longhouse_a': longhouse_a,
    'longhouse_b': longhouse_b,
    'hallhouse_a': hallhouse_a,
    'hallhouse_b': hallhouse_b,
    'tall_a': tall_a,
    'tall_b': tall_b,
    'shed_a': shed_a,
    'shed_b': shed_b,
    'stilt_a': stilt_a,
    'stilt_b': stilt_b,
    'square_a': square_a,
    'square_b': square_b,
}
