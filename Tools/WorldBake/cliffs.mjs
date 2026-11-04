#!/usr/bin/env node
// ---------------------------------------------------------------------------
// cliffs.mjs — breaks up the steep ground erode.mjs leaves behind (sea cliffs
// that drop 30-250 m in a cell or two and render as smooth curtains) into the
// kind of crag a basalt coast actually has: buttresses and gullies in the
// faces, a ragged cliff-top edge, irregular tiers, talus cones and boulder
// aprons at the foot, low wave-cut shelves and the odd sea stack.
//
// Run after erode.mjs. It always starts from the eroded maps, which it keeps
// as Heightmap_<res>_eroded.r16 (erode.mjs writes them; on the first run here
// they are copied from the current maps), so re-running never compounds.
//
// Input : Heightmap_8129_eroded.r16, Heightmap_4033_eroded.r16,
//         Heightmap_16321_eroded.r16 (only with --16k), Masks_4096.png,
//         Masks2_4096.png, Flow_4096.png (as erode.mjs left it, kept as
//         Flow_4096_eroded.png), World.json.
// Output: Heightmap_8129.r16/.png    detailed
//         Heightmap_4033.r16/.png    eroded 4033, with the box-filtered new 8129 where it changed
//         Heightmap_16321.r16/.png   eroded 16321, with the bicubic new 8129 where it changed (--16k)
//         Masks2_4096.png            R (slope) recomputed
//         Flow_4096.png              eroded flow + the change in flow accumulation
//         CliffPreview/*.png         hillshade and oblique before/after
//         World.json                 gains a "cliffs" block describing this run
//
// Pipeline (8129 grid, metres; run in 1024^2 tiles with a 64-cell halo on
// worker threads; the result is independent of the tiling):
//   1. zone     steep ground (> ~37-54 deg) with real relief (local max - min
//               over ~35 m), widened a few cells to take in the top edge and foot.
//   2. cut      the baked faces drop 100-300 m across two or three cells, which
//               leaves no cells to put detail on. The lower envelope of cones
//               rising from the ground at a noisy 62-78 deg lays each face back
//               from its foot (the shoreline stays put) to 10-20 cells wide with
//               a crisp top edge; at most 40% of the local relief / 100 m.
//   3. edge     along the fall line, the gentle ground above a face and the
//               face below are extended as planes and a rounded shoulder is
//               lifted toward the lower of the two: a crisp break, not a dome.
//   4. faces    every contour in the zone is pushed in or out along the
//               (smoothed) fall line by noise: ridged buttresses that flare out
//               toward their base, gullies that bite deepest into the top edge
//               and run out at the foot, a little fine break-up, varying with
//               height in warped, lensing layers so faces step irregularly
//               rather than being extruded straight down. Solved as a short
//               fixed-point iteration h = E(x + g(x) a(x, h)). Weaker on crests.
//   5. talus    debris cones (35 deg, lumpy, blocky) rising from the foot of
//               each face against it: big under gullies and inland crags, small
//               or absent where the sea washes the foot. A cone only spreads
//               over ground it can rest on, never out over the next cliff down.
//   6. shelf    in patches, a low rocky wave-cut platform (+0.4..1.6 m) in
//               front of sea cliffs where the water is shallow.
//   7. stacks   sparse sea stacks just off high cliffs (no taller than 5 radii).
//   8. limits   pads, villages, lakes, rivers, falls, glacier fronts and ice
//               untouched (erode.mjs's protection), nothing left in the
//               +-0.3 m waterline band.
//
// Usage: node cliffs.mjs [--dir ../../Data/World] [--16k]
//        [--crop i0 j0 size]   (dev: a square of the 8129 grid; previews only)
//        [--set '{"warp":{"amp":0.2}}'] [--threads N] [--no-write]
// Run time on an M1 (6 threads): ~70 s with --16k, peak ~4 GB resident.
// ---------------------------------------------------------------------------
import { Worker, isMainThread, parentPort } from "node:worker_threads";
import { fileURLToPath } from "node:url";
import os from "node:os";
import fs from "node:fs";
import path from "node:path";
import { S, enc, dec, hash2, fbm, ridged, smooth, writePNG, readPNG8, hillshade, renderOblique, buildProtection } from "./lib.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));

export const PARAMS = {
  zone: { relief: [10, 30], slope: [0.75, 1.4], radius: 6, widen: 2, blur: 3 },
  warp: {
    amp: 0.2, ampMin: 2, ampMax: 32,          // displacement = clamp(ampMin + amp * relief, .., ampMax) m
    dirBlur: 5,                                 // fall-line direction from a ~2x this many cells smoothed surface
    buttress: { wl: 70, k: 0.9 }, gully: { wl: 110, k: 2.0, pow: 2.5 }, fine: { wl: 16, k: 0.12 }, faceRadius: 80,
    layer: { thick: 24, thickVar: 9, share: 0.35, lens: 0.4, shift: 41 }, incline: 0.12, crest: [0.35, 0.8],
    iters: 4,
  },
  cut: { angle: [62, 78], wl: 170, maxShare: 0.4, maxCut: 100 },
  edge: { scales: [[3, 2], [6, 2]], faceSlope: 1.2, topSlope: 0.9, k: 1, maxLift: 12, maxShare: 0.15, crest: [0.35, 0.8] },
  talus: { tan: 0.70, seaShare: 0.1, landShare: 0.3, cap: 26, rough: 1.4, gullyBoost: 2.2, reach: 8, footSlope: 0.8, minFace: 25 },
  shelf: { width: 38, level: [0.4, 1.6], minDepth: -7, cover: 0.05, rough: 0.5 },
  stacks: { grid: 320, chance: 0.3, minRelief: 45, dist: [18, 85], depth: [-12, -0.6], r: [9, 22], hgt: [0.45, 0.9], slender: 5 },
  waterline: 0.3,
  protect: { falls: [25, 60] },
  halo: 64,                                   // tile overlap, cells (every stage reaches < 25)
  // previews: [file name, island, bearing from its centre (deg, 0 = +x), views]
  previews: [
    ["crowhead_east", "Crowhead", 0, ["sea", "air"]], ["crowhead_west", "Crowhead", 180, ["sea", "air"]],
    ["hearthholm_north", "Hearthholm", 90, ["sea", "air"]], ["hearthholm_south", "Hearthholm", 270, ["sea", "air"]],
    ["wildwing", "Wildwing Cliffs", 225, ["sea", "air"]],
  ],
  stages: "cut,edge,faces,talus,shelf,stacks",
};

// ---------------------------------------------------------------------------
// Grid filters (n x n, Float32Array). All clamp at the borders.
// ---------------------------------------------------------------------------
function boxBlur(src, n, r, out = new Float32Array(n * n)) {
  if (r <= 0) { out.set(src); return out; }
  const tmp = new Float32Array(n * n), inv = 1 / (2 * r + 1);
  for (let y = 0; y < n; y++) {
    const row = y * n; let s = 0;
    for (let k = -r; k <= r; k++) s += src[row + Math.min(n - 1, Math.max(0, k))];
    for (let x = 0; x < n; x++) {
      tmp[row + x] = s * inv;
      s += src[row + Math.min(n - 1, x + r + 1)] - src[row + Math.max(0, x - r)];
    }
  }
  for (let x = 0; x < n; x++) {
    let s = 0;
    for (let k = -r; k <= r; k++) s += tmp[Math.min(n - 1, Math.max(0, k)) * n + x];
    for (let y = 0; y < n; y++) {
      out[y * n + x] = s * inv;
      s += tmp[Math.min(n - 1, y + r + 1) * n + x] - tmp[Math.max(0, y - r) * n + x];
    }
  }
  return out;
}
// Sliding max (sign=1) or min (sign=-1) over a (2r+1)^2 square, van Herk / Gil-Werman.
function rankFilter(src, n, r, sign, out = new Float32Array(n * n)) {
  const w = 2 * r + 1, L = n + 2 * r;
  const buf = new Float32Array(L), g = new Float32Array(L), h = new Float32Array(L);
  const tmp = new Float32Array(n * n);
  const pass = (get, put) => {
    for (let k = 0; k < L; k++) buf[k] = sign * get(Math.min(n - 1, Math.max(0, k - r)));
    for (let k = 0; k < L; k++) g[k] = k % w === 0 ? buf[k] : Math.max(g[k - 1], buf[k]);
    for (let k = L - 1; k >= 0; k--) h[k] = (k % w === w - 1 || k === L - 1) ? buf[k] : Math.max(h[k + 1], buf[k]);
    for (let x = 0; x < n; x++) put(x, sign * Math.max(h[x], g[x + w - 1]));
  };
  for (let y = 0; y < n; y++) pass((x) => src[y * n + x], (x, v) => { tmp[y * n + x] = v; });
  for (let x = 0; x < n; x++) pass((y) => tmp[y * n + x], (y, v) => { out[y * n + x] = v; });
  return out;
}
// 16-neighbour chamfer offsets (dx, dy, length in cells) for the forward raster
// pass; the backward pass uses their negatives. Rounder than 8-neighbour cones.
const CH16 = [[-1, 0, 1], [0, -1, 1], [-1, -1, Math.SQRT2], [1, -1, Math.SQRT2], [-1, -2, Math.sqrt(5)], [1, -2, Math.sqrt(5)], [-2, -1, Math.sqrt(5)], [2, -1, Math.sqrt(5)]];
// Lower envelope in place: F(x) = min(F(x), F(y) + |x-y| * tan(x)), tan per cell (m per m).
function coneErode(F, n, step, TAN, reps = 2) {
  const off = CH16.map(([dx, dy]) => dy * n + dx), len = CH16.map((c) => c[2] * step);
  for (let rep = 0; rep < reps; rep++) {
    for (let y = 2; y < n - 2; y++) for (let x = 2; x < n - 2; x++) {
      const i = y * n + x, t = TAN[i]; let v = F[i];
      for (let k = 0; k < 8; k++) { const c = F[i + off[k]] + len[k] * t; if (c < v) v = c; }
      F[i] = v;
    }
    for (let y = n - 3; y >= 2; y--) for (let x = n - 3; x >= 2; x--) {
      const i = y * n + x, t = TAN[i]; let v = F[i];
      for (let k = 0; k < 8; k++) { const c = F[i - off[k]] + len[k] * t; if (c < v) v = c; }
      F[i] = v;
    }
  }
}
// Debris cones in place: T(x) = max(T(x), T(y) - |x-y| * tan), but a cone only
// spreads over ground it can actually rest on — a cell accepts it only while
// the debris there stays no thicker than at its source (TH), so a cone at the
// foot of a crag on a plateau never pours out over the next cliff down.
function talusCones(T, TH, G, n, step, tan, reps = 2) {
  const off = CH16.map(([dx, dy]) => dy * n + dx), len = CH16.map((c) => c[2] * step * tan);
  const relax = (i, o, l) => {
    const c = T[o] - l;
    if (c > T[i] && c - G[i] <= TH[o]) { T[i] = c; TH[i] = TH[o]; }
  };
  for (let rep = 0; rep < reps; rep++) {
    for (let y = 2; y < n - 2; y++) for (let x = 2; x < n - 2; x++) { const i = y * n + x; for (let k = 0; k < 8; k++) relax(i, i + off[k], len[k]); }
    for (let y = n - 3; y >= 2; y--) for (let x = n - 3; x >= 2; x--) { const i = y * n + x; for (let k = 0; k < 8; k++) relax(i, i - off[k], len[k]); }
  }
}
// Chamfer distance in place: D(x) = min(D(x), D(y) + |x-y|) (metres; start 0 at sources, 1e9 elsewhere).
function coneDilateMin(D, n, step) {
  const d1 = step, d2 = step * Math.SQRT2;
  for (let y = 1; y < n - 1; y++) for (let x = 1; x < n - 1; x++) {
    const i = y * n + x; let v = D[i];
    const a = D[i - 1] + d1, b = D[i - n] + d1, c = D[i - n - 1] + d2, d = D[i - n + 1] + d2;
    if (a < v) v = a; if (b < v) v = b; if (c < v) v = c; if (d < v) v = d; D[i] = v;
  }
  for (let y = n - 2; y >= 1; y--) for (let x = n - 2; x >= 1; x--) {
    const i = y * n + x; let v = D[i];
    const a = D[i + 1] + d1, b = D[i + n] + d1, c = D[i + n + 1] + d2, d = D[i + n - 1] + d2;
    if (a < v) v = a; if (b < v) v = b; if (c < v) v = c; if (d < v) v = d; D[i] = v;
  }
}

// ---------------------------------------------------------------------------
// The detailing itself, on an n x n grid E (eroded heights) whose cell (0,0)
// is 8129-grid cell (oi, oj). P = protection 0..1 (1 = free to change).
// Returns { H, Wz } — new heights and the change-zone weight.
// ---------------------------------------------------------------------------
export function detail(E, P, n, oi, oj, step, prm, log = () => {}, count = {}) {
  const N = n * n;
  const wxOf = (x) => -S / 2 + (oi + x) * step, wzOf = (y) => -S / 2 + (oj + y) * step;
  // --- 1. zone ---
  const Z = prm.zone;
  const Ec = new Float32Array(N); for (let i = 0; i < N; i++) Ec[i] = E[i] < -5 ? -5 : E[i];
  const mx = rankFilter(Ec, n, Z.radius, 1), mn = rankFilter(Ec, n, Z.radius, -1);
  const R = new Float32Array(N); for (let i = 0; i < N; i++) R[i] = mx[i] - mn[i];
  const sl = new Float32Array(N);
  for (let y = 1; y < n - 1; y++) for (let x = 1; x < n - 1; x++) {
    const i = y * n + x;
    sl[i] = Math.hypot(E[i + 1] - E[i - 1], E[i + n] - E[i - n]) / (2 * step);
  }
  const slW = rankFilter(sl, n, Z.widen, 1);
  let Wz = new Float32Array(N);
  for (let i = 0; i < N; i++) Wz[i] = smooth(Z.slope[0], Z.slope[1], slW[i]) * smooth(Z.relief[0], Z.relief[1], R[i]) * (mx[i] > 2 ? 1 : 0);
  Wz = boxBlur(boxBlur(Wz, n, Z.blur), n, Z.blur);
  for (let i = 0; i < N; i++) Wz[i] = Math.min(1, Wz[i] * 1.6) * P[i];
  // a wider mask for the cut, which works up to ~12 cells inland of a face
  const Wc = boxBlur(rankFilter(Wz, n, Z.widen + 10, 1), n, 3);
  log("zone ready");

  // --- 2. faces ---
  const Wp = prm.warp;
  const Ed = boxBlur(boxBlur(E, n, Wp.dirBlur), n, Wp.dirBlur);
  const ST = prm.stages.split(",");
  const bil = (G, fx, fy) => {
    if (fx < 0) fx = 0; if (fy < 0) fy = 0; if (fx > n - 1.001) fx = n - 1.001; if (fy > n - 1.001) fy = n - 1.001;
    const ix = fx | 0, iy = fy | 0, tx = fx - ix, ty = fy - iy, c = iy * n + ix;
    return (G[c] * (1 - tx) + G[c + 1] * tx) * (1 - ty) + (G[c + n] * (1 - tx) + G[c + n + 1] * tx) * ty;
  };

  // --- 2a. cut: lay faces back to 62-78 deg ---
  // A heightmap face only has as much detail as it has cells across it, and
  // the baked cliffs drop 100-250 m in two or three. The lower envelope of
  // cones rising from the ground at a noisy face angle cuts each cliff back
  // from its foot (the shoreline stays put) to a face 10-20 cells wide with a
  // crisp top edge; the cut is limited to a share of the local relief so
  // headlands and pinnacles keep their height.
  let C = E;
  if (ST.includes("cut")) {
    const CU = prm.cut;
    const TAN = new Float32Array(N);
    for (let y = 0; y < n; y++) for (let x = 0; x < n; x++) {
      const wx = wxOf(x), wz = wzOf(y);
      const a = CU.angle[0] + (CU.angle[1] - CU.angle[0]) * smooth(-0.55, 0.55, fbm(wx / CU.wl, wz / CU.wl, 3, 1601));
      TAN[y * n + x] = Math.tan(a * Math.PI / 180);
    }
    const F = new Float32Array(E);
    coneErode(F, n, step, TAN);
    C = new Float32Array(N);
    let cut = 0;
    for (let i = 0; i < N; i++) {
      let d = E[i] - F[i];
      if (d > 0) { d = Math.min(d, CU.maxShare * R[i], CU.maxCut) * Wc[i] * P[i]; cut++; }
      C[i] = E[i] - Math.max(0, d);
    }
    count.cut = cut; log(`cut: ${cut} cells lowered`);
  }

  // --- 2b. edge: rebuild the convex corner at the top of each face ---
  // Along the fall line, the gentle ground k_u cells above and the face k_f
  // cells below are extended as planes; a rounded shoulder below the lower of
  // the two is lifted toward it, so the slope breaks over one cell.
  const Es = new Float32Array(C);
  if (ST.includes("edge")) {
    const EG = prm.edge;
    let lifted = 0;
    for (let y = 2; y < n - 2; y++) for (let x = 2; x < n - 2; x++) {
      const i = y * n + x, w = Wz[i];
      if (w <= 0.01) continue;
      let gx = Ed[i + 1] - Ed[i - 1], gz = Ed[i + n] - Ed[i - n];
      const gl = Math.hypot(gx, gz); if (gl < 1e-4) continue;
      gx /= gl; gz /= gl;
      const h = C[i];
      if (h < 1) continue;
      let best = 0;
      for (const [ku, kf] of EG.scales) {
        const ux = x + gx * ku, uy = y + gz * ku, fx = x - gx * kf, fy = y - gz * kf;
        const hU = bil(C, ux, uy), hF = bil(C, fx, fy);
        const sU = (bil(C, ux + gx, uy + gz) - bil(C, ux - gx, uy - gz)) / (2 * step);
        const sF = (bil(C, fx + gx, fy + gz) - bil(C, fx - gx, fy - gz)) / (2 * step);
        if (sF < EG.faceSlope || sU > EG.topSlope || sF < sU + 0.6) continue;
        const up = hU - Math.max(0, sU) * ku * step, fp = hF + sF * kf * step;
        const tgt = Math.min(up, fp, hU);
        if (tgt - h > best) best = tgt - h;
      }
      if (best > 0) { Es[i] = h + w * EG.k * smooth(EG.crest[0], EG.crest[1], gl / (2 * step)) * Math.min(best, EG.maxLift, EG.maxShare * R[i]); lifted++; }
    }
    count.edge = lifted; log(`edge: ${lifted} cells lifted`);
  }
  const H = new Float32Array(Es);
  const samp = (fx, fy) => bil(Es, fx, fy);
  const B = Wp.buttress, G = Wp.gully, F = Wp.fine, LY = Wp.layer;
  // t = height within the face (0 foot .. 1 top): gullies bite deepest into the
  // top edge and run out at the foot (their floors are less steep than the
  // face, as couloirs are); buttresses flare out toward their base.
  const base = (qx, qz, t) =>
    B.k * (ridged(qx / B.wl, qz / B.wl, 3, 501) - 0.55) * (1.2 - 0.6 * t)
    - G.k * Math.pow(ridged(qx / G.wl + 17.3, qz / G.wl - 4.1, 2, 733), G.pow) * (0.15 + 1.1 * t)
    + F.k * fbm(qx / F.wl, qz / F.wl, 2, 911);
  const fr = Math.round(Wp.faceRadius / step);
  const fLo = rankFilter(Es, n, fr, -1), fHi = rankFilter(Es, n, fr, 1);
  const GD = new Float32Array(N);       // gully-ness (for talus cones), at the face
  let warped = 0;
  if (ST.includes("faces")) for (let y = 1; y < n - 1; y++) for (let x = 1; x < n - 1; x++) {
    const i = y * n + x, w = Wz[i];
    if (w <= 0.002) continue;
    let gx = Ed[i + 1] - Ed[i - 1], gz = Ed[i + n] - Ed[i - n];
    const gl = Math.hypot(gx, gz); if (gl < 1e-4) continue;
    gx /= gl; gz /= gl;
    const wx = wxOf(x), wz = wzOf(y);
    // less on crests and narrow ridges, where the smoothed fall line is unreliable
    const A = w * smooth(Wp.crest[0], Wp.crest[1], gl / (2 * step)) * Math.min(Wp.ampMax, Wp.ampMin + Wp.amp * R[i]) / step;     // cells
    // layer geometry varies slowly
    const thick = LY.thick + LY.thickVar * fbm(wx / 1400, wz / 1400, 2, 41);
    const lvlOff = 18 * fbm(wx / 650, wz / 650, 3, 55) + 0.003 * wx - 0.002 * wz;
    const incx = Wp.incline * Math.cos(fbm(wx / 2000, wz / 2000, 1, 7) * 6), incz = Wp.incline * Math.sin(fbm(wx / 2000, wz / 2000, 1, 7) * 6);
    const lens = smooth(-0.4, 0.3, fbm(wx / 300, wz / 300, 2, 63));         // where the layering shows
    let h = Es[i], hn = h;
    for (let it = 0; it < Wp.iters; it++) {
      const u = (h + lvlOff) / thick, L = Math.floor(u), f = u - L;
      const b = smooth(LY.lens, 1, f);
      const o0x = ((hash2(L, 3, 99) & 1023) / 1023 - 0.5) * 2 * LY.shift, o0z = ((hash2(L, 5, 99) & 1023) / 1023 - 0.5) * 2 * LY.shift;
      const o1x = ((hash2(L + 1, 3, 99) & 1023) / 1023 - 0.5) * 2 * LY.shift, o1z = ((hash2(L + 1, 5, 99) & 1023) / 1023 - 0.5) * 2 * LY.shift;
      const t = Math.min(1, Math.max(0, (h - Math.max(fLo[i], -2)) / Math.max(1, fHi[i] - Math.max(fLo[i], -2))));
      const nl = base(wx + o0x, wz + o0z, t) * (1 - b) + base(wx + o1x, wz + o1z, t) * b;
      const nc = base(wx + h * incx, wz + h * incz, t);
      const sh = LY.share * lens;
      const nv = nl * sh + nc * (1 - sh);
      hn = samp(x + gx * A * nv, y + gz * A * nv);
      h = it === 0 ? hn : 0.5 * (h + hn);
      if (it === Wp.iters - 1) GD[i] = w * Math.pow(ridged(wx / G.wl + 17.3, wz / G.wl - 4.1, 2, 733), G.pow);
    }
    H[i] = h; warped++;
  }
  count.faces = warped; log(`faces: ${warped} cells displaced`);


  // --- 4. talus ---
  const TL = prm.talus;
  const T = new Float32Array(N).fill(-1e9), TH = new Float32Array(N);
  if (ST.includes("talus")) {
    // Sources: the foot of each face, i.e. gentle cells (or sea floor) within two
    // cells of a steep one. A cone of debris rises from there against the face.
    const sH = new Float32Array(N), steep = new Float32Array(N);
    for (let y = 1; y < n - 1; y++) for (let x = 1; x < n - 1; x++) {
      const i = y * n + x;
      sH[i] = Math.hypot(H[i + 1] - H[i - 1], H[i + n] - H[i - n]) / (2 * step);
      steep[i] = sH[i] > 1.3 ? 1 : 0;
    }
    const near = rankFilter(steep, n, 2, 1), above = rankFilter(H, n, TL.reach, 1), below = rankFilter(H, n, TL.reach, -1), gd = rankFilter(GD, n, 3, 1);
    for (let y = 1; y < n - 1; y++) for (let x = 1; x < n - 1; x++) {
      const i = y * n + x, w = Wz[i];
      if (w < 0.25 || near[i] < 1 || sH[i] > TL.footSlope) continue;
      const wx = wxOf(x), wz = wzOf(y);
      const foot = H[i], rel = above[i] - foot;
      if (foot - below[i] > 0.2 * (above[i] - below[i])) continue;     // the bottom of the face, not the top
      const share = (foot > 2 ? TL.landShare : TL.seaShare + (TL.landShare - TL.seaShare) * smooth(-6, 2, foot))
        * (1 + TL.gullyBoost * gd[i]) * 1.4 * Math.pow(smooth(-0.25, 0.55, fbm(wx / 200, wz / 200, 3, 1201)), 1.5);   // patchy: some feet are bare, washed by the sea
      if (rel < TL.minFace) continue;
      const top = foot + Math.min(TL.cap, share * rel) * w;
      if (top > T[i]) { T[i] = top; TH[i] = top - foot + 0.5; }
    }
    talusCones(T, TH, H, n, step, TL.tan);
    // round off the cone apexes (no sawtooth of peaks along the foot)
    {
      const v = new Float32Array(N), tv = new Float32Array(N);
      for (let i = 0; i < N; i++) if (T[i] > H[i]) { v[i] = 1; tv[i] = T[i] - H[i]; }      // thickness, not level: never averaged across a cliff
      const num = boxBlur(boxBlur(tv, n, 1), n, 1), den = boxBlur(boxBlur(v, n, 1), n, 1);
      for (let i = 0; i < N; i++) T[i] = v[i] ? H[i] + Math.min(TL.cap, num[i] / den[i]) : -1e9;
    }
    let cells = 0;
    for (let y = 1; y < n - 1; y++) for (let x = 1; x < n - 1; x++) {
      const i = y * n + x, d = T[i] - H[i];
      if (d <= 0 || P[i] <= 0) continue;
      const wx = wxOf(x), wz = wzOf(y);
      // lumpy cones and lobes rather than a planar ramp, with blocky debris on top
      const lump = 0.75 + 0.35 * smooth(-0.6, 0.6, fbm(wx / 28, wz / 28, 2, 1309));
      const rough = (TL.rough * fbm(wx / 7, wz / 7, 2, 1301) + 0.8 * fbm(wx / 30, wz / 30, 2, 1307)) * smooth(1, 6, d);
      H[i] += P[i] * Math.max(0, d * lump + rough);
      cells++;
    }
    count.talus = cells; log(`talus: ${cells} cells`);
  }

  // --- 5. shelf ---
  const SH = prm.shelf;
  if (ST.includes("shelf")) {
    const D = new Float32Array(N);
    for (let i = 0; i < N; i++) D[i] = H[i] > 3 && Wz[i] > 0.3 ? 0 : 1e9;
    coneDilateMin(D, n, step);
    let cells = 0;
    for (let y = 1; y < n - 1; y++) for (let x = 1; x < n - 1; x++) {
      const i = y * n + x, h = H[i], d = D[i];
      if (h > 0.6 || h < SH.minDepth || d > SH.width || P[i] <= 0) continue;
      const wx = wxOf(x), wz = wzOf(y);
      const patch = smooth(SH.cover - 0.25, SH.cover + 0.25, fbm(wx / 240, wz / 240, 3, 1401));
      const lim = SH.width * patch * (0.55 + 0.45 * smooth(-0.5, 0.5, fbm(wx / 24, wz / 24, 2, 1403)));
      if (d >= lim) continue;
      const lv = SH.level[0] + (SH.level[1] - SH.level[0]) * (0.5 + 0.5 * fbm(wx / 90, wz / 90, 2, 1405)) - 0.025 * d;
      const rough = SH.rough * (ridged(wx / 6, wz / 6, 2, 1407) - 0.5) * 2;
      const edgeFall = smooth(lim, lim - 6, d);                 // steps down at its seaward edge
      const tgt = h + (lv + rough - h) * edgeFall;
      if (tgt > h) { H[i] = h + (tgt - h) * P[i]; cells++; }
    }
    count.shelf = cells; log(`shelf: ${cells} cells`);
  }
  // --- 6. stacks ---
  const SK = prm.stacks;
  if (ST.includes("stacks")) {
    const D = new Float32Array(N);
    for (let i = 0; i < N; i++) D[i] = H[i] > 3 && Wz[i] > 0.3 ? 0 : 1e9;
    coneDilateMin(D, n, step);
    const near = rankFilter(H, n, Math.round(110 / step), 1);
    const gs = SK.grid, wx0 = wxOf(0), wz0 = wzOf(0), wx1 = wxOf(n - 1), wz1 = wzOf(n - 1);
    let placed = 0; const stackList = [];
    for (let gy = Math.floor(wz0 / gs); gy <= Math.floor(wz1 / gs); gy++) for (let gx = Math.floor(wx0 / gs); gx <= Math.floor(wx1 / gs); gx++) {
      const hs = hash2(gx, gy, 4242);
      if ((hs & 1023) / 1023 > SK.chance) continue;
      const px = (gx + ((hs >>> 10) & 1023) / 1023) * gs, pz = (gy + ((hs >>> 20) & 1023) / 1023) * gs;
      const ci = Math.round((px + S / 2) / step) - oi, cj = Math.round((pz + S / 2) / step) - oj;
      const margin = Math.ceil(SK.r[1] * 2.2 / step) + 2;
      if (ci < margin || cj < margin || ci >= n - margin || cj >= n - margin) continue;
      const c = cj * n + ci, h0 = H[c];
      if (h0 < SK.depth[0] || h0 > SK.depth[1] || D[c] < SK.dist[0] || D[c] > SK.dist[1] || P[c] < 0.95) continue;
      const cliffTop = near[c];
      if (cliffTop < SK.minRelief) continue;
      const u1 = (hash2(gx, gy, 4243) & 1023) / 1023, u2 = (hash2(gx, gy, 4244) & 1023) / 1023;
      const r = SK.r[0] + (SK.r[1] - SK.r[0]) * u1;
      const top = Math.min(cliffTop * (SK.hgt[0] + (SK.hgt[1] - SK.hgt[0]) * u2), SK.slender * r);   // no needles
      // keep the stack inside protected-free sea
      let ok = true;
      const rc = Math.ceil(r * 2.2 / step);
      for (let dy = -rc; dy <= rc && ok; dy++) for (let dx = -rc; dx <= rc; dx++) { const k = c + dy * n + dx; if (P[k] < 0.95 || H[k] > 2) { ok = false; break; } }
      if (!ok) continue;
      for (let dy = -rc; dy <= rc; dy++) for (let dx = -rc; dx <= rc; dx++) {
        const k = c + dy * n + dx, x = ci + dx, y = cj + dy, wx = wxOf(x), wz = wzOf(y);
        const rho = Math.hypot(dx, dy) * step;
        const rr = r * (0.7 + 0.6 * (0.5 + 0.5 * fbm(wx / 11, wz / 11, 2, 4300 + (hs & 255))));
        let h;
        if (rho < rr) {
          const q = rho / rr;
          h = top * (1 - 0.12 * q * q) + 0.07 * top * fbm(wx / 14, wz / 14, 2, 4310) + 2.5 * (ridged(wx / 6, wz / 6, 2, 4320) - 0.5);
          // a stepped, undercut-looking flank: steeper near the waterline
          h -= top * 0.25 * smooth(0.75, 1, q);
        } else {
          const t = (rho - rr) / (0.9 * rr);
          if (t > 1) continue;
          h = 1.2 + top * 0.1 * (1 - t) * (1 - t) + 1.2 * (ridged(wx / 7, wz / 7, 2, 4330) - 0.5);    // fallen blocks round the base
        }
        if (h > H[k]) H[k] = h;
      }
      placed++; stackList.push({ x: Math.round(px), z: Math.round(pz), r: +r.toFixed(1), top: +top.toFixed(1), ci, cj });
    }
    count.stacks = placed; count.stackList = stackList; log(`stacks: ${placed}`);
  }
  return { H, Wz, R, GD, mn };
}

// Protection 0..1: erode.mjs's (water, lakes, pads/villages), ice, and a
// disc around every waterfall so the falls meshes still meet their cliff.
function protection(dir, E, n, oi, oj, step, world, prm) {
  const { P: P8, ICE } = buildProtection({ dir, H0: E, n, oi, oj, step, world });
  const P = new Float32Array(n * n);
  for (let i = 0; i < n * n; i++) P[i] = P8[i] / 255 * (1 - smooth(0.05, 0.5, ICE[i] / 255));
  const fade = (x, z, r0, r1) => {
    const ia = Math.max(0, Math.floor((x - r1 + S / 2) / step) - oi), ib = Math.min(n - 1, Math.ceil((x + r1 + S / 2) / step) - oi);
    const ja = Math.max(0, Math.floor((z - r1 + S / 2) / step) - oj), jb = Math.min(n - 1, Math.ceil((z + r1 + S / 2) / step) - oj);
    for (let y = ja; y <= jb; y++) for (let xx = ia; xx <= ib; xx++) {
      const d = Math.hypot(-S / 2 + (oi + xx) * step - x, -S / 2 + (oj + y) * step - z);
      P[y * n + xx] *= smooth(r0, r1, d);
    }
  };
  for (const f of world.falls || []) { fade(f.x, f.z, prm.protect.falls[0], prm.protect.falls[1]); if (f.x1 !== undefined) fade(f.x1, f.z1, prm.protect.falls[0], prm.protect.falls[1]); }
  for (const g of world.glacierFronts || []) fade(g.x, g.z, g.w * 0.6, g.w * 0.6 + 60);
  for (const p of world.pads || []) fade(p.x, p.z, p.r + 10, p.r + 60);
  return P;
}

// ---------------------------------------------------------------------------
// Workers: tiles of the 8129 grid (core + halo), and the resampling to the
// other two resolutions in bands of rows.
// ---------------------------------------------------------------------------
const OPS = {
  tile(j) {
    const E = new Float32Array(j.E), P = new Float32Array(j.P), OUT = new Float32Array(j.OUT);
    const { NF, x0, y0, x1, y1, halo, step, prm } = j;
    // square window of the core plus a halo, slid inward at the grid's edges
    const n = Math.min(NF, j.T + 2 * halo);
    const wi = Math.min(Math.max(0, x0 - halo), NF - n), wj = Math.min(Math.max(0, y0 - halo), NF - n);
    const e = new Float32Array(n * n), p = new Float32Array(n * n);
    for (let y = 0; y < n; y++) { e.set(E.subarray((wj + y) * NF + wi, (wj + y) * NF + wi + n), y * n); p.set(P.subarray((wj + y) * NF + wi, (wj + y) * NF + wi + n), y * n); }
    const count = {};
    const { H } = detail(e, p, n, wi + j.oi, wj + j.oj, step, prm, () => {}, count);
    for (let y = y0; y < y1; y++) for (let x = x0; x < x1; x++) OUT[y * NF + x] = H[(y - wj) * n + (x - wi)];
    // stacks are reported by the tile whose core holds their centre
    count.stackList = (count.stackList || []).filter((s) => s.ci + wi >= x0 && s.ci + wi < x1 && s.cj + wj >= y0 && s.cj + wj < y1).map(({ x, z, r, top }) => ({ x, z, r, top }));
    count.stacks = count.stackList.length;
    return count;
  },
  // Another grid = its eroded original, with the new 8129 surface (box or
  // clamped bicubic) blended in where the 8129 map changed.
  resample(j) {
    const H = new Float32Array(j.H), W = new Float32Array(j.W), U = new Uint16Array(j.U);
    const { nf, n, y0, y1, mode } = j;
    const r = (nf - 1) / (n - 1);
    const at = (G, fx, fy) => {
      if (fx < 0) fx = 0; if (fy < 0) fy = 0; if (fx > nf - 1.001) fx = nf - 1.001; if (fy > nf - 1.001) fy = nf - 1.001;
      const ix = fx | 0, iy = fy | 0, tx = fx - ix, ty = fy - iy, c = iy * nf + ix;
      return (G[c] * (1 - tx) + G[c + 1] * tx) * (1 - ty) + (G[c + nf] * (1 - tx) + G[c + nf + 1] * tx) * ty;
    };
    const cr = (p0, p1, p2, p3, t) => p1 + 0.5 * t * (p2 - p0 + t * (2 * p0 - 5 * p1 + 4 * p2 - p3 + t * (3 * (p1 - p2) + p3 - p0)));
    const cub = (fx, fy) => {
      const ix = Math.min(nf - 2, Math.floor(fx)), iy = Math.min(nf - 2, Math.floor(fy)), tx = fx - ix, ty = fy - iy;
      const row = [0, 0, 0, 0];
      for (let k = -1; k <= 2; k++) {
        const yy = Math.min(nf - 1, Math.max(0, iy + k)) * nf;
        row[k + 1] = cr(H[yy + Math.max(0, ix - 1)], H[yy + ix], H[yy + ix + 1], H[yy + Math.min(nf - 1, ix + 2)], tx);
      }
      let v = cr(row[0], row[1], row[2], row[3], ty);
      // no overshoot beyond the four surrounding samples (no ringing at cliff steps)
      const c = iy * nf + ix, lo = Math.min(H[c], H[c + 1], H[c + nf], H[c + nf + 1]), hi = Math.max(H[c], H[c + 1], H[c + nf], H[c + nf + 1]);
      return v < lo ? lo : v > hi ? hi : v;
    };
    for (let y = y0; y < y1; y++) for (let x = 0; x < n; x++) {
      const fx = x * r, fy = y * r;
      const w = at(W, fx, fy);
      if (w <= 0.001) continue;
      const h = mode === "box"
        ? 0.25 * (at(H, fx - 0.5, fy - 0.5) + at(H, fx + 0.5, fy - 0.5) + at(H, fx - 0.5, fy + 0.5) + at(H, fx + 0.5, fy + 0.5))
        : cub(fx, fy);
      const i = y * n + x, h0 = dec(U[i]);
      U[i] = enc(h0 + (h - h0) * Math.min(1, w));
    }
  },
};
if (!isMainThread) parentPort.on("message", (job) => parentPort.postMessage(OPS[job.op](job) ?? { ok: 1 }));

// Multiple-flow-direction accumulation (cells) over land of a grid (M x M),
// highest first; the sea (h <= 0) and pits absorb.
function mfdAccum(G, M) {
  const N = M * M, acc = new Float32Array(N);
  const lo = -1, BIN = 0.05, nb = Math.ceil(1400 / BIN);
  const cnt = new Uint32Array(nb + 1);
  const key = (h) => Math.min(nb - 1, Math.max(0, Math.floor((h - lo) / BIN)));
  for (let i = 0; i < N; i++) if (G[i] > 0) cnt[key(G[i]) + 1]++;
  for (let k = 0; k < nb; k++) cnt[k + 1] += cnt[k];
  const order = new Uint32Array(cnt[nb]);
  const pos = cnt.slice();
  for (let i = 0; i < N; i++) if (G[i] > 0) order[pos[key(G[i])]++] = i;
  const OFF = [-1, 1, -M, M, -M - 1, -M + 1, M - 1, M + 1], DST = [1, 1, 1, 1, Math.SQRT2, Math.SQRT2, Math.SQRT2, Math.SQRT2];
  const wts = new Float32Array(8);
  for (let k = order.length - 1; k >= 0; k--) {
    const i = order[k], x = i % M, y = (i / M) | 0;
    acc[i] += 1;
    if (x === 0 || y === 0 || x === M - 1 || y === M - 1) continue;
    const h = G[i];
    let sum = 0;
    for (let q = 0; q < 8; q++) { const d = h - G[i + OFF[q]]; const w = d > 0 ? Math.pow(d / DST[q], 1.1) : 0; wts[q] = w; sum += w; }
    if (sum <= 0) continue;
    const a = acc[i] / sum;
    for (let q = 0; q < 8; q++) if (wts[q] > 0) acc[i + OFF[q]] += a * wts[q];
  }
  return acc;
}

// ---------------------------------------------------------------------------
// Main.
// ---------------------------------------------------------------------------
if (isMainThread && process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const argv = process.argv;
  const arg = (k, d) => { const i = argv.indexOf(k); return i > 0 ? argv[i + 1] : d; };
  const DIR = path.resolve(arg("--dir", path.join(HERE, "..", "..", "Data", "World")));
  const DO16 = argv.includes("--16k");
  const THREADS = +arg("--threads", Math.max(1, os.cpus().length - 1));
  const ci = argv.indexOf("--crop");
  const CROP = ci > 0 ? { i0: +argv[ci + 1], j0: +argv[ci + 2], size: +argv[ci + 3] } : null;
  const PRM = PARAMS;
  const deepMerge = (a, b) => { for (const [k, v] of Object.entries(b)) { if (v && typeof v === "object" && !Array.isArray(v) && a[k] && typeof a[k] === "object") deepMerge(a[k], v); else a[k] = v; } };
  if (arg("--set")) deepMerge(PRM, JSON.parse(arg("--set")));
  const NF = 8129, step = S / (NF - 1);
  const t0 = Date.now();
  const log = (...a) => console.log(`[${((Date.now() - t0) / 1000).toFixed(1)}s]`, ...a);
  const PREV = path.join(DIR, "CliffPreview");
  fs.mkdirSync(PREV, { recursive: true });
  const world = JSON.parse(fs.readFileSync(path.join(DIR, "World.json"), "utf8"));

  // --- inputs: the eroded maps (made from the current ones on the first run) ---
  const erodedOf = (res) => path.join(DIR, `Heightmap_${res}_eroded.r16`);
  const curOf = (res, ext) => path.join(DIR, `Heightmap_${res}.${ext}`);
  const flowEroded = path.join(DIR, "Flow_4096_eroded.png");
  if (!CROP) {
    const need = [8129, 4033, ...(DO16 ? [16321] : [])].filter((r) => !fs.existsSync(erodedOf(r)));
    if (need.length || !fs.existsSync(flowEroded)) {
      if (world.cliffs) throw new Error(`World.json says cliffs.mjs already ran but ${need.map((r) => path.basename(erodedOf(r))).join(", ") || "Flow_4096_eroded.png"} is missing: re-run erode.mjs first`);
      for (const r of need) { fs.copyFileSync(curOf(r, "r16"), erodedOf(r)); log(`kept ${path.basename(curOf(r, "r16"))} as ${path.basename(erodedOf(r))}`); }
      if (!fs.existsSync(flowEroded)) fs.copyFileSync(path.join(DIR, "Flow_4096.png"), flowEroded);
    }
  }
  const src8 = fs.existsSync(erodedOf(8129)) ? erodedOf(8129) : curOf(8129, "r16");
  const raw8 = new Uint16Array(fs.readFileSync(src8).buffer.slice(0));
  if (raw8.length !== NF * NF) throw new Error("8129 size");
  const oi = CROP ? CROP.i0 : 0, oj = CROP ? CROP.j0 : 0, n = CROP ? CROP.size : NF, N = n * n;
  const sab = (bytes) => new SharedArrayBuffer(bytes);
  const Es = sab(N * 4), E = new Float32Array(Es);
  for (let y = 0; y < n; y++) for (let x = 0; x < n; x++) E[y * n + x] = dec(raw8[(oj + y) * NF + oi + x]);
  log(`loaded ${path.basename(src8)}; grid ${n}^2 at (${oi},${oj}); ${THREADS} threads`);
  const Ps = sab(N * 4), P = new Float32Array(Ps);
  P.set(protection(DIR, E, n, oi, oj, step, world, PRM));
  log("protection ready");

  // --- detail, in tiles ---
  const Hs = sab(N * 4), H = new Float32Array(Hs);
  const count = { cut: 0, edge: 0, faces: 0, talus: 0, shelf: 0, stacks: 0, stackList: [] };
  const workers = Array.from({ length: THREADS }, () => new Worker(fileURLToPath(import.meta.url)));
  async function run(jobs) {
    let next = 0; const results = [];
    await Promise.all(workers.map((w) => new Promise((res, rej) => {
      const feed = () => { if (next >= jobs.length) { w.off("message", onMsg); w.off("error", rej); res(); return; } w.postMessage(jobs[next++]); };
      const onMsg = (m) => { results.push(m); feed(); };
      w.on("message", onMsg); w.on("error", rej); feed();
    })));
    return results;
  }
  {
    const T = 1024, halo = PRM.halo, jobs = [];
    for (let y0 = 0; y0 < n; y0 += T) for (let x0 = 0; x0 < n; x0 += T) {
      const x1 = Math.min(n, x0 + T), y1 = Math.min(n, y0 + T);
      let land = false;
      for (let y = Math.max(0, y0 - halo); y < Math.min(n, y1 + halo) && !land; y += 4) for (let x = Math.max(0, x0 - halo); x < Math.min(n, x1 + halo); x += 4) if (E[y * n + x] > 1) { land = true; break; }
      if (!land) { for (let y = y0; y < y1; y++) H.set(E.subarray(y * n + x0, y * n + x1), y * n + x0); continue; }
      jobs.push({ op: "tile", E: Es, P: Ps, OUT: Hs, NF: n, T, x0, y0, x1, y1, halo, step, prm: PRM, oi, oj });
    }
    const res = await run(jobs);
    for (const c of res) for (const k of Object.keys(count)) if (k === "stackList") count.stackList.push(...(c.stackList || [])); else count[k] += c[k] || 0;
    if (CROP) log("stacks", JSON.stringify(count.stackList));
    log(`detail: ${jobs.length} tiles; cut ${count.cut}, edge ${count.edge}, faces ${count.faces}, talus ${count.talus}, shelf ${count.shelf} cells; ${count.stacks} stacks`);
  }

  // --- limits: protection already scaled every change; keep out of the waterline band ---
  const wl = PRM.waterline;
  let flips = 0;
  for (let i = 0; i < N; i++) {
    let h = H[i];
    if (P[i] <= 0) h = E[i];
    if (h > -wl && h < wl && h !== E[i]) h = (E[i] > 0) === (h >= 0) ? (h >= 0 ? wl : -wl) : (h >= 0 ? wl : -wl);
    H[i] = h;
    if ((E[i] > 0) !== (h > 0)) flips++;
  }
  // stats
  let maxUp = 0, maxDn = 0, changed = 0, sumAbs = 0;
  for (let i = 0; i < N; i++) { const d = H[i] - E[i]; if (Math.abs(d) > 0.05) { changed++; sumAbs += Math.abs(d); } if (d > maxUp) maxUp = d; if (d < maxDn) maxDn = d; }
  const stats = { cellsChanged: changed, meanAbsChanged: +(sumAbs / Math.max(1, changed)).toFixed(2), maxUp: +maxUp.toFixed(1), maxDown: +maxDn.toFixed(1), coastCellsChanged: flips, ...count };
  delete stats.stackList;
  log("stats", JSON.stringify(stats));

  // --- previews ---
  const shots = [];
  const toIJ = (wx, wz) => [Math.round((wx + S / 2) / step) - oi, Math.round((wz + S / 2) / step) - oj];
  const shore = (G, cx, cz, ang) => {           // first land -> open sea crossing walking out from (cx,cz)
    const dx = Math.cos(ang), dz = Math.sin(ang);
    const h = (x, z) => { const [i, j] = toIJ(x, z); return i < 0 || j < 0 || i >= n || j >= n ? -99 : G[j * n + i]; };
    for (let d = 0; d < 8000; d += 2) {
      if (h(cx + dx * d, cz + dz * d) > 0 && h(cx + dx * (d + 2), cz + dz * (d + 2)) <= 0) {
        let clear = true; for (let e = d + 4; e < d + 900; e += 4) if (h(cx + dx * e, cz + dz * e) > 0) { clear = false; break; }
        if (clear) return { x: cx + dx * d, z: cz + dz * d, dx, dz };
      }
    }
    return null;
  };
  if (CROP) { const c = toIJ(0, 0); shots.push({ name: "crop", i: n >> 1, j: n >> 1, views: [] }); void c; }
  else for (const [name, isl, angDeg, kinds] of PRM.previews) {
    const I = world.islands.find((q) => q.name === isl); if (!I) continue;
    const sp = shore(E, I.x, I.z, angDeg * Math.PI / 180); if (!sp) continue;
    const [i, j] = toIJ(sp.x, sp.z);
    shots.push({ name, i, j, sp, views: kinds });
  }
  for (const sh of shots) {
    const sz = 640, a0 = sh.i - sz / 2, b0 = sh.j - sz / 2;
    const bef = hillshade(E, n, step, a0, b0, sz), aft = hillshade(H, n, step, a0, b0, sz);
    const cmp = new Uint8Array(sz * 2 * sz * 3);
    for (let y = 0; y < sz; y++) { cmp.set(bef.subarray(y * sz * 3, (y + 1) * sz * 3), y * sz * 6); cmp.set(aft.subarray(y * sz * 3, (y + 1) * sz * 3), y * sz * 6 + sz * 3); }
    writePNG(path.join(PREV, `${sh.name}_hillshade.png`), sz * 2, sz, 3, 8, cmp);
    for (const v of sh.views) {
      const { sp } = sh, far = v === "air" ? 820 : 360, hgt = v === "air" ? 240 : 16;
      const cam = { x: sp.x + sp.dx * far, y: hgt, z: sp.z + sp.dz * far, tx: sp.x - sp.dx * 100, ty: v === "air" ? 20 : 40, tz: sp.z - sp.dz * 100, fov: 55, sunAz: Math.atan2(sp.dz, sp.dx) + 0.95, sunEl: 0.42 };   // low sun across the face
      const W = 800, Hh = 450, cn = 760, ci0 = sh.i - cn / 2, cj0 = sh.j - cn / 2;
      const ims = [E, H].map((G) => renderOblique(G, n, step, ci0, cj0, cn, cam, W, Hh, oi, oj));
      const m = new Uint8Array(W * Hh * 2 * 3); m.set(ims[0], 0); m.set(ims[1], W * Hh * 3);
      writePNG(path.join(PREV, `${sh.name}_${v}.png`), W, Hh * 2, 3, 8, m);
    }
    sh.web = { x: Math.round(-S / 2 + (oi + sh.i) * step), z: Math.round(-S / 2 + (oj + sh.j) * step) };
  }
  log("previews written", shots.map((s) => `${s.name}@(${s.web.x},${s.web.z})`).join(" "));
  if (CROP || argv.includes("--no-write")) {
    if (CROP) { fs.writeFileSync(path.join(PREV, "crop_before.f32"), Buffer.from(E.buffer)); fs.writeFileSync(path.join(PREV, "crop_after.f32"), Buffer.from(H.buffer)); }
    for (const w of workers) w.terminate(); log("done (nothing written to the maps)"); process.exit(0);
  }

  // --- write heightmaps ---
  const U8 = new Uint16Array(N);
  for (let i = 0; i < N; i++) U8[i] = enc(H[i]);
  fs.writeFileSync(curOf(8129, "r16"), Buffer.from(U8.buffer));
  writePNG(curOf(8129, "png"), NF, NF, 1, 16, U8);
  log("wrote Heightmap_8129.r16/.png");
  // change weight: 1 within 3 cells of any change, fading over 4 more
  const Ws = sab(N * 4), W = new Float32Array(Ws);
  {
    const ch = new Float32Array(N); for (let i = 0; i < N; i++) ch[i] = Math.abs(H[i] - E[i]) > 0.02 ? 1 : 0;
    W.set(boxBlur(rankFilter(ch, n, 3, 1), n, 2));
    for (let i = 0; i < N; i++) W[i] = Math.min(1, W[i] * 1.5);
  }
  for (const [res, mode] of [[4033, "box"], ...(DO16 ? [[16321, "cubic"]] : [])]) {
    const ob = fs.readFileSync(erodedOf(res));
    if (ob.length !== res * res * 2) throw new Error(`${erodedOf(res)} size`);
    const Us = sab(res * res * 2); new Uint8Array(Us).set(ob);
    const rows = Math.ceil(res / (THREADS * 8)), jobs = [];
    for (let y = 0; y < res; y += rows) jobs.push({ op: "resample", H: Hs, W: Ws, U: Us, nf: NF, n: res, y0: y, y1: Math.min(res, y + rows), mode });
    await run(jobs);
    const R = new Uint16Array(Us), O = new Uint16Array(ob.buffer, ob.byteOffset, res * res);
    let fl = 0; for (let i = 0; i < res * res; i++) if ((O[i] > 32768) !== (R[i] > 32768)) fl++;
    stats[`coastCellsChanged_${res}`] = fl;
    fs.writeFileSync(curOf(res, "r16"), Buffer.from(R.buffer));
    writePNG(curOf(res, "png"), res, res, 1, 16, R);
    log(`wrote Heightmap_${res}.r16/.png (coast flips ${fl})`);
  }
  if (!DO16 && fs.existsSync(curOf(16321, "r16"))) log("note: Heightmap_16321 left as it was (pass --16k to regenerate it too)");

  // --- Masks2 slope (R), as bake.mjs / erode.mjs measure it ---
  const m2path = path.join(DIR, "Masks2_4096.png"), m2 = readPNG8(m2path), M = m2.w;
  const bil = (G, x, z) => {
    let fx = (x + S / 2) / step, fy = (z + S / 2) / step;
    if (fx < 0) fx = 0; if (fy < 0) fy = 0; if (fx > NF - 1.001) fx = NF - 1.001; if (fy > NF - 1.001) fy = NF - 1.001;
    const ix = fx | 0, iy = fy | 0, tx = fx - ix, ty = fy - iy, c = iy * NF + ix;
    return (G[c] * (1 - tx) + G[c + 1] * tx) * (1 - ty) + (G[c + NF] * (1 - tx) + G[c + NF + 1] * tx) * ty;
  };
  const M2 = new Uint8Array(M * M * 3), ms = S / M, eps = 6;
  const HM = new Float32Array(M * M), EM = new Float32Array(M * M), WM = new Float32Array(M * M);
  for (let y = 0; y < M; y++) {
    const z = -S / 2 + (y + 0.5) * ms;
    for (let x = 0; x < M; x++) {
      const wx = -S / 2 + (x + 0.5) * ms, k = y * M + x;
      M2[k * 3] = m2.data[k * m2.ch]; M2[k * 3 + 1] = m2.data[k * m2.ch + 1]; M2[k * 3 + 2] = m2.data[k * m2.ch + 2];
      HM[k] = bil(H, wx, z); EM[k] = bil(E, wx, z); WM[k] = bil(W, wx, z);
      if (WM[k] <= 0.001) continue;                           // unchanged: keep erode.mjs's value
      if (HM[k] < -2) { M2[k * 3] = 0; continue; }
      const nx = bil(H, wx - eps, z) - bil(H, wx + eps, z), nz = bil(H, wx, z - eps) - bil(H, wx, z + eps), ny = 2 * eps;
      const s = Math.min(1, (1 - ny / Math.hypot(nx, ny, nz)) * 2.6);
      M2[k * 3] = Math.max(0, Math.min(255, Math.round(s * 255)));
    }
  }
  writePNG(m2path, M, M, 3, 8, M2);
  log("wrote Masks2_4096.png (slope recomputed where the surface changed)");

  // --- Flow: erode.mjs's droplet flow + the change in MFD accumulation ---
  {
    const f0 = readPNG8(flowEroded);
    const aE = mfdAccum(EM, M), aH = mfdAccum(HM, M);
    const vals = []; for (let k = 0; k < M * M; k += 7) if (EM[k] > 0) vals.push(aE[k]);
    vals.sort((a, b) => a - b);
    const T = vals.length ? vals[Math.floor(vals.length * 0.999)] : 1, L = Math.log1p(T);
    const F = new Uint8Array(M * M);
    for (let k = 0; k < M * M; k++) {
      let v = f0.data[k * f0.ch];
      if (WM[k] > 0.001) v = HM[k] <= 0 ? 0 : v + WM[k] * 255 * (Math.log1p(aH[k]) - Math.log1p(aE[k])) / L;
      F[k] = Math.max(0, Math.min(255, Math.round(v)));
    }
    writePNG(path.join(DIR, "Flow_4096.png"), M, M, 1, 8, F);
    log("wrote Flow_4096.png");
  }

  // --- World.json note ---
  world.cliffs = {
    tool: "Tools/WorldBake/cliffs.mjs", source: path.basename(src8), params: PRM, stats,
    stacks: count.stackList,
    previews: shots.map((s) => ({ name: s.name, web: s.web })),
    seconds: Math.round((Date.now() - t0) / 1000),
  };
  fs.writeFileSync(path.join(DIR, "World.json"), JSON.stringify(world));
  for (const w of workers) w.terminate();
  log("done");
}
