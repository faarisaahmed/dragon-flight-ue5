#!/usr/bin/env python3
"""coast.py - a smooth "metres from the shoreline" field for the sea's surf and foam.

The sea used to judge the shore by water depth read from the 4033 heightmap texture (11 m a
texel). At a sea cliff the depth jumps by 100 m inside one texel, so the foam traced the texel
corners (a zigzag ribbon) and the breaker lines bunched into dashes. A signed distance to the
coast changes by exactly a metre a metre everywhere, so it interpolates cleanly at any
resolution and the surf can be laid out by it: breakers rolling in over a surf zone, white
water at the foot of the cliffs, foam trails drifting off.

Writes Data/World/Coast_8192.png (8-bit grey, pixel centres like Masks_4096):
    v = clip((d + 24) * 2, 0, 255)      d = metres from the shoreline, + at sea, - on land
so -24 m .. +103.5 m at half-metre steps; 255 = open sea.

Built from Heightmap_8129.r16 (the landscape's own vertices), with the last texel either side
of the shore taken from the height itself (h / |grad h|), which is where the drawn landscape
actually crosses sea level. Peak memory ~1.6 GB.

    python3 Tools/WorldBake/coast.py
"""
import json
import os
import sys

import numpy as np
from PIL import Image
from scipy import ndimage

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
W = os.path.join(ROOT, "Data", "World")
M = 8192
LO, SCALE = -24.0, 2.0


def main():
    js = json.load(open(os.path.join(W, "World.json")))
    S = float(js["terrainSize"])
    n = int(js["heightmap"]["res"])
    zs = js["heightmap"]["zScalePercent"]
    h = np.fromfile(os.path.join(W, f"Heightmap_{n}.r16"), dtype="<u2").reshape(n, n).astype(np.float32)
    h = (h - 32768.0) * (zs / 100.0 / 128.0)                       # metres
    step = S / (n - 1)
    land = h > 0.0
    d_sea = ndimage.distance_transform_edt(~land).astype(np.float32)
    d_land = ndimage.distance_transform_edt(land).astype(np.float32)
    sd = (d_sea - d_land) * step                                   # texel centres -> +-0.5 texel off
    del d_sea, d_land
    # Next to the shore: the linear crossing of the drawn surface, -h / |grad h|.
    gy, gx = np.gradient(h, step)
    g = np.sqrt(gx * gx + gy * gy)
    del gx, gy
    lin = -h / np.maximum(g, 0.02)
    del g
    lin = np.clip(lin, -2.0 * step, 2.0 * step)
    w = np.clip(2.0 - np.abs(sd) / step, 0.0, 1.0)                 # 1 within a texel, 0 beyond two
    sd = sd * (1.0 - w) + lin * w
    del lin, w, h, land
    sd = ndimage.gaussian_filter(sd, 0.6)
    # Resample onto the texture's pixel centres.
    out = np.empty((M, M), np.uint8)
    xs = ((np.arange(M) + 0.5) * S / M) / step
    for r0 in range(0, M, 1024):
        r1 = min(M, r0 + 1024)
        ys = ((np.arange(r0, r1) + 0.5) * S / M) / step
        gy, gx = np.meshgrid(ys, xs, indexing="ij")
        v = ndimage.map_coordinates(sd, [gy, gx], order=1, mode="nearest")
        out[r0:r1] = np.clip(np.round((v - LO) * SCALE), 0, 255).astype(np.uint8)
    Image.fromarray(out, "L").save(os.path.join(W, f"Coast_{M}.png"), optimize=True)
    js["coastNote"] = "Tools/WorldBake/coast.py: Coast_8192.png = clip((metres from shore, + at sea) + 24) * 2, pixel centres."
    json.dump(js, open(os.path.join(W, "World.json"), "w"), separators=(",", ":"))
    print("wrote", f"Coast_{M}.png", "sea share", float((out == 255).mean()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
