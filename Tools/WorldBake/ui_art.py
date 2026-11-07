#!/usr/bin/env python3
"""ui_art.py - the HUD's few ornaments, drawn procedurally (white on transparent; tinted in game).

    Content/UI/Knot.png   a triquetra: the trefoil knot drawn with true over/under weave
    Content/UI/Rule.png   a hairline divider, tapering at both ends, with a small knot at its centre
    Content/UI/Corner.png a frame corner: two hairlines meeting at a small interlaced loop
    Content/UI/Ring.png   a thin ring with a band of interlace, behind the compass/map rose

The weave: the trefoil is a 3D curve (z = -sin 3t); segments are painted far-to-near, each with a
dark gap around it, so every crossing goes over-under-over as in carved work.

    python3 Tools/WorldBake/ui_art.py
"""
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "Content", "UI")
SS = 4   # supersampling


def woven(draw, pts, width, gap):
    """pts: [(x, y, z)] along a closed curve; paints far-to-near with an erasing gap."""
    segs = []
    for i in range(len(pts)):
        a, b = pts[i], pts[(i + 1) % len(pts)]
        segs.append(((a[2] + b[2]) * 0.5, a, b))
    segs.sort(key=lambda s: s[0])
    for _, a, b in segs:
        draw.line([(a[0], a[1]), (b[0], b[1])], fill=(0, 0, 0, 0), width=int(width + 2 * gap))
        r = (width + 2 * gap) / 2
        draw.ellipse([a[0] - r, a[1] - r, a[0] + r, a[1] + r], fill=(0, 0, 0, 0))
    # second pass: the strands themselves, near-to-far order replayed so near ones end on top
    for _, a, b in segs:
        draw.line([(a[0], a[1]), (b[0], b[1])], fill=(255, 255, 255, 255), width=int(width))
        r = width / 2
        draw.ellipse([a[0] - r, a[1] - r, a[0] + r, a[1] + r], fill=(255, 255, 255, 255))


def woven_proper(img, pts, width, gap):
    """Painter's algorithm per segment: each segment erases a gap then draws itself, far first."""
    d = ImageDraw.Draw(img)
    segs = []
    n = len(pts)
    for i in range(n):
        a, b = pts[i], pts[(i + 1) % n]
        segs.append(((a[2] + b[2]) * 0.5, a, b))
    segs.sort(key=lambda s: s[0])
    for _, a, b in segs:
        w = width + 2 * gap
        d.line([(a[0], a[1]), (b[0], b[1])], fill=(0, 0, 0, 0), width=int(w))
        d.line([(a[0], a[1]), (b[0], b[1])], fill=(255, 255, 255, 255), width=int(width))
        r = width / 2
        d.ellipse([a[0] - r, a[1] - r, a[0] + r, a[1] + r], fill=(255, 255, 255, 255))



def zweave(N, curves, width, gap, edge=None, body=0.28):
    """Depth-buffered interlace. curves: list of [(x, y, z)] (closed). A strand cuts a gap only in
    strands that pass under it (z lower by > 0.35), so there are no nicks along a strand.
    Ribbon look: two edge lines (edge px) with a faint body."""
    ink = np.zeros((N, N), np.float32)
    zb = np.full((N, N), -9.0, np.float32)
    pts = [p for c in curves for p in c]
    pts.sort(key=lambda p: p[2])
    R = width / 2 + gap
    r = width / 2
    yy, xx = np.mgrid[-int(R) - 1:int(R) + 2, -int(R) - 1:int(R) + 2]
    dist = np.hypot(xx, yy).astype(np.float32)
    e = edge if edge is not None else width * 0.22
    prof = np.where(dist <= r, np.where(dist >= r - e, 1.0, body), 0.0).astype(np.float32)
    for (x, y, z) in pts:
        cx, cy = int(round(x)), int(round(y))
        y0, x0 = cy - yy.shape[0] // 2, cx - xx.shape[1] // 2
        sl = (slice(max(0, y0), min(N, y0 + yy.shape[0])), slice(max(0, x0), min(N, x0 + xx.shape[1])))
        ky = slice(sl[0].start - y0, sl[0].stop - y0); kx = slice(sl[1].start - x0, sl[1].stop - x0)
        d, pr = dist[ky, kx], prof[ky, kx]
        Z, I = zb[sl], ink[sl]
        under = Z < z - 0.35
        gapm = (d <= R) & under
        I[gapm] = 0.0
        Z[gapm] = z - 0.2
        core = d <= r
        take = core & (Z <= z + 0.35)
        I[take] = np.maximum(np.where(Z[take] < z - 0.35, 0.0, I[take]), pr[take])
        Z[take] = np.maximum(Z[take], z)
    return ink


def to_img(ink, alpha=1.0):
    a = (np.clip(ink, 0, 1) * 255 * alpha).astype(np.uint8)
    img = Image.new("RGBA", ink.shape[::-1], (255, 255, 255, 0))
    img.putalpha(Image.fromarray(a, "L"))
    return img


def trefoil(cx, cy, s, n=1400, rot=0.0):
    pts = []
    for i in range(n):
        t = 2 * math.pi * i / n
        x = math.sin(t) + 2 * math.sin(2 * t)
        y = math.cos(t) - 2 * math.cos(2 * t)
        z = -math.sin(3 * t)
        c, sn = math.cos(rot), math.sin(rot)
        x, y = x * c - y * sn, x * sn + y * c
        pts.append((cx + x * s, cy + y * s, z))
    return pts


def save(img, name, size):
    img = img.resize(size, Image.LANCZOS)
    img.save(os.path.join(OUT, name))
    print("wrote", name, size)


def knot():
    N = 256 * SS
    ink = zweave(N, [trefoil(N / 2, N / 2, N * 0.135, n=5000, rot=math.pi)], width=N * 0.07, gap=N * 0.024, edge=N * 0.014)
    img = to_img(ink)
    d = ImageDraw.Draw(img)
    r = N * 0.47
    d.ellipse([N / 2 - r, N / 2 - r, N / 2 + r, N / 2 + r], outline=(255, 255, 255, 150), width=int(N * 0.010))
    save(img, "Knot.png", (256, 256))


def rule():
    Wd, H = 1024 * SS, 64 * SS
    img = Image.new("RGBA", (Wd, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cy = H / 2
    gapx = H * 0.75
    for x0, x1 in ((0, Wd / 2 - gapx), (Wd / 2 + gapx, Wd)):
        steps = 200
        for i in range(steps):
            u0, u1 = x0 + (x1 - x0) * i / steps, x0 + (x1 - x0) * (i + 1) / steps
            m = (u0 + u1) / 2
            t = 1 - abs(m - Wd / 2) / (Wd / 2)          # 1 at the centre, 0 at the ends
            a = int(255 * min(1, t * 1.6) ** 1.2)
            d.line([(u0, cy), (u1, cy)], fill=(255, 255, 255, a), width=int(SS * 1.2))
    # small diamonds where the line meets the knot
    for sx in (-1, 1):
        x = Wd / 2 + sx * (gapx - H * 0.12)
        r = H * 0.06
        d.polygon([(x - r, cy), (x, cy - r), (x + r, cy), (x, cy + r)], fill=(255, 255, 255, 230))
    kn = to_img(zweave(H, [trefoil(H / 2, H / 2, H * 0.085, n=3000, rot=math.pi)], width=H * 0.06, gap=H * 0.02, edge=H * 0.016, body=0.35))
    img.alpha_composite(kn, (int(Wd / 2 - H / 2), 0))
    save(img, "Rule.png", (1024, 64))


def corner():
    N = 128 * SS
    img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    w = int(SS * 1.6)
    o = N * 0.18
    # two hairlines running off along the frame edges, fading out
    for i in range(160):
        u0, u1 = o + (N - o) * i / 160, o + (N - o) * (i + 1) / 160
        a = int(255 * (1 - i / 160) ** 1.5)
        d.line([(u0, o), (u1, o)], fill=(255, 255, 255, a), width=w)
        d.line([(o, u0), (o, u1)], fill=(255, 255, 255, a), width=w)
    # an interlaced loop pair at the corner: two rings, linked
    r = N * 0.11
    for (cx, cy, z0) in ((o + r * 0.55, o + r * 0.55, 0.0),):
        pass
    pts1 = [(o + r * 0.2 + r * math.cos(t), o + r * 1.0 + r * math.sin(t), math.sin(t)) for t in [2 * math.pi * i / 600 for i in range(600)]]
    pts2 = [(o + r * 1.0 + r * math.cos(t), o + r * 0.2 + r * math.sin(t), -math.sin(t + 0.6)) for t in [2 * math.pi * i / 600 for i in range(600)]]
    # weave the two rings together: paint all segments of both by depth
    allp = []
    for pts in (pts1, pts2):
        for i in range(len(pts)):
            a, b = pts[i], pts[(i + 1) % len(pts)]
            allp.append(((a[2] + b[2]) * 0.5, a, b))
    allp.sort(key=lambda s: s[0])
    for _, a, b in allp:
        d.line([(a[0], a[1]), (b[0], b[1])], fill=(0, 0, 0, 0), width=int(N * 0.05))
        d.line([(a[0], a[1]), (b[0], b[1])], fill=(255, 255, 255, 255), width=int(N * 0.022))
    save(img, "Corner.png", (128, 128))


def ring():
    N = 512 * SS
    img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c = N / 2
    for r, a, w in ((N * 0.48, 200, 1.4), (N * 0.405, 140, 1.0)):
        d.ellipse([c - r, c - r, c + r, c + r], outline=(255, 255, 255, a), width=int(SS * w))
    # an interlace band between the two rings: two sinusoids crossing, woven
    R0, A, k = N * 0.4425, N * 0.022, 36
    pts1, pts2 = [], []
    for i in range(3600):
        t = 2 * math.pi * i / 3600
        s = math.sin(k * t)
        pts1.append((c + (R0 + A * s) * math.cos(t), c + (R0 + A * s) * math.sin(t), math.cos(k * t)))
        pts2.append((c + (R0 - A * s) * math.cos(t), c + (R0 - A * s) * math.sin(t), -math.cos(k * t)))
    allp = []
    for pts in (pts1, pts2):
        for i in range(len(pts)):
            a, b = pts[i], pts[(i + 1) % len(pts)]
            allp.append(((a[2] + b[2]) * 0.5, a, b))
    allp.sort(key=lambda s: s[0])
    for _, a, b in allp:
        d.line([(a[0], a[1]), (b[0], b[1])], fill=(0, 0, 0, 0), width=int(N * 0.010))
        d.line([(a[0], a[1]), (b[0], b[1])], fill=(255, 255, 255, 170), width=int(N * 0.0045))
    save(img, "Ring.png", (512, 512))


def main():
    os.makedirs(OUT, exist_ok=True)
    knot(); rule(); corner(); ring()
    return 0


if __name__ == "__main__":
    sys.exit(main())
