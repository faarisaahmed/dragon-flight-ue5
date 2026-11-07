#!/usr/bin/env python3
"""Carry naturalise.py's change to the 16321 heightmap, in bands (low memory).

    16321 = 16321_cliffs + bicubic(8129_now - 8129_cliffs)

Reads Heightmap_8129.r16 (written by naturalise.py), Heightmap_8129_cliffs.r16 and
Heightmap_16321_cliffs.r16; writes Heightmap_16321.r16 and .png. Peak memory ~1.5 GB
(the 16-bit PNG is written from a memory-mapped array).

    python3 Tools/WorldBake/naturalise16k.py
"""
import json
import os
import sys

import numpy as np
from scipy import ndimage

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
W = os.path.join(ROOT, "Data", "World")


def main():
    js = json.load(open(os.path.join(W, "World.json")))
    zs = js["heightmap"]["zScalePercent"]
    k = zs / 100.0 / 128.0                       # metres per raw step
    n, N = 8129, 16321
    a = np.fromfile(os.path.join(W, f"Heightmap_{n}.r16"), dtype="<u2").reshape(n, n).astype(np.float32)
    b = np.fromfile(os.path.join(W, f"Heightmap_{n}_cliffs.r16"), dtype="<u2").reshape(n, n).astype(np.float32)
    D = (a - b) * k                               # metres of change on the 8129 grid
    del a, b
    src = np.memmap(os.path.join(W, f"Heightmap_{N}_cliffs.r16"), dtype="<u2", mode="r", shape=(N, N))
    out = np.memmap(os.path.join(W, f"Heightmap_{N}.r16"), dtype="<u2", mode="w+", shape=(N, N))
    f = (n - 1) / (N - 1)
    band = 1024
    xs = np.arange(N, dtype=np.float32) * f
    for r0 in range(0, N, band):
        r1 = min(N, r0 + band)
        ys = np.arange(r0, r1, dtype=np.float32) * f
        y0 = max(0, int(ys[0]) - 3); y1 = min(n, int(ys[-1]) + 5)
        sub = D[y0:y1]
        gy, gx = np.meshgrid(ys - y0, xs, indexing="ij")
        d = ndimage.map_coordinates(sub, [gy, gx], order=3, mode="nearest")
        h = src[r0:r1].astype(np.float32) * 1.0 + d / k
        out[r0:r1] = np.clip(np.round(h), 0, 65535).astype("<u2")
        print(f"rows {r0}-{r1}", flush=True)
    out.flush()
    del out, src
    try:
        from PIL import Image
        arr = np.memmap(os.path.join(W, f"Heightmap_{N}.r16"), dtype="<u2", mode="r", shape=(N, N))
        Image.fromarray(np.asarray(arr, dtype=np.uint16)).save(os.path.join(W, f"Heightmap_{N}.png"))
    except Exception as e:  # the PNG is only for the editor's importer
        print("png skipped:", e)
    print("wrote Heightmap_16321.r16/.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
