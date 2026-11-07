#!/usr/bin/env python3
"""forests.py - real forests: dense pine woods where a northern island would grow them.

The web game's forest mask (Masks_4096 R) covers under 2 % of the map in scattered spots. This
adds forest stands, organic in outline, on the sheltered temperate ground of chosen islands:

    woodland isles (World.json "biomes"): near-continuous forest over the lowland
    Hearthholm, Crowhead, Exile's Rock:   big stands in the valleys and on the lee slopes

Where trees grow: dry land 6 m above the sea up to the tree line (lower toward the cold north,
as UDragonWorldSubsystem::SnowLineAt), off cliffs and scree (Masks2 R slope), out of the
villages and trodden pads (Masks2 B), not on the volcanic or moorland isles (Masks3), and
thickest in the gullies and hollows (Flow_4096) where the soil is deep and the wind can't reach.

Both the landscape material (forest floor, canopy colour from afar) and ADragonScatter's trees
read Masks_4096 R, so they agree. The web game's original mask is kept in Masks_4096_web.png and
this script always starts from it (safe to re-run).

    python3 Tools/WorldBake/forests.py
"""
import json
import os
import shutil
import sys

import numpy as np
from PIL import Image
from scipy import ndimage

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
W = os.path.join(ROOT, "Data", "World")
STANDS = {"Hearthholm": 0.5, "Crowhead": 0.45, "Exile's Rock": 0.4}   # share of good ground under forest


def smooth(x, a, b):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def main():
    js = json.load(open(os.path.join(W, "World.json")))
    S = float(js["terrainSize"])
    M = 4096
    src = os.path.join(W, "Masks_4096.png")
    web = os.path.join(W, "Masks_4096_web.png")
    if not os.path.exists(web):
        shutil.copy(src, web)
    mk = np.asarray(Image.open(web)).astype(np.float32) / 255.0
    mk2 = np.asarray(Image.open(os.path.join(W, "Masks2_4096.png"))).astype(np.float32) / 255.0
    mk3 = np.asarray(Image.open(os.path.join(W, "Masks3_4096.png"))).astype(np.float32) / 255.0
    flow = np.asarray(Image.open(os.path.join(W, "Flow_4096.png"))).astype(np.float32)
    flow = (flow[..., 0] if flow.ndim == 3 else flow) / 255.0

    # heights on the mask's pixel centres
    n = 4033
    h = np.fromfile(os.path.join(W, f"Heightmap_{n}.r16"), dtype="<u2").reshape(n, n).astype(np.float32)
    h = (h - 32768.0) * (js["heightmap"]["zScalePercent"] / 100.0 / 128.0)
    c = ((np.arange(M) + 0.5) * S / M) / (S / (n - 1))
    gy, gx = np.meshgrid(c, c, indexing="ij")
    h = ndimage.map_coordinates(h, [gy, gx], order=1, mode="nearest").astype(np.float32)

    xs = -S / 2 + (np.arange(M) + 0.5) * S / M
    X, Y = np.meshgrid(xs, xs)            # rows = UE Y (north is -Y), cols = UE X
    wob = 1800.0 * np.sin(X / 5200.0 + 0.7) + 900.0 * np.sin(X / 2100.0 + 2.1)
    cold = smooth((-Y - 9000.0 + wob) / 9000.0, 0.0, 1.0)
    snow = (480.0 + (40.0 - 480.0) * cold)
    treeline = np.minimum(330.0, snow * 0.62)

    slope = mk2[..., 0]
    pad = mk2[..., 2]
    volc, moor = mk3[..., 1], mk3[..., 2]
    ok = (smooth(h, 6.0, 18.0) * (1.0 - smooth(h, treeline * 0.8, treeline)) * (1.0 - smooth(slope, 0.42, 0.7))
          * (1.0 - smooth(pad, 0.05, 0.3)) * (1.0 - volc) * (1.0 - 0.85 * moor) * (1.0 - smooth(cold, 0.25, 0.5))
          * (1.0 - smooth(mk[..., 2], 0.1, 0.4)) * (1.0 - smooth(mk[..., 3], 0.1, 0.4)))

    # stand outlines: two octaves of smooth noise, so woods have bays, rides and outliers
    rng = np.random.default_rng(11)
    noise = np.zeros((M, M), np.float32)
    for wl, amp in ((1400.0, 0.65), (420.0, 0.35)):
        k = int(S / wl) + 3
        g = rng.standard_normal((k, k)).astype(np.float32)
        noise += ndimage.zoom(g, M / k, order=3)[:M, :M] * amp
    noise = (noise - noise.mean()) / (noise.std() + 1e-6)
    shelter = smooth(ndimage.gaussian_filter(flow, 3.0), 0.05, 0.4)

    forest = np.zeros((M, M), np.float32)
    biomes = js.get("biomes", {})
    for isl in js["islands"]:
        name = isl["name"]
        woodland = biomes.get(name) == "woodland"
        if not woodland and name not in STANDS:
            continue
        d = np.hypot(X - isl["x"], Y - isl["z"]) / isl["r"]
        near = d < 1.6
        if woodland:
            want = smooth(noise + 0.9 + shelter * 0.6, 0.0, 0.6)
        else:
            # threshold so roughly STANDS[name] of the island's good ground is wooded
            score = noise + shelter * 1.2
            q = np.quantile(score[near & (ok > 0.5)], 1.0 - STANDS[name]) if np.any(near & (ok > 0.5)) else 9.0
            want = smooth(score, q - 0.25, q + 0.25)
        forest = np.maximum(forest, np.where(near, want * ok, 0.0))

    # soft edges: woods thin out over 30-60 m, they don't stop at a line
    forest = ndimage.gaussian_filter(forest, 1.5)
    out = mk.copy()
    out[..., 0] = np.maximum(mk[..., 0], forest)
    Image.fromarray(np.clip(out * 255.0 + 0.5, 0, 255).astype(np.uint8), "RGBA").save(src)
    land = h > 1.0
    print(f"forest > 0.5: {float((out[..., 0] > 0.5)[land].mean()) * 100:.1f}% of land (was {float((mk[..., 0] > 0.5)[land].mean()) * 100:.1f}%)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
