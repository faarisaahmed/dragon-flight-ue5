#!/usr/bin/env python3
"""map.py - the Tab map: a shaded-relief sea chart baked from the world itself.

Writes Content/UI/Map_4096.jpg (staged with the game, loaded at runtime by the HUD).
Island names, the dragon's marker and the frame are drawn live by ADragonHUD, so they
stay crisp at any zoom.

The look: a modern chart of a northern sea. Deep water is a dark ink blue that lightens over
the shelf, with faint depth contours; the land is in muted natural colour (pasture, heath,
pine, bare rock, snow) under a soft north-west hillshade; a pale line traces the coast with a
faint halo of surf. No parchment, no fake runes.

    python3 Tools/WorldBake/map.py
"""
import json
import os
import sys

import numpy as np
from PIL import Image
from scipy import ndimage

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
W = os.path.join(ROOT, "Data", "World")
OUT = os.path.join(ROOT, "Content", "UI", "Map_4096.jpg")
M = 4096


def smooth(x, a, b):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def lerp(a, b, t):
    return a + (b - a) * t[..., None]


def rgb(*c):
    return np.array(c, np.float32) / 255.0


def load_mask(name):
    a = np.asarray(Image.open(os.path.join(W, name))).astype(np.float32) / 255.0
    if a.shape[0] != M:
        a = np.stack([ndimage.zoom(a[..., i], M / a.shape[0], order=1) for i in range(a.shape[-1])], -1) if a.ndim == 3 else ndimage.zoom(a, M / a.shape[0], order=1)
    return a


def main():
    js = json.load(open(os.path.join(W, "World.json")))
    S = float(js["terrainSize"])
    n = 8129
    h = np.fromfile(os.path.join(W, f"Heightmap_{n}.r16"), dtype="<u2").reshape(n, n).astype(np.float32)
    h = (h - 32768.0) * (js["heightmap"]["zScalePercent"] / 100.0 / 128.0)       # metres
    # onto the map's pixel centres (rows = UE Y, north up = -Y at the top, like the masks)
    c = ((np.arange(M) + 0.5) * S / M) / (S / (n - 1))
    h = ndimage.map_coordinates(h, np.meshgrid(c, c, indexing="ij"), order=1).astype(np.float32)
    px = S / M                                                                      # metres a pixel (~11 m)

    mk = load_mask("Masks_4096.png")          # R forest, ...
    mk2 = load_mask("Masks2_4096.png")        # R slope/scree, B villages/pads
    mk3 = load_mask("Masks3_4096.png")        # G volcanic, B moor
    forest = mk[..., 0]
    scree = mk2[..., 0]
    volc, moor = mk3[..., 1], mk3[..., 2]
    land = h > 0.0

    # --- sea: depth tint, shelf glow, contours ------------------------------------------
    depth = np.clip(-h, 0, None)
    deep, mid, shelf = rgb(10, 22, 38), rgb(18, 48, 70), rgb(52, 104, 120)
    t1 = smooth(depth, 0, 18)
    t2 = smooth(depth, 18, 160)
    sea = lerp(lerp(shelf, mid, t1), deep, t2)
    # contour lines at 10, 25, 50, 100 m, very faint
    for d, a in ((10, 0.10), (25, 0.08), (50, 0.07), (100, 0.06)):
        line = np.exp(-((depth - d) / (0.9 + d * 0.012)) ** 2)
        sea += line[..., None] * a * rgb(180, 220, 230)

    # --- land: natural colour by height and cover ------------------------------------------
    pasture, heath, rock, snow = rgb(92, 112, 64), rgb(122, 112, 78), rgb(112, 106, 98), rgb(236, 240, 244)
    pine, volcanic, moorc, beach = rgb(36, 58, 40), rgb(58, 52, 50), rgb(118, 100, 72), rgb(196, 182, 150)
    lt = lerp(pasture, heath, smooth(h, 80, 320))
    lt = lerp(lt, moorc, moor * 0.8)
    lt = lerp(lt, rock, np.maximum(smooth(h, 300, 520), smooth(scree, 0.35, 0.8)))
    lt = lerp(lt, volcanic, volc)
    lt = lerp(lt, pine, smooth(forest, 0.25, 0.75) * 0.92)
    # snow line falls toward the cold north (as UDragonWorldSubsystem::SnowLineAt)
    xs = -S / 2 + (np.arange(M) + 0.5) * px
    X, Y = np.meshgrid(xs, xs)
    cold = smooth((-Y - 9000.0) / 9000.0, 0.0, 1.0)
    snowline = 480.0 + (40.0 - 480.0) * cold
    lt = lerp(lt, snow, smooth(h - snowline, 0, 60) * 0.95)
    lt = lerp(lt, beach, (1 - smooth(h, 0.5, 3.0)) * (1 - smooth(scree, 0.3, 0.6)) * 0.8)
    # fine canopy texture so the woods read as woods
    rng = np.random.default_rng(5)
    speck = ndimage.gaussian_filter(rng.standard_normal((M, M)).astype(np.float32), 0.7)
    lt *= (1.0 + 0.10 * speck * smooth(forest, 0.3, 0.7))[..., None]

    # --- hillshade (sun from the north-west, 40 deg) -----------------------------------------
    hs_h = ndimage.gaussian_filter(np.maximum(h, 0), 0.8)
    gy, gx = np.gradient(hs_h, px)
    alt = np.radians(40.0)
    # image x = east, image y = south; light comes from the north-west: L = (-1, -1)/sqrt2 * cos(alt), sin(alt)
    nx, ny, nz = -gx * 2.2, -gy * 2.2, np.ones_like(gx)
    nl = np.sqrt(nx * nx + ny * ny + nz * nz)
    lx = ly = -np.cos(alt) / np.sqrt(2.0)
    lz = np.sin(alt)
    shade = np.clip((nx * lx + ny * ly + nz * lz) / nl, 0, 1)
    flat = np.sin(alt)
    shade_m = 0.62 + 0.62 * (shade - flat * 0.6)
    lt *= np.clip(shade_m, 0.35, 1.25)[..., None]

    img = np.where(land[..., None], lt, sea)

    # --- coast: pale line + soft surf halo ------------------------------------------------
    dist_sea = ndimage.distance_transform_edt(~land) * px          # metres out from the coast, at sea
    coast = np.exp(-(dist_sea / 9.0) ** 2) * (~land)
    halo = np.exp(-dist_sea / 70.0) * (~land)
    img += halo[..., None] * 0.10 * rgb(170, 210, 215)
    img = lerp(img, rgb(226, 222, 204), np.clip(coast * 0.85, 0, 1))

    # lakes (inland water below the local ground but above the sea) - keep them clear blue
    # (they come out of the heightmap as low land; World.json lakes are drawn by name only)

    # --- graticule: faint lines every 2 km, with a hint of a vignette -----------------------
    g = np.zeros((M, M), np.float32)
    step = 2000.0 / px
    for k in np.arange(0, M, step):
        i = int(round(k))
        g[:, max(0, i - 1):i + 1] = 1.0
        g[max(0, i - 1):i + 1, :] = 1.0
    img = lerp(img, rgb(200, 215, 225), g * np.where(land, 0.05, 0.08))
    yy, xx = np.mgrid[0:M, 0:M] / M - 0.5
    vig = 1.0 - 0.28 * smooth(np.hypot(xx, yy), 0.32, 0.72)
    img *= vig[..., None]

    out = np.clip(img * 255.0 + 0.5, 0, 255).astype(np.uint8)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    Image.fromarray(out, "RGB").save(OUT, quality=90, optimize=True)
    print("wrote", OUT, os.path.getsize(OUT) // 1024, "KB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
