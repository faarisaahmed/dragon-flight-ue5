#!/usr/bin/env python3
"""biomes.py - give the islands their own characters.

Writes Data/World/Masks3_4096.png (same pixel convention as Masks_4096):
    R  woodland   sheltered southern isles with real conifer forest
    G  volcanic   young basalt: black sand, dark lava rock, yellow-green moss, no trees, steam
    B  moorland   heather and blanket bog, few trees

and World.json "biomes" (island -> biome) for the game and the docs. The cold north
(snow and ice) is a climate, not a biome: it comes from the position (ColdAt).

    python3 Tools/WorldBake/biomes.py
"""
import json
import os
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
W = os.path.join(ROOT, "Data", "World")

# Which islands are what. Volcanic: a tall young cone (Wyrmspire) and the ember isle;
# woodland: the sheltered southern isles; moorland: the low boggy ones.
BIOMES = {
    "Wyrmspire": "volcanic",
    "Emberwick": "volcanic",
    "Mender's Isle": "woodland",
    "Clawshore": "woodland",
    "Lullaby Isle": "woodland",
    "Vanaheim": "woodland",
    "Shifting Isle": "woodland",
    "Quietmoor": "moorland",
    "Mirefall Bog": "moorland",
    "Greywater Sound": "moorland",
    "Sheep Island": "moorland",
}
CH = {"woodland": 0, "volcanic": 1, "moorland": 2}


def main():
    js = json.load(open(os.path.join(W, "World.json")))
    S = js["terrainSize"]
    M = 4096
    xs = -S / 2 + (np.arange(M) + 0.5) * S / M
    X, Z = np.meshgrid(xs, xs)          # rows = z (UE Y), cols = x
    out = np.zeros((M, M, 3), np.float32)
    rng = np.random.default_rng(3)
    # a wobbly edge so a biome doesn't end on a circle
    wob = np.zeros((M, M), np.float32)
    for wl, amp in ((3000.0, 0.18), (900.0, 0.08)):
        k = int(S / wl) + 3
        g = rng.standard_normal((k, k)).astype(np.float32)
        from scipy import ndimage
        wob += ndimage.zoom(g, M / k, order=3)[:M, :M] * amp
    for isl in js["islands"]:
        b = BIOMES.get(isl["name"])
        if not b:
            continue
        d = np.hypot(X - isl["x"], Z - isl["z"]) / isl["r"] + wob
        w = np.clip((1.45 - d) / 0.5, 0.0, 1.0)
        w = w * w * (3 - 2 * w)
        c = CH[b]
        out[:, :, c] = np.maximum(out[:, :, c], w)
    Image.fromarray(np.clip(out * 255.0 + 0.5, 0, 255).astype(np.uint8), "RGB").save(os.path.join(W, "Masks3_4096.png"))
    js["biomes"] = {k: v for k, v in BIOMES.items() if any(i["name"] == k for i in js["islands"])}
    js["biomesNote"] = "Tools/WorldBake/biomes.py: Masks3_4096.png R woodland G volcanic B moorland (pixel centres as Masks_4096)."
    json.dump(js, open(os.path.join(W, "World.json"), "w"), separators=(",", ":"))
    print("biomes:", js["biomes"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
