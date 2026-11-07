#!/usr/bin/env python3
"""Lakes from the final heightmap.

The web game's lakes (World.json "lakes") were found on the terrain before erosion and
the cliff pass reshaped it, so their levels and boxes no longer match the ground: flooding
them in the game spilled flat sheets of water over cliff edges. This finds the lakes the
finished terrain really holds:

  priority-flood the land from the sea  ->  the level water would fill every hollow to
  depth = filled - ground                 ->  connected wet regions are lakes
  keep the ones big and deep enough, off the ice; each lake is flat at its spill level.

Writes World.json "lakes2": [{level, i0, j0, w, h, cell, rle, area, depth}], where cells
are heightmap texels of the 4033 grid (11.16 m) and rle is run lengths of dry/wet,
row-major over the w x h box, starting with dry.

    python3 Tools/WorldBake/lakes.py            (reads Data/World, updates World.json)
"""
import heapq
import json
import os
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
W = os.path.join(ROOT, "Data", "World")

MIN_CELLS = 200       # ~2.5 ha: tarns and up; the eroded ground is full of smaller pits
MIN_DEPTH = 5.0       # metres at the deepest point
WET = 0.25            # a cell is lake if the fill covers it by this much


def main():
    js = json.load(open(os.path.join(W, "World.json")))
    S = js["terrainSize"]
    n = 4033
    raw = np.fromfile(os.path.join(W, f"Heightmap_{n}.r16"), dtype="<u2").reshape(n, n)
    zs = js["heightmap"]["zScalePercent"]
    H = (raw.astype(np.float32) - 32768.0) / 128.0 * zs / 100.0   # metres
    cell = S / (n - 1)

    # Ice (mask B) and village pads (mask2 B) are no place for a lake.
    ice = np.asarray(Image.open(os.path.join(W, "Masks_4096.png")).convert("RGBA"))[:, :, 2].astype(np.float32) / 255.0
    ice = np.asarray(Image.fromarray((ice * 255).astype(np.uint8)).resize((n, n), Image.BILINEAR)).astype(np.float32) / 255.0

    # Priority flood (Barnes et al.), seeded from the sea and the map edge.
    F = H.copy()
    seen = np.zeros((n, n), bool)
    heap = []
    sea = H <= 0.0
    seen[sea] = True
    # seed: land cells touching the sea or the border
    land = ~sea
    touch = np.zeros((n, n), bool)
    touch[1:, :] |= sea[:-1, :]; touch[:-1, :] |= sea[1:, :]; touch[:, 1:] |= sea[:, :-1]; touch[:, :-1] |= sea[:, 1:]
    touch[0, :] = touch[-1, :] = touch[:, 0] = touch[:, -1] = True
    touch &= land
    for j, i in zip(*np.nonzero(touch)):
        heapq.heappush(heap, (float(H[j, i]), int(j), int(i)))
        seen[j, i] = True
    Hl = H.tolist(); Fl = F.tolist(); seenl = seen.tolist()
    pop, push = heapq.heappop, heapq.heappush
    while heap:
        z, j, i = pop(heap)
        for dj, di in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            jj, ii = j + dj, i + di
            if jj < 0 or ii < 0 or jj >= n or ii >= n or seenl[jj][ii]:
                continue
            seenl[jj][ii] = True
            h = Hl[jj][ii]
            f = h if h > z else z
            Fl[jj][ii] = f
            push(heap, (f, jj, ii))
    F = np.array(Fl, dtype=np.float32)
    depth = F - H
    wet = (depth > WET) & (ice < 0.3) & (H > 1.0)

    # Connected regions.
    from scipy import ndimage  # noqa: E402
    lab, nl = ndimage.label(wet)
    objs = ndimage.find_objects(lab)
    lakes = []
    for k, sl in enumerate(objs, start=1):
        m = lab[sl] == k
        cnt = int(m.sum())
        if cnt < MIN_CELLS:
            continue
        d = depth[sl][m]
        if float(d.max()) < MIN_DEPTH:
            continue
        level = float(np.median(F[sl][m]))
        flat = m.astype(np.uint8).ravel()
        # run lengths, starting with a dry run
        rle = []
        cur, run = 0, 0
        for v in flat:
            if v == cur:
                run += 1
            else:
                rle.append(run); cur = v; run = 1
        rle.append(run)
        lakes.append({
            "level": round(level, 2),
            "i0": int(sl[1].start), "j0": int(sl[0].start),
            "w": int(sl[1].stop - sl[1].start), "h": int(sl[0].stop - sl[0].start),
            "cell": round(cell, 4), "res": n,
            "rle": rle,
            "area": round(cnt * cell * cell),
            "depth": round(float(d.max()), 1),
        })
    lakes.sort(key=lambda L: -L["area"])
    js["lakes2"] = lakes
    js["lakes2Note"] = "Tools/WorldBake/lakes.py: lakes of the final heightmap (priority flood). Cells of the res grid; x = -S/2 + (i0+i)*cell; rle = dry/wet runs over w*h, row-major, dry first."
    json.dump(js, open(os.path.join(W, "World.json"), "w"), separators=(",", ":"))
    tot = sum(L["area"] for L in lakes)
    print(f"{len(lakes)} lakes ({nl} wet regions before filtering), {tot/1e6:.2f} km^2; biggest:",
          [(L["level"], L["area"], L["depth"]) for L in lakes[:8]])
    return 0


if __name__ == "__main__":
    sys.exit(main())
