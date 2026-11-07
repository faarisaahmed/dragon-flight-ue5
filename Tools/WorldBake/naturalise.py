#!/usr/bin/env python3
"""naturalise.py - North Atlantic relief from the baked terrain.

The baked islands (bake.mjs -> erode.mjs -> cliffs.mjs) are covered in 100-300 m
noise lumps: on the land the median slope is ~30 deg and 44% of it is steeper than
33 deg, so most of every island renders as bare brown crag and almost nothing grows.
Real islands of this kind (the Faroes, Lofoten, the Hebrides, Shetland) are mostly
smooth turf and heath slopes, with sheer rock where the sea cuts the coast and on the
high peaks and cirque walls.

This keeps the islands where they are (every coastline, the big landforms, the
villages, the bathymetry) and changes the in-between:

  H = L + D            L = the landform (Gaussian, ~300 m), D = the noise lumps on it
  H' = L + a * D       a ~0.4 on low ground -> ~0.85 on the mountains,
                       1 on the coast (sea cliffs stay), at the villages and under water
  + hummocks           a little 6-40 m ground roughness on the gentle land
  + slope relax        a few passes of talus relaxation above ~45 deg away from the coast

Runs on the 8129 grid (5.5 m) and carries the change to 4033 (box filter) and to
16321 (bicubic delta, --16k). Keeps the input as Heightmap_<res>_cliffs.r16 so
re-running never compounds. Recomputes Masks2 R (slope). Run lakes.py afterwards.

    python3 Tools/WorldBake/naturalise.py [--16k] [--preview]
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
SCRATCH = os.environ.get("DF_PREVIEW_DIR", os.path.join(W, "NaturalPreview"))

P = dict(
    landform_m=300.0,     # Gaussian sigma of the landform
    a_low=0.36,           # lump amplitude kept on low ground
    a_high=0.85,          # ... on the mountains
    high=(260.0, 700.0),  # landform height over which a goes low -> high
    coast_keep_m=(18.0, 110.0),  # full detail within the first, fading to a by the second
    village_keep_m=(260.0, 520.0),
    hummock_m=0.9,        # amplitude of the ground roughness
    relax_deg=46.0, relax_iters=6,
    gully_m=9.0,          # deepest stream cut, metres
    gully_min_ha=1.5,     # catchment where a channel starts
)


def read(res, suffix=""):
    a = np.fromfile(os.path.join(W, f"Heightmap_{res}{suffix}.r16"), dtype="<u2").reshape(res, res)
    return a


def to_m(a, zs):
    return (a.astype(np.float32) - 32768.0) / 128.0 * zs / 100.0


def from_m(h, zs):
    v = np.round(h / (zs / 100.0) * 128.0 + 32768.0)
    return np.clip(v, 0, 65535).astype("<u2")


def smooth(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def fbm(n, cell, wavelengths, seed):
    rng = np.random.default_rng(seed)
    out = np.zeros((n, n), np.float32)
    for wl, amp in wavelengths:
        k = max(2, int(n * cell / wl))
        g = rng.standard_normal((k + 3, k + 3)).astype(np.float32)
        z = ndimage.zoom(g, (n / k), order=3)[:n, :n]
        out += z * amp
    return out


def hillshade(h, cell):
    gy, gx = np.gradient(h, cell)
    L = np.array([-1.0, -1.0, 1.4]); L /= np.linalg.norm(L)
    nz = 1.0 / np.sqrt(gx * gx + gy * gy + 1.0)
    sh = np.clip((-gx * L[0] - gy * L[1] + L[2]) * nz, 0, 1)
    rgb = np.dstack([sh * 0.92, sh * 0.96, sh]) * 255
    rgb[h <= 0] = [28, 58, 105]
    return rgb.astype(np.uint8)


def main():
    args = sys.argv[1:]
    js = json.load(open(os.path.join(W, "World.json")))
    S = js["terrainSize"]; zs = js["heightmap"]["zScalePercent"]
    n = 8129; cell = S / (n - 1)

    # Always start from the cliffs.mjs output.
    for res in (4033, 8129, 16321):
        src = os.path.join(W, f"Heightmap_{res}.r16"); keep = os.path.join(W, f"Heightmap_{res}_cliffs.r16")
        if os.path.exists(src) and not os.path.exists(keep):
            shutil.copyfile(src, keep)
    H = to_m(read(n, "_cliffs"), zs)
    land = H > 0.0
    print("loaded", n, "land km2", round(land.sum() * cell * cell / 1e6, 1))

    # Landform and lumps.
    sig = P["landform_m"] / cell
    L = ndimage.gaussian_filter(H, sig, mode="nearest")
    D = H - L

    # How much of the lumps to keep.
    a = P["a_low"] + (P["a_high"] - P["a_low"]) * smooth(P["high"][0], P["high"][1], L)
    dist_sea = ndimage.distance_transform_edt(land) * cell        # m from the sea, on land
    # Sea cliffs where the land stands high over the sea; low shores (bays, strands) smooth out.
    k_coast = (1.0 - smooth(P["coast_keep_m"][0], P["coast_keep_m"][1], dist_sea)) * smooth(25.0, 90.0, L)
    # ...and the last few tens of metres of every shore stay as they are (no plates at the tide line).
    k_coast = np.maximum(k_coast, 1.0 - smooth(15.0, 60.0, dist_sea))
    # villages and harbours stay exactly as baked
    vm = np.zeros((n, n), bool)
    pts = [(v["x"], v["z"]) for v in js.get("village", [])] + [(p["x"], p["z"]) for p in js.get("pads", [])]
    for x, z in pts:
        i = int(round((x + S / 2) / cell)); j = int(round((z + S / 2) / cell))
        if 0 <= i < n and 0 <= j < n:
            vm[j, i] = True
    dist_v = ndimage.distance_transform_edt(~vm) * cell if vm.any() else np.full((n, n), 1e9, np.float32)
    k_vil = 1.0 - smooth(P["village_keep_m"][0], P["village_keep_m"][1], dist_v)
    keep = np.maximum(k_coast, k_vil)
    keep[~land] = 1.0
    A = a + (1.0 - a) * keep
    Hn = L + A * D
    del D

    # Talus relaxation: nothing but cliff stays steeper than ~46 deg inland.
    tmax = np.tan(np.radians(P["relax_deg"])) * cell
    relax_w = (1.0 - np.maximum(k_coast, k_vil)) * land
    for _ in range(P["relax_iters"]):
        for ax in (0, 1):
            d = np.diff(Hn, axis=ax)
            ex = np.sign(d) * np.maximum(np.abs(d) - tmax, 0.0) * 0.25
            if ax == 0:
                w = np.minimum(relax_w[1:, :], relax_w[:-1, :])
                Hn[1:, :] -= ex * w; Hn[:-1, :] += ex * w
            else:
                w = np.minimum(relax_w[:, 1:], relax_w[:, :-1])
                Hn[:, 1:] -= ex * w; Hn[:, :-1] += ex * w

    # Drainage: streams and gullies where water collects. D8 flow accumulation on a 22 m
    # grid of the smoothed land, then a V cut growing with catchment (A^0.4), widened with it.
    c4 = 4
    m = n // c4
    Hs = ndimage.zoom(Hn, 1.0 / c4, order=1)[:m, :m]
    landS = Hs > 0.5
    cs = cell * c4
    order = np.argsort(-Hs, axis=None)
    acc = np.ones(m * m, np.float64)
    Hf = Hs.ravel()
    offs = [(-1, -1), (-1, 0), (-1, 1), (0, -1), (0, 1), (1, -1), (1, 0), (1, 1)]
    # steepest-descent receiver for every cell (vectorised), then accumulate in height order
    best = np.full(m * m, -1, np.int64); bestd = np.zeros(m * m)
    jj, ii = np.divmod(np.arange(m * m), m)
    for dj, di in offs:
        j2 = np.clip(jj + dj, 0, m - 1); i2 = np.clip(ii + di, 0, m - 1)
        k2 = j2 * m + i2
        drop = (Hf - Hf[k2]) / (np.hypot(dj, di))
        better = drop > bestd
        best[better] = k2[better]; bestd[better] = drop[better]
    rec = best.tolist(); accl = acc.tolist(); landl = landS.ravel().tolist()
    for k in order.tolist():
        r = rec[k]
        if r >= 0 and landl[k]:
            accl[r] += accl[k]
    A = np.array(accl).reshape(m, m) * cs * cs / 1e4            # hectares
    chan = np.where(landS, np.clip((A / P["gully_min_ha"]) ** 0.4 - 1.0, 0.0, None), 0.0)
    depth = np.minimum(chan * 2.2, P["gully_m"])
    # widen: a cut of depth d spreads over ~3-6 cells; take the max of blurred copies (V profile)
    cut = np.maximum(ndimage.grey_dilation(depth, size=3) * 0.55, depth)
    cut = ndimage.gaussian_filter(cut, 1.2)
    # not on the sea cliffs, at the villages, or the lowest few metres...
    def lowres(x):
        return ndimage.zoom(x, 1.0 / c4, order=1)[:m, :m]
    cut *= (1.0 - lowres(np.maximum(k_coast, k_vil).astype(np.float32))) * landS * smooth(3.0, 15.0, Hs)
    # ...but every channel still runs downhill all the way: walk the drainage tree from the
    # top and lower any receiver that would hold water (no pits where a cut fades out).
    Hc = (Hs - cut).ravel().tolist()
    chanl = (chan.ravel() > 0.0).tolist()
    for k in order.tolist():
        r = rec[k]
        if r >= 0 and chanl[k] and landl[r] and Hc[r] > Hc[k] - 0.02:
            Hc[r] = Hc[k] - 0.02
    cut = np.maximum(Hs - np.array(Hc, np.float32).reshape(m, m), 0.0)
    Cut = ndimage.zoom(cut, c4, order=3)[:n, :n]
    Cut = np.pad(Cut, ((0, n - Cut.shape[0]), (0, n - Cut.shape[1])), mode="edge")
    Cut = np.maximum(Cut, 0.0) * land
    Hn -= Cut
    print("gullies: deepest %.1f m, %.1f%% of land cut > 1 m" % (Cut.max(), (Cut[land] > 1.0).mean() * 100))

    # Hummocks on the gentle land.
    gy, gx = np.gradient(Hn, cell); sl = np.hypot(gx, gy)
    hum = fbm(n, cell, [(40.0, 0.55), (17.0, 0.3), (7.0, 0.15)], 7) * P["hummock_m"]
    Hn += hum * (1.0 - smooth(0.4, 0.9, sl)) * smooth(2.0, 12.0, dist_sea) * (1.0 - k_vil) * land

    # Coastline is sacred: land stays land, sea stays sea.
    Hn = np.where(land, np.maximum(Hn, np.minimum(H, 0.6)), np.minimum(Hn, H))
    Hn = np.where(land, Hn, H)

    gy, gx = np.gradient(H, cell); s0 = np.hypot(gx, gy)[land]
    gy, gx = np.gradient(Hn, cell); s1 = np.hypot(gx, gy)
    print("slope median %.2f -> %.2f   >33deg %.0f%% -> %.0f%%   >25deg %.0f%% -> %.0f%%" % (
        np.median(s0), np.median(s1[land]), (s0 > 0.66).mean() * 100, (s1[land] > 0.66).mean() * 100,
        (s0 > 0.46).mean() * 100, (s1[land] > 0.46).mean() * 100))

    if "--preview" in args:
        os.makedirs(SCRATCH, exist_ok=True)
        for name, (x, z, r) in {"home": (0, -5850, 5200), "crowhead": (15075, -225, 4400), "wyrm": (-11925, -6750, 3200)}.items():
            i = int((x + S / 2) / cell); j = int((z + S / 2) / cell); rr = int(r / cell)
            a0 = hillshade(H[j - rr:j + rr, i - rr:i + rr], cell); a1 = hillshade(Hn[j - rr:j + rr, i - rr:i + rr], cell)
            im = Image.new("RGB", (1400, 700))
            im.paste(Image.fromarray(a0).resize((700, 700)), (0, 0)); im.paste(Image.fromarray(a1).resize((700, 700)), (700, 0))
            im.save(os.path.join(SCRATCH, f"nat_{name}.jpg"), quality=85)
        print("previews in", SCRATCH)
        if "--write" not in args:
            return 0

    # Write 8129, 4033 (box), 16321 (bicubic delta).
    from_m(Hn, zs).tofile(os.path.join(W, f"Heightmap_{n}.r16"))
    Delta = Hn - H
    del H
    h4 = to_m(read(4033, "_cliffs"), zs)
    d4 = ndimage.zoom(Delta, 4033 / n, order=1)[:4033, :4033]
    from_m(h4 + d4, zs).tofile(os.path.join(W, "Heightmap_4033.r16"))
    if "--16k" in args and os.path.exists(os.path.join(W, "Heightmap_16321_cliffs.r16")):
        h16 = to_m(read(16321, "_cliffs"), zs)
        d16 = ndimage.zoom(Delta, 16321 / n, order=3)[:16321, :16321]
        from_m(h16 + d16, zs).tofile(os.path.join(W, "Heightmap_16321.r16"))
        del h16, d16
    # 16-bit PNGs (the editor imports these).
    for res in (4033, 8129) + ((16321,) if "--16k" in args else ()):
        f = os.path.join(W, f"Heightmap_{res}.r16")
        if os.path.exists(f):
            Image.fromarray(np.fromfile(f, dtype="<u2").reshape(res, res).astype(np.uint16)).save(os.path.join(W, f"Heightmap_{res}.png"))

    # Masks2 R = slope, encoded as bake.mjs / cliffs.mjs do (and the landscape shader reads it):
    # min(1, (1 - Nz) * 2.6), 0 under the sea (< -2 m), at the 4096 pixel centres.
    m2p = os.path.join(W, "Masks2_4096.png")
    m2 = np.asarray(Image.open(m2p).convert("RGBA")).copy()
    nz = 1.0 / np.sqrt(1.0 + s1 * s1)
    enc = np.minimum(1.0, (1.0 - nz) * 2.6)
    enc = np.where(Hn < -2.0, 0.0, enc)
    m2[:, :, 0] = np.clip(np.round(ndimage.zoom(enc, 4096 / n, order=1)[:4096, :4096] * 255.0), 0, 255).astype(np.uint8)
    Image.fromarray(m2).save(m2p)
    scale = 2.6
    js["naturalise"] = {"tool": "Tools/WorldBake/naturalise.py", "params": P, "slopeEncoding": "min(1,(1-Nz)*2.6)"}
    json.dump(js, open(os.path.join(W, "World.json"), "w"), separators=(",", ":"))
    print("wrote heightmaps, Masks2 slope, World.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
