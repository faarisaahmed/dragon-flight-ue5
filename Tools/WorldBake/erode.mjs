#!/usr/bin/env node
// ---------------------------------------------------------------------------
// erode.mjs — weathers the baked heightmap so the islands read as eroded land
// rather than smooth procedural dunes, without moving anything the game cares
// about (coastlines, lakes, rivers, village pads, peaks).
//
// Input : Data/World/Heightmap_8129_smooth.r16 (made from Heightmap_8129.r16 on
//         the first run, so re-running never erodes twice), Masks_4096.png,
//         Masks2_4096.png, World.json.
// Output: Heightmap_8129.r16/.png   eroded
//         Heightmap_4033.r16/.png   old smooth 4033 + the 8129 change, filtered
//         Heightmap_16321.r16/.png  old smooth 16321 + the 8129 change, bicubic
//                                   (only with --16k; no new detail below 5.5 m)
//         Masks2_4096.png           R (slope) recomputed from the eroded surface
//         Flow_4096.png             8-bit grey "flow / wetness" (see below)
//         ErosionPreview/*.png      hillshade before/after crops
//         World.json                gains an "erosion" block describing this run
//
// Pipeline (all on the 8129 grid unless noted):
//   0. seed     a few tenths of a metre of fine ridged noise on land, so water
//               has something to choose between on the perfectly smooth source.
//   1. hydraulic droplet erosion at three scales — 22 m, 11 m and 5.5 m cells.
//               Each droplet carries water and sediment downhill with inertia,
//               erodes with a small brush where it can carry more than it has,
//               and drops sediment where the slope eases (fans, valley fill).
//               The coarse passes cut valleys, the fine pass rills and gullies.
//               Each scale's change is upsampled and added to the full grid.
//               Work is split into tiles run in 4 parity phases, so no two
//               threads ever touch the same cells at once.
//   2. thermal  talus relaxation to ~36 deg on loose ground; cells that were
//               already cliff (> ~52 deg) are bedrock and shed only a trickle,
//               which is what builds scree aprons under them.
//   3. strata   on steep ground only: horizontal lava-flow layering (benches
//               and risers), layer thickness and level warped by noise, faded
//               out in patches, so cliffs read as stacked basalt.
//   4. limits   change blended out over rivers, lakes, pads and deep sea, soft
//               clamped, and then pinned: a land cell never goes below sea level
//               and a sea cell never comes above it; a lake cell stays below its
//               lake's level and dry ground in a lake's box stays above it.
//
// Flow_4096.png: per mask pixel (same pixel-centre convention as Masks_4096),
//   255 * log(1 + t) / log(1 + T), where t is the number of droplet paths that
//   crossed the pixel at the 11 m and 5.5 m passes (per unit rain) and T is the
//   99.9th percentile over land. 0 on open sea. High = gully floors, valley
//   bottoms, fans: use it for wet grass/moss, darker soil, stream decals.
//
// Usage: node erode.mjs [--dir ../../Data/World] [--threads N] [--16k]
//        [--crop i0 j0 size]   (dev: erode a square of the 8129 grid only and
//                               write previews of it to ErosionPreview/crop_*)
// ---------------------------------------------------------------------------
import { Worker, isMainThread, parentPort } from "node:worker_threads";
import { fileURLToPath } from "node:url";
import os from "node:os";
import fs from "node:fs";
import path from "node:path";
import zlib from "node:zlib";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const S = 45000;                // world size, metres
const ZSCALE = 400;
const enc = (m) => { const v = Math.round((m * 100) / ZSCALE * 128) + 32768; return v < 0 ? 0 : v > 65535 ? 65535 : v; };
const dec = (v) => (v - 32768) * ZSCALE / 128 / 100;

// ---------------------------------------------------------------------------
// Parameters. Heights in metres, slopes as tangents, distances in cells of the
// level they apply to. Tuned for 5.5 m cells and a wet, cool climate: a dense
// network of short steep gullies on every slope, few big fans, ground that
// stands steep (turf binds it) but scree under any crag.
// ---------------------------------------------------------------------------
export const PARAMS = {
  seed: { amp: 1.2, wavelength: 28 },               // metres
  levels: [
    // f: grid factor (cell = 5.54 m * f). density: droplets per land cell.
    // spl: stream-power incision on priority-flood drainage (iters, K, m, Ac m^2, kappa diffusion, fill of pits per iter)
    { f: 4, spl: { iters: 30, K: 0.006, m: 0.5, Ac: 20000, kappa: 0.0, fill: 0, smax: 0.8 }, density: 0 },
    { f: 2, spl: { iters: 20, K: 0.006, m: 0.5, Ac: 5000, kappa: 0.0, fill: 0, smax: 0.9 }, density: 0 },
    { f: 1, density: 6, life: 100, radius: 2, inertia: 0.3, Kc: 1.5, Ke: 0.25, Kd: 0.1, evap: 0.01, g: 2, friction: 0.03, minSlope: 0.02, pitFill: 0.5, tile: 256 },
  ],
  rounds: 8,                                         // phase sweeps per level
  thermal: { iters: 30, talus: 0.73, rate: 0.06, cliff0: 1.0, cliff1: 1.4, cliffK: 0.04 },
  strata: { slope0: 0.55, slope1: 1.0, thick: 11, thickVar: 5, strength: 0.75, power: 3, warp: 18 },
  clampUp: 10, clampDown: 30,                        // soft limits on fill / cut, m
  cutShare: 0.2,                                     // and a cut is at most 1 m + this share of the height above sea
  seaFade: [-14, -1],                                // change fades out below this depth
  pinMargin: 0.3,                                    // metres off the waterline when pinned
};

// ---------------------------------------------------------------------------
// Noise (shared by main and workers).
// ---------------------------------------------------------------------------
function hash2(ix, iy, seed) {
  let h = (Math.imul(ix, 0x27d4eb2d) ^ Math.imul(iy, 0x165667b1) ^ Math.imul(seed, 0x9e3779b1)) >>> 0;
  h = Math.imul(h ^ (h >>> 15), 0x85ebca6b);
  h = Math.imul(h ^ (h >>> 13), 0xc2b2ae35);
  return (h ^ (h >>> 16)) >>> 0;
}
const GX = new Float32Array(16), GY = new Float32Array(16);
for (let k = 0; k < 16; k++) { GX[k] = Math.cos(k * Math.PI / 8); GY[k] = Math.sin(k * Math.PI / 8); }
function gnoise(x, y, seed) {   // gradient noise, about -1..1
  const ix = Math.floor(x), iy = Math.floor(y), fx = x - ix, fy = y - iy;
  const u = fx * fx * fx * (fx * (fx * 6 - 15) + 10), v = fy * fy * fy * (fy * (fy * 6 - 15) + 10);
  const h00 = hash2(ix, iy, seed) & 15, h10 = hash2(ix + 1, iy, seed) & 15;
  const h01 = hash2(ix, iy + 1, seed) & 15, h11 = hash2(ix + 1, iy + 1, seed) & 15;
  const n00 = GX[h00] * fx + GY[h00] * fy, n10 = GX[h10] * (fx - 1) + GY[h10] * fy;
  const n01 = GX[h01] * fx + GY[h01] * (fy - 1), n11 = GX[h11] * (fx - 1) + GY[h11] * (fy - 1);
  const a = n00 + (n10 - n00) * u, b = n01 + (n11 - n01) * u;
  return (a + (b - a) * v) * 1.4;
}
function fbm(x, y, oct, seed) {
  let s = 0, a = 1, w = 0;
  for (let o = 0; o < oct; o++) { s += a * gnoise(x, y, seed + o * 101); w += a; a *= 0.5; x *= 2.03; y *= 2.03; }
  return s / w;
}
function ridged(x, y, oct, seed) {
  let s = 0, a = 1, w = 0;
  for (let o = 0; o < oct; o++) { s += a * (1 - Math.abs(gnoise(x, y, seed + o * 131))); w += a; a *= 0.5; x *= 2.07; y *= 2.07; }
  return s / w;   // 0..1, sharp crests
}
const smooth = (e0, e1, x) => { const t = Math.min(1, Math.max(0, (x - e0) / (e1 - e0))); return t * t * (3 - 2 * t); };

// ---------------------------------------------------------------------------
// Worker side.
// ---------------------------------------------------------------------------
if (!isMainThread) {
  parentPort.on("message", (job) => {
    const r = OPS[job.op](job);
    parentPort.postMessage(r ?? { ok: 1 });
  });
}

const OPS = {
  // Fine-grid preparation for a band of rows: erodibility E (0..255), thermal
  // hardness K (0..255) and the seed noise added to H.
  prep(j) {
    const H = new Float32Array(j.H), H0 = new Float32Array(j.H0), E = new Uint8Array(j.E), K = new Uint8Array(j.K);
    const P = new Uint8Array(j.P), ICE = new Uint8Array(j.ICE);
    const { n, oi, oj, step, y0, y1, prm } = j;
    for (let y = y0; y < y1; y++) for (let x = 0; x < n; x++) {
      const i = y * n + x, h = H0[i];
      if (h < -20) { E[i] = 0; K[i] = 0; H[i] = h; continue; }
      const wx = -S / 2 + (oi + x) * step, wz = -S / 2 + (oj + y) * step;
      // Original slope (tangent), central differences.
      const xa = x > 0 ? x - 1 : x, xb = x < n - 1 ? x + 1 : x, ya = y > 0 ? y - 1 : y, yb = y < n - 1 ? y + 1 : y;
      const gx = (H0[y * n + xb] - H0[y * n + xa]) / ((xb - xa) * step || 1);
      const gy = (H0[yb * n + x] - H0[ya * n + x]) / ((yb - ya) * step || 1);
      const sl = Math.hypot(gx, gy);
      const p = P[i] / 255, ice = ICE[i] / 255;
      // Lithology: patches of harder and softer rock at a few hundred metres.
      const lith = 0.55 + 0.45 * smooth(-0.5, 0.5, fbm(wx / 420, wz / 420, 3, 77));
      const cliff = smooth(prm.thermal.cliff0, prm.thermal.cliff1, sl);       // 0 soil .. 1 bedrock face
      const sea = h < 0 ? smooth(prm.seaFade[0], prm.seaFade[1], h) : 1;
      E[i] = Math.round(255 * p * sea * (1 - smooth(0, 0.4, ice)) * lith * (1 - 0.9 * cliff) * (1 - smooth(1.6, 2.5, sl)));
      K[i] = Math.round(255 * p * sea * (cliff * prm.thermal.cliffK + (1 - cliff)) * (0.7 + 0.3 * lith));
      if (h > 0.5 && p > 0) {
        const r = ridged(wx / prm.seed.wavelength, wz / prm.seed.wavelength, 3, 9) - 0.5;
        H[i] = h + r * prm.seed.amp * p * (1 - ice) * smooth(0.5, 3, h);
      } else H[i] = h;
    }
  },

  // One tile of droplets on a level grid.
  drops(j) {
    const H = new Float32Array(j.H), E = new Uint8Array(j.E), W = new Uint8Array(j.W);
    const F = j.F ? new Float32Array(j.F) : null;
    const { n, cell, tx, ty, T, count, p } = j;
    const inv = 1 / cell;
    const x0 = tx * T, y0 = ty * T;
    const half = (T >> 1) - p.radius - 2;
    const bx0 = Math.max(p.radius + 1, x0 - half), by0 = Math.max(p.radius + 1, y0 - half);
    const bx1 = Math.min(n - p.radius - 3, x0 + T + half), by1 = Math.min(n - p.radius - 3, y0 + T + half);
    // Brush.
    const R = p.radius, bo = [], bw = [];
    let ws = 0;
    for (let dy = -R; dy <= R; dy++) for (let dx = -R; dx <= R; dx++) {
      const d = Math.hypot(dx, dy);
      if (d <= R + 0.01) { const w = Math.max(0.15, R + 1 - d); bo.push(dy * n + dx); bw.push(w); ws += w; }
    }
    const BO = Int32Array.from(bo), BW = Float32Array.from(bw.map((w) => w / ws)), BN = BO.length;
    let s = (j.seed >>> 0) || 1;
    const rnd = () => { s = (s + 0x6d2b79f5) >>> 0; let t = s; t = Math.imul(t ^ (t >>> 15), t | 1); t ^= t + Math.imul(t ^ (t >>> 7), t | 61); return ((t ^ (t >>> 14)) >>> 0) / 4294967296; };
    const { inertia, Kc, Ke, Kd, evap, g, friction, minSlope, life } = p;
    let steps = 0;
    for (let k = 0; k < count; k++) {
      let x = x0 + rnd() * T, y = y0 + rnd() * T;
      if (x < bx0 || y < by0 || x >= bx1 || y >= by1) continue;
      let ix = x | 0, iy = y | 0, c = iy * n + ix;
      if (W[c] || E[c] < 3 || H[c] < 0.5) continue;
      let dx = 0, dy = 0, speed = 1, water = 1, sed = 0;
      for (let t = 0; t < life; t++) {
        const fx = x - ix, fy = y - iy;
        const a = H[c], b = H[c + 1], cc = H[c + n], d = H[c + n + 1];
        const gx = ((b - a) * (1 - fy) + (d - cc) * fy) * inv;
        const gy = ((cc - a) * (1 - fx) + (d - b) * fx) * inv;
        const h = a + (b - a) * fx + (cc - a) * fy + (a - b - cc + d) * fx * fy;
        dx = dx * inertia - gx * (1 - inertia);
        dy = dy * inertia - gy * (1 - inertia);
        const len = Math.hypot(dx, dy);
        if (len < 1e-6) break;
        dx /= len; dy /= len;
        const nx = x + dx, ny = y + dy;
        if (nx < bx0 || ny < by0 || nx >= bx1 || ny >= by1) break;
        const nix = nx | 0, niy = ny | 0, nc = niy * n + nix;
        const nfx = nx - nix, nfy = ny - niy;
        const a2 = H[nc], b2 = H[nc + 1], c2 = H[nc + n], d2 = H[nc + n + 1];
        const hn = a2 + (b2 - a2) * nfx + (c2 - a2) * nfy + (a2 - b2 - c2 + d2) * nfx * nfy;
        const dh = hn - h;
        if (F) F[c] += water;
        if (W[nc]) break;                         // river or lake: it carries the load away
        if (hn < 0) break;                        // reached the sea: the load goes offshore (the sea floor is left as baked)
        const slope = -dh * inv;
        const cap = Math.min(Math.max(slope, minSlope), 1.2) * speed * water * Kc;   // a waterfall carries no more than a steep chute
        if (dh > 0 || sed > cap) {
          // ran into a hollow (part-fill it and soak away) or slowed down (drop the excess).
          // No corner is built above the higher of here and the next step, so fill stays a floor.
          const q = dh > 0 ? Math.min(dh, sed) * p.pitFill : (sed - cap) * Kd;
          const lim = Math.min(h > hn ? h : hn, 0.25 * (a + b + cc + d) + 0.5);   // and none above its neighbours' mean
          let put = 0, v, w;
          w = q * (1 - fx) * (1 - fy); v = H[c];         if (v + w > lim) w = Math.max(0, lim - v); H[c] = v + w; put += w;
          w = q * fx * (1 - fy);       v = H[c + 1];     if (v + w > lim) w = Math.max(0, lim - v); H[c + 1] = v + w; put += w;
          w = q * (1 - fx) * fy;       v = H[c + n];     if (v + w > lim) w = Math.max(0, lim - v); H[c + n] = v + w; put += w;
          w = q * fx * fy;             v = H[c + n + 1]; if (v + w > lim) w = Math.max(0, lim - v); H[c + n + 1] = v + w; put += w;
          sed -= put;
          if (dh > 0) break;
        } else {
          const want = Math.min((cap - sed) * Ke, -dh);
          let got = 0;
          for (let m = 0; m < BN; m++) {
            const cm = c + BO[m];
            let q = want * BW[m] * E[cm] * (1 / 255);
            const room = H[cm] - hn;                // never dig a brush cell below where the water goes next
            if (q > room) q = room > 0 ? room : 0;
            H[cm] -= q; got += q;
          }
          sed += got;
        }
        speed = Math.sqrt(Math.max(0, speed * speed - dh * inv * g)) * (1 - friction);
        water *= 1 - evap;
        x = nx; y = ny; ix = nix; iy = niy; c = nc;
        steps++;
      }
    }
    return { steps };
  },

  // One Jacobi step of talus relaxation over a band (A -> B).
  thermal(j) {
    const A = new Float32Array(j.A), B = new Float32Array(j.B), K = new Uint8Array(j.K), H0 = new Float32Array(j.H0);
    const { n, y0, y1, step, prm } = j;
    const tc = prm.talus * step, td = prm.talus * step * Math.SQRT2, rate = prm.rate / 255;
    for (let y = y0; y < y1; y++) {
      const row = y * n;
      if (y === 0 || y === n - 1) { for (let x = 0; x < n; x++) B[row + x] = A[row + x]; continue; }
      B[row] = A[row]; B[row + n - 1] = A[row + n - 1];
      for (let x = 1; x < n - 1; x++) {
        const i = row + x, h = A[i];
        if (H0[i] < -20) { B[i] = h; continue; }
        const ki = K[i];
        let dlt = 0;
        // 4 cardinal + 4 diagonal neighbours; flux uses the higher cell's hardness.
        let o, e;
        o = i - 1; e = h - A[o]; if (e > tc) dlt -= ki * (e - tc); else if (-e > tc) dlt += K[o] * (-e - tc);
        o = i + 1; e = h - A[o]; if (e > tc) dlt -= ki * (e - tc); else if (-e > tc) dlt += K[o] * (-e - tc);
        o = i - n; e = h - A[o]; if (e > tc) dlt -= ki * (e - tc); else if (-e > tc) dlt += K[o] * (-e - tc);
        o = i + n; e = h - A[o]; if (e > tc) dlt -= ki * (e - tc); else if (-e > tc) dlt += K[o] * (-e - tc);
        o = i - n - 1; e = h - A[o]; if (e > td) dlt -= ki * (e - td); else if (-e > td) dlt += K[o] * (-e - td);
        o = i - n + 1; e = h - A[o]; if (e > td) dlt -= ki * (e - td); else if (-e > td) dlt += K[o] * (-e - td);
        o = i + n - 1; e = h - A[o]; if (e > td) dlt -= ki * (e - td); else if (-e > td) dlt += K[o] * (-e - td);
        o = i + n + 1; e = h - A[o]; if (e > td) dlt -= ki * (e - td); else if (-e > td) dlt += K[o] * (-e - td);
        B[i] = h + dlt * rate;
      }
    }
  },

  // Strata ledges on steep ground (A -> B).
  strata(j) {
    const A = new Float32Array(j.A), B = new Float32Array(j.B), P = new Uint8Array(j.P), ICE = new Uint8Array(j.ICE);
    const { n, oi, oj, step, y0, y1, prm } = j;
    for (let y = y0; y < y1; y++) for (let x = 0; x < n; x++) {
      const i = y * n + x, h = A[i];
      B[i] = h;
      if (h < 1 || x === 0 || y === 0 || x === n - 1 || y === n - 1 || P[i] === 0) continue;
      const gx = (A[i + 1] - A[i - 1]) / (2 * step), gy = (A[i + n] - A[i - n]) / (2 * step);
      const sl = Math.hypot(gx, gy);
      let w = smooth(prm.slope0, prm.slope1, sl);
      if (w <= 0) continue;
      const wx = -S / 2 + (oi + x) * step, wz = -S / 2 + (oj + y) * step;
      // Patchy: rock shows in bands and outcrops, not everywhere.
      w *= smooth(-0.35, 0.25, fbm(wx / 260, wz / 260, 3, 311)) * (P[i] / 255) * (1 - ICE[i] / 255);
      if (w <= 0) continue;
      // Layer thickness and level vary slowly; flows dip a little and lens out.
      const t = prm.thick + prm.thickVar * fbm(wx / 1500, wz / 1500, 2, 41);
      const hl = h + prm.warp * fbm(wx / 900, wz / 900, 3, 55) + 0.004 * wx - 0.002 * wz;
      const u = hl / t, f = u - Math.floor(u);
      const gfun = Math.pow(f, prm.power);            // long bench, short steep riser
      B[i] = h + w * prm.strength * t * (gfun - f);
    }
  },

  // Final: blend, soft clamp, pin to the waterline. Writes OUT (f32) and U (u16).
  finish(j) {
    const A = new Float32Array(j.A), H0 = new Float32Array(j.H0), P = new Uint8Array(j.P);
    const OUT = new Float32Array(j.OUT), U = new Uint16Array(j.U);
    const { n, y0, y1, prm } = j;
    const DU = prm.clampUp, DD = prm.clampDown, m = prm.pinMargin;
    for (let i = y0 * n; i < y1 * n; i++) {
      const h0 = H0[i];
      let d = A[i] - h0;
      const sea = h0 < 0 ? smooth(prm.seaFade[0], prm.seaFade[1], h0) : 1;
      d *= sea * (P[i] / 255);
      // Cuts are limited to a share of the height above the sea, so low coastal
      // ground and the foot of sea cliffs keep their profile (streams hang).
      const dd = h0 > 0 ? Math.min(DD, prm.cutShare * h0 + 1) : DD;
      d = d > 0 ? DU * Math.tanh(d / DU) : dd * Math.tanh(d / dd);
      let h = h0 + d;
      if (h0 > 0) { const lo = Math.min(h0, m); if (h < lo) h = lo; }
      else { const cap = h0 > -m ? h0 : -m; if (h > cap) h = cap; }
      OUT[i] = h; U[i] = enc(h);
    }
  },

  // Resample the 8129 change onto another grid and add to that grid's smooth
  // original, pinned to its waterline. (4033: box-filtered bilinear; 16321: bicubic)
  resample(j) {
    const DL = new Float32Array(j.DL), O = new Uint16Array(j.O), U = new Uint16Array(j.U);
    const { nf, n, y0, y1, mode, m } = j;
    const r = (nf - 1) / (n - 1);
    const at = (fx, fy) => {
      if (fx < 0) fx = 0; if (fy < 0) fy = 0; if (fx > nf - 1.001) fx = nf - 1.001; if (fy > nf - 1.001) fy = nf - 1.001;
      const ix = fx | 0, iy = fy | 0, tx = fx - ix, ty = fy - iy, c = iy * nf + ix;
      return (DL[c] * (1 - tx) + DL[c + 1] * tx) * (1 - ty) + (DL[c + nf] * (1 - tx) + DL[c + nf + 1] * tx) * ty;
    };
    const cr = (p0, p1, p2, p3, t) => p1 + 0.5 * t * (p2 - p0 + t * (2 * p0 - 5 * p1 + 4 * p2 - p3 + t * (3 * (p1 - p2) + p3 - p0)));
    const cub = (fx, fy) => {
      const ix = Math.floor(fx), iy = Math.floor(fy), tx = fx - ix, ty = fy - iy;
      const row = new Float64Array(4);
      for (let k = -1; k <= 2; k++) {
        const yy = Math.min(nf - 1, Math.max(0, iy + k)) * nf;
        const xa = Math.max(0, ix - 1), xb = ix, xc = Math.min(nf - 1, ix + 1), xd = Math.min(nf - 1, ix + 2);
        row[k + 1] = cr(DL[yy + xa], DL[yy + xb], DL[yy + xc], DL[yy + xd], tx);
      }
      return cr(row[0], row[1], row[2], row[3], ty);
    };
    const mv = Math.round(m * 100 / ZSCALE * 128);
    for (let y = y0; y < y1; y++) for (let x = 0; x < n; x++) {
      const i = y * n + x, v0 = O[i];
      const fx = x * r, fy = y * r;
      let d;
      if (mode === "box") d = 0.25 * (at(fx - 0.5, fy - 0.5) + at(fx + 0.5, fy - 0.5) + at(fx - 0.5, fy + 0.5) + at(fx + 0.5, fy + 0.5));
      else d = cub(fx, fy);
      let v = Math.round(v0 + d * 100 / ZSCALE * 128);
      if (v0 > 32768) { const lo = Math.min(v0, 32768 + mv); if (v < lo) v = lo; }
      else { const cap = v0 > 32768 - mv ? v0 : 32768 - mv; if (v > cap) v = cap; }
      U[i] = v < 0 ? 0 : v > 65535 ? 65535 : v;
    }
  },

  // Masks2 slope (R) from the eroded surface, exactly as bake.mjs measures it.
  slope(j) {
    const A = new Float32Array(j.A), H0 = new Float32Array(j.H0), M2 = new Uint8Array(j.M2);
    const { nf, M, y0, y1 } = j;
    const st = S / (nf - 1), ms = S / M;
    const bil = (G, x, z) => {
      let fx = (x + S / 2) / st, fy = (z + S / 2) / st;
      if (fx < 0) fx = 0; if (fy < 0) fy = 0; if (fx > nf - 1.001) fx = nf - 1.001; if (fy > nf - 1.001) fy = nf - 1.001;
      const ix = fx | 0, iy = fy | 0, tx = fx - ix, ty = fy - iy, c = iy * nf + ix;
      return (G[c] * (1 - tx) + G[c + 1] * tx) * (1 - ty) + (G[c + nf] * (1 - tx) + G[c + nf + 1] * tx) * ty;
    };
    const eps = 6;
    for (let y = y0; y < y1; y++) {
      const z = -S / 2 + (y + 0.5) * ms;
      for (let x = 0; x < M; x++) {
        const wx = -S / 2 + (x + 0.5) * ms, k = (y * M + x) * 3;
        if (bil(H0, wx, z) < -2) { M2[k] = 0; continue; }
        const nx = bil(A, wx - eps, z) - bil(A, wx + eps, z), nz = bil(A, wx, z - eps) - bil(A, wx, z + eps), ny = 2 * eps;
        const s = Math.min(1, (1 - ny / Math.hypot(nx, ny, nz)) * 2.6);
        M2[k] = Math.max(0, Math.min(255, Math.round(s * 255)));
      }
    }
  },
};


// ---------------------------------------------------------------------------
// Stream-power incision (main thread, coarse grids). Each iteration routes
// every land cell to the sea (or to a river/lake cell) by priority flood, so
// pits drain over their lowest saddle; accumulates drainage area down that
// tree; then lowers each cell toward its receiver with the implicit scheme
//   h_i <- (h_i + F h_r) / (1 + F),  F = K * E_i * (A_i^m - Ac^m)+ / dist
// which is unconditionally stable (Braun & Willett 2013). Pit cells are filled
// a little toward their spill level (sediment), and a light diffusion keeps
// channel walls from becoming one-cell slots.
// ---------------------------------------------------------------------------
function streamPower(L, E, W, n, cell, p) {
  const N = n * n;
  const Z = new Float32Array(N), rcv = new Int32Array(N), order = new Int32Array(N), A = new Float32Array(N);
  const closed = new Uint8Array(N);
  const hp = new Int32Array(N + 1), hv = new Float32Array(N + 1);
  let hn = 0;
  const push = (c, v) => {
    let k = ++hn;
    while (k > 1) { const q = k >> 1; if (hv[q] <= v) break; hp[k] = hp[q]; hv[k] = hv[q]; k = q; }
    hp[k] = c; hv[k] = v;
  };
  const pop = () => {
    const top = hp[1], lc = hp[hn], lv = hv[hn--];
    let k = 1;
    for (;;) {
      let m = k << 1; if (m > hn) break;
      if (m < hn && hv[m + 1] < hv[m]) m++;
      if (hv[m] >= lv) break;
      hp[k] = hp[m]; hv[k] = hv[m]; k = m;
    }
    hp[k] = lc; hv[k] = lv;
    return top;
  };
  const OFF = [-1, 1, -n, n, -n - 1, -n + 1, n - 1, n + 1];
  const DST = [1, 1, 1, 1, Math.SQRT2, Math.SQRT2, Math.SQRT2, Math.SQRT2].map((d) => d * cell);
  const cellA = cell * cell, Acm = Math.pow(p.Ac, p.m);
  const isOutlet = (c) => L[c] <= 0 || W[c] === 1;
  const lap = new Float32Array(N);
  for (let it = 0; it < p.iters; it++) {
    closed.fill(0); hn = 0;
    let cnt = 0;
    for (let y = 0; y < n; y++) for (let x = 0; x < n; x++) {
      const c = y * n + x;
      if (x === 0 || y === 0 || x === n - 1 || y === n - 1) { closed[c] = 1; rcv[c] = -1; Z[c] = L[c]; continue; }
      if (!isOutlet(c)) continue;
      closed[c] = 1; rcv[c] = -1; Z[c] = L[c];
      let edge = false;
      for (let k = 0; k < 8; k++) if (!isOutlet(c + OFF[k])) { edge = true; break; }
      if (edge) push(c, L[c]);
    }
    while (hn) {
      const c = pop(), zc = Z[c];
      if (rcv[c] >= 0 || !isOutlet(c)) order[cnt++] = c;
      for (let k = 0; k < 8; k++) {
        const o = c + OFF[k];
        if (closed[o]) continue;
        closed[o] = 1; rcv[o] = c;
        const z = L[o] > zc + 1e-3 ? L[o] : zc + 1e-3;
        Z[o] = z; push(o, z);
      }
    }
    for (let k = 0; k < cnt; k++) A[order[k]] = cellA;
    for (let k = cnt - 1; k >= 0; k--) { const c = order[k], r = rcv[c]; if (r >= 0 && !isOutlet(r)) A[r] += A[c]; }
    for (let k = 0; k < cnt; k++) {
      const c = order[k], r = rcv[c];
      const e = E[c] / 255; if (e <= 0) continue;
      const hr = L[r], h = L[c];
      if (h > hr) {
        let d = 0; for (let q = 0; q < 8; q++) if (c + OFF[q] === r) { d = DST[q]; break; }
        if ((h - hr) / d > p.smax) continue;       // a fall over a crag: streams hang, they don't saw it down
        const a = Math.pow(A[c], p.m) - Acm;
        if (a <= 0) continue;
        const F = p.K * e * a / d;
        L[c] = (h + F * hr) / (1 + F);
      } else if (Z[c] > h) L[c] = h + (Z[c] - h) * p.fill * e;
    }
    if (p.kappa > 0) {
      for (let y = 1; y < n - 1; y++) for (let x = 1; x < n - 1; x++) {
        const c = y * n + x; if (L[c] <= 0 || !E[c]) { lap[c] = 0; continue; }
        lap[c] = (L[c - 1] + L[c + 1] + L[c - n] + L[c + n] - 4 * L[c]) * p.kappa * E[c] / 255;
      }
      for (let c = 0; c < N; c++) L[c] += lap[c];
    }
  }
}

// ---------------------------------------------------------------------------
// PNG read/write (no dependencies).
// ---------------------------------------------------------------------------
const CRC = (() => { const t = new Uint32Array(256); for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; } return t; })();
function crc32(buf) { let c = 0xffffffff; for (let i = 0; i < buf.length; i++) c = CRC[(c ^ buf[i]) & 0xff] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; }
function chunk(type, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type, "ascii"), data]);
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td));
  return Buffer.concat([len, td, crc]);
}
function writePNG(file, w, h, channels, depth, pix) {
  const colorType = { 1: 0, 3: 2, 4: 6 }[channels];
  const bpp = channels * (depth / 8);
  const raw = Buffer.alloc((w * bpp + 1) * h);
  for (let y = 0; y < h; y++) {
    const o = y * (w * bpp + 1);
    raw[o] = 0;
    if (depth === 8) raw.set(pix.subarray(y * w * channels, (y + 1) * w * channels), o + 1);
    else for (let i = 0, b = y * w * channels; i < w * channels; i++) { const v = pix[b + i]; raw[o + 1 + i * 2] = v >> 8; raw[o + 2 + i * 2] = v & 255; }
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4);
  ihdr[8] = depth; ihdr[9] = colorType;
  const idat = zlib.deflateSync(raw, { level: 6 });
  fs.writeFileSync(file, Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk("IHDR", ihdr), chunk("IDAT", idat), chunk("IEND", Buffer.alloc(0))]));
}
function readPNG8(file) {
  const b = fs.readFileSync(file);
  let o = 8, w = 0, h = 0, ct = 0, depth = 0; const idat = [];
  while (o < b.length) {
    const len = b.readUInt32BE(o), type = b.toString("ascii", o + 4, o + 8), d = b.subarray(o + 8, o + 8 + len);
    if (type === "IHDR") { w = d.readUInt32BE(0); h = d.readUInt32BE(4); depth = d[8]; ct = d[9]; if (d[12]) throw new Error("interlaced PNG"); }
    else if (type === "IDAT") idat.push(d);
    else if (type === "IEND") break;
    o += 12 + len;
  }
  if (depth !== 8) throw new Error(`${file}: depth ${depth} unsupported`);
  const ch = { 0: 1, 2: 3, 4: 2, 6: 4 }[ct];
  const raw = zlib.inflateSync(Buffer.concat(idat));
  const stride = w * ch, out = new Uint8Array(stride * h);
  for (let y = 0; y < h; y++) {
    const f = raw[y * (stride + 1)], src = y * (stride + 1) + 1, dst = y * stride;
    for (let x = 0; x < stride; x++) {
      const a = x >= ch ? out[dst + x - ch] : 0, up = y ? out[dst - stride + x] : 0, ul = y && x >= ch ? out[dst - stride + x - ch] : 0;
      let v = raw[src + x];
      if (f === 1) v += a; else if (f === 2) v += up; else if (f === 3) v += (a + up) >> 1;
      else if (f === 4) { const p = a + up - ul, pa = Math.abs(p - a), pb = Math.abs(p - up), pc = Math.abs(p - ul); v += pa <= pb && pa <= pc ? a : pb <= pc ? up : ul; }
      out[dst + x] = v & 255;
    }
  }
  return { w, h, ch, data: out };
}

// ---------------------------------------------------------------------------
// Main.
// ---------------------------------------------------------------------------
if (isMainThread) {
  const argv = process.argv;
  const arg = (k, d) => { const i = argv.indexOf(k); return i > 0 ? argv[i + 1] : d; };
  const DIR = path.resolve(arg("--dir", path.join(HERE, "..", "..", "Data", "World")));
  const THREADS = +arg("--threads", Math.max(1, os.cpus().length));
  const DO16 = argv.includes("--16k");
  const ci = argv.indexOf("--crop");
  const CROP = ci > 0 ? { i0: +argv[ci + 1], j0: +argv[ci + 2], size: +argv[ci + 3] } : null;
  const PRM = PARAMS;
  const deepMerge = (a, b) => { for (const [k, v] of Object.entries(b)) { if (v && typeof v === "object" && !Array.isArray(v) && a[k] && typeof a[k] === "object") deepMerge(a[k], v); else a[k] = v; } };
  if (arg("--set")) deepMerge(PRM, JSON.parse(arg("--set")));
  const STAGES = arg("--stages", "seed,h0,h1,h2,thermal,strata").split(",");
  if (!STAGES.includes("seed")) PRM.seed.amp = 0;
  if (!STAGES.includes("thermal")) PRM.thermal.iters = 0;
  const NF = 8129, step = S / (NF - 1);
  const t0 = Date.now();
  const log = (...a) => console.log(`[${((Date.now() - t0) / 1000).toFixed(1)}s]`, ...a);
  const PREV = path.join(DIR, "ErosionPreview");
  fs.mkdirSync(PREV, { recursive: true });

  // --- backups / inputs ---
  const smoothOf = (res, ext) => path.join(DIR, `Heightmap_${res}_smooth.${ext}`);
  const curOf = (res, ext) => path.join(DIR, `Heightmap_${res}.${ext}`);
  if (!CROP) for (const res of [8129, 4033, ...(DO16 ? [16321] : [])]) for (const ext of ["r16", "png"]) {
    if (!fs.existsSync(smoothOf(res, ext))) {
      if (!fs.existsSync(curOf(res, ext))) throw new Error(`missing ${curOf(res, ext)}`);
      fs.copyFileSync(curOf(res, ext), smoothOf(res, ext));
      log(`backed up ${path.basename(curOf(res, ext))} -> ${path.basename(smoothOf(res, ext))}`);
    }
  }
  const src8 = fs.existsSync(smoothOf(8129, "r16")) ? smoothOf(8129, "r16") : curOf(8129, "r16");
  const raw8 = new Uint16Array(fs.readFileSync(src8).buffer.slice(0));
  if (raw8.length !== NF * NF) throw new Error("8129 size");
  const oi = CROP ? CROP.i0 : 0, oj = CROP ? CROP.j0 : 0, n = CROP ? CROP.size : NF;
  if ((n - 1) % 4) throw new Error("crop size must be 4k+1");
  const N2 = n * n;
  const sab = (bytes) => new SharedArrayBuffer(bytes);
  const H0s = sab(N2 * 4), Hs = sab(N2 * 4), Bs = sab(N2 * 4);
  const H0 = new Float32Array(H0s), H = new Float32Array(Hs);
  for (let y = 0; y < n; y++) for (let x = 0; x < n; x++) H0[y * n + x] = dec(raw8[(oj + y) * NF + oi + x]);
  if (argv.includes("--synth")) for (let y = 0; y < n; y++) for (let x = 0; x < n; x++) {
    const r = Math.hypot(x - n / 2, y - n / 2) * step; H0[y * n + x] = 250 * (1 - r / (n * step * 0.45)) + 8 * fbm(x / 60, y / 60, 3, 5);
  }
  log(`loaded ${path.basename(src8)}; grid ${n}^2 at (${oi},${oj}); ${THREADS} threads`);

  // --- masks -> protection P and ice at the fine grid ---
  const m1 = readPNG8(path.join(DIR, "Masks_4096.png"));
  const MR = m1.w;
  const m2path = path.join(DIR, "Masks2_4096.png");
  const m2 = readPNG8(m2path);
  // Dilate water a little (rivers get calm banks).
  const wat = new Float32Array(MR * MR), padm = new Float32Array(MR * MR), icem = new Float32Array(MR * MR);
  for (let k = 0; k < MR * MR; k++) { wat[k] = m1.data[k * 4 + 3] / 255; icem[k] = m1.data[k * 4 + 2] / 255; padm[k] = m2.data[k * m2.ch + 2] / 255; }
  const watD = new Float32Array(MR * MR);
  for (let y = 0; y < MR; y++) for (let x = 0; x < MR; x++) {
    let v = 0;
    for (let dy = -2; dy <= 2; dy++) for (let dx = -2; dx <= 2; dx++) {
      const xx = x + dx, yy = y + dy;
      if (xx < 0 || yy < 0 || xx >= MR || yy >= MR) continue;
      const w = wat[yy * MR + xx]; if (!w) continue;
      v = Math.max(v, w * (1 - Math.hypot(dx, dy) / 3.2));
    }
    watD[y * MR + x] = v;
  }
  const msamp = (G, wx, wz) => {
    let u = (wx + S / 2) / S * MR - 0.5, v = (wz + S / 2) / S * MR - 0.5;
    if (u < 0) u = 0; if (v < 0) v = 0; if (u > MR - 1.001) u = MR - 1.001; if (v > MR - 1.001) v = MR - 1.001;
    const iu = u | 0, iv = v | 0, tu = u - iu, tv = v - iv, c = iv * MR + iu;
    return (G[c] * (1 - tu) + G[c + 1] * tu) * (1 - tv) + (G[c + MR] * (1 - tu) + G[c + MR + 1] * tu) * tv;
  };
  const Ps = sab(N2), ICEs = sab(N2), Es = sab(N2), Ks = sab(N2), Wfs = sab(N2);
  const P = new Uint8Array(Ps), ICE = new Uint8Array(ICEs), WF = new Uint8Array(Wfs);
  const world = JSON.parse(fs.readFileSync(path.join(DIR, "World.json"), "utf8"));
  for (let y = 0; y < n; y++) {
    const wz = -S / 2 + (oj + y) * step;
    for (let x = 0; x < n; x++) {
      const i = y * n + x;
      if (H0[i] < -20) continue;
      const wx = -S / 2 + (oi + x) * step;
      if (argv.includes("--synth")) { P[i] = 255; continue; }
      const w = msamp(watD, wx, wz), pd = msamp(padm, wx, wz);
      const p = (1 - smooth(0.05, 0.6, w)) * (1 - smooth(0.02, 0.5, pd));
      P[i] = Math.round(255 * p);
      ICE[i] = Math.round(255 * msamp(icem, wx, wz));
      WF[i] = msamp(wat, wx, wz) > 0.35 ? 1 : 0;
    }
  }
  // Lakes: the basin below its level, plus a 2 m shore, is left alone.
  if (!argv.includes("--synth")) for (const L of world.lakes) {
    const ia = Math.max(0, Math.floor((L.x0 + S / 2) / step) - oi - 2), ib = Math.min(n - 1, Math.ceil((L.x0 + L.n * L.s + S / 2) / step) - oi + 2);
    const ja = Math.max(0, Math.floor((L.z0 + S / 2) / step) - oj - 2), jb = Math.min(n - 1, Math.ceil((L.z0 + L.n * L.s + S / 2) / step) - oj + 2);
    for (let y = ja; y <= jb; y++) for (let x = ia; x <= ib; x++) {
      const i = y * n + x;
      if (H0[i] < L.level) { P[i] = 0; WF[i] = 1; }
      else P[i] = Math.round(P[i] * smooth(L.level + 0.5, L.level + 4, H0[i]));
    }
  }
  log("masks and protection ready");

  // --- worker pool ---
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
  const bands = (rows, extra) => {
    const b = Math.max(8, Math.ceil(rows / (THREADS * 6))), out = [];
    for (let y = 0; y < rows; y += b) out.push({ ...extra, y0: y, y1: Math.min(rows, y + b) });
    return out;
  };

  // --- 0. prep / seed ---
  await run(bands(n, { op: "prep", H: Hs, H0: H0s, E: Es, K: Ks, P: Ps, ICE: ICEs, n, oi, oj, step, prm: PRM }));
  log("prep + seed noise done");

  // --- 1. hydraulic, coarse to fine ---
  const flowLevels = [];
  let totalSteps = 0;
  for (const [li, lv] of PRM.levels.entries()) {
    if (!STAGES.includes("h" + li)) continue;
    const f = lv.f, nl = (n - 1) / f + 1, cell = step * f;
    let Ls, Els, Wls;
    if (f === 1) { Ls = Hs; Els = Es; Wls = Wfs; }
    else {
      // tent-filtered downsample of the current surface; min-pooled erodibility; any-water stop map
      Ls = sab(nl * nl * 4); Els = sab(nl * nl); Wls = sab(nl * nl);
      const L = new Float32Array(Ls), EL = new Uint8Array(Els), WL = new Uint8Array(Wls), E = new Uint8Array(Es);
      const tmp = new Float32Array(n * nl);
      for (let y = 0; y < n; y++) for (let X = 0; X < nl; X++) {
        let s = 0, ws = 0;
        for (let k = -f + 1; k < f; k++) { const x = X * f + k; if (x < 0 || x >= n) continue; const w = f - Math.abs(k); s += H[y * n + x] * w; ws += w; }
        tmp[y * nl + X] = s / ws;
      }
      for (let Y = 0; Y < nl; Y++) for (let X = 0; X < nl; X++) {
        let s = 0, ws = 0;
        for (let k = -f + 1; k < f; k++) { const y = Y * f + k; if (y < 0 || y >= n) continue; const w = f - Math.abs(k); s += tmp[y * nl + X] * w; ws += w; }
        L[Y * nl + X] = s / ws;
        let e = 0, cnt = 0, wv = 0;
        for (let dy = 0; dy < f && Y * f + dy < n; dy++) for (let dx = 0; dx < f && X * f + dx < n; dx++) {
          const c = (Y * f + dy) * n + X * f + dx; e += E[c]; cnt++; wv |= WF[c];
        }
        EL[Y * nl + X] = Math.round(e / cnt); WL[Y * nl + X] = wv;
      }
    }
    const before = new Float32Array(new Float32Array(Ls));
    { let bad = 0; for (let k = 0; k < before.length; k++) if (!Number.isFinite(before[k])) bad++; if (bad) log(`level ${li}: ${bad} non-finite before`); }
    const Fls = lv.density > 0 ? sab(nl * nl * 4) : null;
    if (lv.spl) streamPower(new Float32Array(Ls), new Uint8Array(Els), new Uint8Array(Wls), nl, cell, lv.spl);
    // tiles
    const T = lv.tile, nt = Math.ceil(nl / T);
    const L = new Float32Array(Ls), EL = new Uint8Array(Els);
    const land = [];
    for (let ty = 0; ty < nt; ty++) for (let tx = 0; tx < nt; tx++) {
      let c = 0;
      for (let y = ty * T; y < Math.min(nl, ty * T + T); y++) for (let x = tx * T; x < Math.min(nl, tx * T + T); x++) {
        const i = y * nl + x; if (L[i] > 0.5 && EL[i] > 2) c++;
      }
      if (c) land.push({ tx, ty, c });
    }
    if (lv.density > 0) for (let r = 0; r < PRM.rounds; r++) for (let ph = 0; ph < 4; ph++) {
      const jobs = land.filter((t) => ((t.tx & 1) | ((t.ty & 1) << 1)) === ph).map((t) => ({
        op: "drops", H: Ls, E: Els, W: Wls, F: Fls, n: nl, cell, tx: t.tx, ty: t.ty, T,
        count: Math.round(lv.density * t.c / PRM.rounds), p: lv,
        seed: hash2(t.tx * 7919 + li * 104729, t.ty * 7907 + r * 15485863, 12345 + ph),
      }));
      const res = await run(jobs);
      for (const m of res) totalSteps += m.steps || 0;
    }
    if (f > 1) {
      // add the change to the fine grid (bilinear upsample)
      const Dl = new Float32Array(nl * nl);
      for (let k = 0; k < nl * nl; k++) Dl[k] = L[k] - before[k];
      for (let y = 0; y < n; y++) {
        const fy = y / f, iy = Math.min(nl - 2, fy | 0), ty = fy - iy;
        for (let x = 0; x < n; x++) {
          const fx = x / f, ix = Math.min(nl - 2, fx | 0), tx = fx - ix, c = iy * nl + ix;
          const d = (Dl[c] * (1 - tx) + Dl[c + 1] * tx) * (1 - ty) + (Dl[c + nl] * (1 - tx) + Dl[c + nl + 1] * tx) * ty;
          if (d) H[y * n + x] += d;
        }
      }
    }
    if (Fls) flowLevels.push({ F: new Float32Array(Fls), nl, f, density: lv.density });
    let mx = 0, mk = 0; for (let k = 0; k < nl * nl; k++) { const v = Math.abs(L[k] - before[k]); if (v > mx) { mx = v; mk = k; } }
    log(`  largest raw change ${(L[mk] - before[mk]).toFixed(1)} m at cell ${mk % nl},${(mk / nl) | 0} (was ${before[mk].toFixed(1)} m)`);
    log(`hydraulic level ${li} (${nl}^2, ${cell.toFixed(1)} m): ${land.length} tiles, max change ${mx.toFixed(1)} m`);
  }
  log(`droplet steps ${(totalSteps / 1e6).toFixed(1)} M`);

  // --- 2. thermal ---
  let A = Hs, B = Bs;
  for (let it = 0; it < PRM.thermal.iters; it++) {
    await run(bands(n, { op: "thermal", A, B, K: Ks, H0: H0s, n, step, prm: PRM.thermal }));
    [A, B] = [B, A];
  }
  log(`thermal x${PRM.thermal.iters} done`);

  // --- 3. strata ---
  if (STAGES.includes("strata")) await run(bands(n, { op: "strata", A, B, P: Ps, ICE: ICEs, n, oi, oj, step, prm: PRM.strata }));
  if (STAGES.includes("strata")) [A, B] = [B, A];
  log("strata done");

  // --- 4. finish ---
  const OUTs = sab(N2 * 4), Us = sab(N2 * 2);
  await run(bands(n, { op: "finish", A, H0: H0s, P: Ps, OUT: OUTs, U: Us, n, prm: PRM }));
  const OUT = new Float32Array(OUTs), U = new Uint16Array(Us);
  // lakes
  const mL = PRM.pinMargin;
  for (const L of world.lakes) {
    const ia = Math.max(0, Math.floor((L.x0 + S / 2) / step) - oi - 2), ib = Math.min(n - 1, Math.ceil((L.x0 + L.n * L.s + S / 2) / step) - oi + 2);
    const ja = Math.max(0, Math.floor((L.z0 + S / 2) / step) - oj - 2), jb = Math.min(n - 1, Math.ceil((L.z0 + L.n * L.s + S / 2) / step) - oj + 2);
    for (let y = ja; y <= jb; y++) for (let x = ia; x <= ib; x++) {
      const i = y * n + x, h0 = H0[i];
      let h = OUT[i];
      if (h0 < L.level) { const cap = h0 > L.level - mL ? h0 : L.level - mL; if (h > cap) h = cap; }
      else { const lo = h0 < L.level + mL ? h0 : L.level + mL; if (h < lo) h = lo; }
      if (h !== OUT[i]) { OUT[i] = h; U[i] = enc(h); }
    }
  }
  log("limits + pinning done");

  // --- stats ---
  let maxUp = 0, maxDn = 0, coastFlip = 0, land = 0, sumAbs = 0, over15 = 0;
  const hist = new Float64Array(200);
  for (let i = 0; i < N2; i++) {
    const h0 = H0[i], h = dec(U[i]), d = h - h0;
    if ((h0 > 0) !== (h > 0)) coastFlip++;
    if (h0 > 0) {
      land++; sumAbs += Math.abs(d); if (Math.abs(d) > 15) over15++;
      hist[Math.min(199, Math.floor(Math.abs(d) * 2))]++;
      if (d > maxUp) maxUp = d; if (d < maxDn) maxDn = d;
    }
  }
  const pct = (q) => { let acc = 0; for (let k = 0; k < 200; k++) { acc += hist[k]; if (acc >= q * land) return k / 2; } return 100; };
  const stats = { landCells: land, meanAbs: +(sumAbs / land).toFixed(2), p50: pct(0.5), p90: pct(0.9), p99: pct(0.99), p999: pct(0.999),
    maxUp: +maxUp.toFixed(2), maxDown: +maxDn.toFixed(2), over15mPct: +(over15 / land * 100).toFixed(2), coastCellsChanged: coastFlip };
  log("stats", JSON.stringify(stats));

  // --- previews ---
  const hill = (G, gn, ci0, cj0, sz) => {
    const img = new Uint8Array(sz * sz * 3);
    const lx = -Math.SQRT1_2 * Math.cos(Math.PI / 4), lz = -Math.SQRT1_2 * Math.sin(Math.PI / 4), ly = Math.SQRT1_2;
    for (let y = 0; y < sz; y++) for (let x = 0; x < sz; x++) {
      const X = Math.min(gn - 2, Math.max(1, ci0 + x)), Y = Math.min(gn - 2, Math.max(1, cj0 + y)), i = Y * gn + X;
      const gx = (G[i + 1] - G[i - 1]) / (2 * step), gz = (G[i + gn] - G[i - gn]) / (2 * step);
      const len = Math.hypot(gx, 1, gz), nx = -gx / len, ny = 1 / len, nz = -gz / len;
      let s = Math.max(0, nx * lx + ny * ly + nz * lz);
      s = 0.15 + 0.85 * s;
      const h = G[i];
      let c = h < 0 ? [70 * s + 20, 100 * s + 30, 150 * s + 40] : [235 * s, 230 * s, 220 * s];
      img.set(c.map((v) => Math.max(0, Math.min(255, Math.round(v)))), (y * sz + x) * 3);
    }
    return img;
  };
  const spots = [];
  const toIJ = (wx, wz) => [Math.round((wx + S / 2) / step) - oi, Math.round((wz + S / 2) / step) - oj];
  if (CROP) spots.push({ name: "crop", i: (n >> 1), j: (n >> 1) });
  else {
    const [hi, hj] = toIJ(0, -5850); spots.push({ name: "home_hearthholm", i: hi, j: hj });
    // steepest stretch of coast on the land the game names (cells just above the sea, steep)
    // the densest stretch of >50 m ground within 33 m of the sea (Wildwing Cliffs)
    const [ci, cj] = toIJ(-8681, -19311); spots.push({ name: "steep_coast_wildwing", i: ci, j: cj });
    const [mi, mj] = toIJ(-11925, -6750); spots.push({ name: "mountain_wyrmspire", i: mi, j: mj });
  }
  for (const sp of spots) {
    const sz = 512, a0 = sp.i - 256, b0 = sp.j - 256;
    const bef = hill(H0, n, a0, b0, sz), aft = hill(OUT, n, a0, b0, sz);
    writePNG(path.join(PREV, `${sp.name}_before.png`), sz, sz, 3, 8, bef);
    writePNG(path.join(PREV, `${sp.name}_after.png`), sz, sz, 3, 8, aft);
    const cmp = new Uint8Array(sz * 2 * sz * 3);
    for (let y = 0; y < sz; y++) { cmp.set(bef.subarray(y * sz * 3, (y + 1) * sz * 3), y * sz * 6); cmp.set(aft.subarray(y * sz * 3, (y + 1) * sz * 3), y * sz * 6 + sz * 3); }
    writePNG(path.join(PREV, `${sp.name}_compare.png`), sz * 2, sz, 3, 8, cmp);
    const wx = -S / 2 + (oi + sp.i) * step, wz = -S / 2 + (oj + sp.j) * step;
    sp.web = { x: Math.round(wx), z: Math.round(wz) };
  }
  log("previews written", spots.map((s) => `${s.name}@(${s.web.x},${s.web.z})`).join(" "));

  if (CROP || argv.includes("--no-write")) {
    const dump = argv.includes("--dump");
    if (dump) { fs.writeFileSync(path.join(PREV, "crop_h0.f32"), Buffer.from(H0.buffer)); fs.writeFileSync(path.join(PREV, "crop_h1.f32"), Buffer.from(OUT.buffer)); }
    for (const w of workers) w.terminate(); log("crop done"); process.exit(0);
  }

  // --- write heightmaps ---
  fs.writeFileSync(curOf(8129, "r16"), Buffer.from(U.buffer));
  writePNG(curOf(8129, "png"), NF, NF, 1, 16, U);
  log("wrote Heightmap_8129.r16/.png");
  const DLs = sab(N2 * 4); { const DL = new Float32Array(DLs); for (let i = 0; i < N2; i++) DL[i] = dec(U[i]) - H0[i]; }
  for (const [res, mode] of [[4033, "box"], ...(DO16 ? [[16321, "cubic"]] : [])]) {
    const ob = fs.readFileSync(smoothOf(res, "r16"));
    const Os = sab(res * res * 2); new Uint8Array(Os).set(ob);
    const Rs = sab(res * res * 2);
    await run(bands(res, { op: "resample", DL: DLs, O: Os, U: Rs, nf: NF, n: res, mode, m: PRM.pinMargin }));
    const R = new Uint16Array(Rs), O = new Uint16Array(Os), st = S / (res - 1), mv = Math.round(PRM.pinMargin * 100 / ZSCALE * 128);
    for (const L of world.lakes) {
      const lv = enc(L.level);
      const ia = Math.max(0, Math.floor((L.x0 + S / 2) / st) - 2), ib = Math.min(res - 1, Math.ceil((L.x0 + L.n * L.s + S / 2) / st) + 2);
      const ja = Math.max(0, Math.floor((L.z0 + S / 2) / st) - 2), jb = Math.min(res - 1, Math.ceil((L.z0 + L.n * L.s + S / 2) / st) + 2);
      for (let y = ja; y <= jb; y++) for (let x = ia; x <= ib; x++) {
        const i = y * res + x, v0 = O[i];
        if (v0 < lv) { const cap = v0 > lv - mv ? v0 : lv - mv; if (R[i] > cap) R[i] = cap; }
        else { const lo = v0 < lv + mv ? v0 : lv + mv; if (R[i] < lo) R[i] = lo; }
      }
    }
    let flips = 0, mx = 0;
    for (let i = 0; i < res * res; i++) { if ((O[i] > 32768) !== (R[i] > 32768)) flips++; mx = Math.max(mx, Math.abs(R[i] - O[i])); }
    fs.writeFileSync(curOf(res, "r16"), Buffer.from(R.buffer));
    writePNG(curOf(res, "png"), res, res, 1, 16, R);
    stats[`coastCellsChanged_${res}`] = flips; stats[`maxAbs_${res}`] = +dec(32768 + mx).toFixed(2);
    log(`wrote Heightmap_${res}.r16/.png (coast flips ${flips}, max |change| ${dec(32768 + mx).toFixed(1)} m)`);
  }

  // --- masks ---
  const M = MR, M2s = sab(M * M * 3); const M2 = new Uint8Array(M2s);
  for (let k = 0; k < M * M; k++) { M2[k * 3] = m2.data[k * m2.ch]; M2[k * 3 + 1] = m2.data[k * m2.ch + 1]; M2[k * 3 + 2] = m2.data[k * m2.ch + 2]; }
  await run(bands(M, { op: "slope", A: OUTs, H0: H0s, M2: M2s, nf: NF, M }));
  writePNG(m2path, M, M, 3, 8, M2);
  log("wrote Masks2_4096.png (slope recomputed)");

  // Flow: droplet traffic at 11 m and 5.5 m, per unit rain, on the mask grid.
  const flow = new Float32Array(M * M);
  for (const fl of flowLevels) {
    const { F, nl, f, density } = fl, st = step * f;
    for (let y = 0; y < M; y++) for (let x = 0; x < M; x++) {
      // sum the level cells that fall in this mask pixel
      const xa = Math.floor((x * S / M) / st), xb = Math.floor(((x + 1) * S / M) / st);
      const ya = Math.floor((y * S / M) / st), yb = Math.floor(((y + 1) * S / M) / st);
      let s = 0, c = 0;
      for (let yy = ya; yy <= Math.min(nl - 1, yb); yy++) for (let xx = xa; xx <= Math.min(nl - 1, xb); xx++) { s += F[yy * nl + xx]; c++; }
      // cells crossed per unit rain, scaled to the level's cell size (so levels agree)
      flow[y * M + x] = Math.max(flow[y * M + x], c ? (s / c) / density : 0);
    }
  }
  const vals = []; for (let k = 0; k < M * M; k += 7) if (flow[k] > 0) vals.push(flow[k]);
  vals.sort((a, b) => a - b);
  const T999 = vals.length ? vals[Math.floor(vals.length * 0.999)] : 1;
  const fimg = new Uint8Array(M * M);
  for (let k = 0; k < M * M; k++) fimg[k] = Math.min(255, Math.round(255 * Math.log1p(flow[k]) / Math.log1p(T999)));
  writePNG(path.join(DIR, "Flow_4096.png"), M, M, 1, 8, fimg);
  log(`wrote Flow_4096.png (T99.9 = ${T999.toFixed(1)})`);

  // --- World.json note ---
  world.erosion = {
    tool: "Tools/WorldBake/erode.mjs", source: "Heightmap_8129_smooth.r16", params: PRM, stats,
    flow: { file: "Flow_4096.png", note: "8-bit grey, Masks pixel-centre convention; 255*log(1+t)/log(1+T), t = droplet paths per unit rain crossing the pixel (11 m + 5.5 m passes), T = 99.9th percentile. High on gully floors, valley bottoms and fans." },
    previews: spots.map((s) => ({ name: s.name, web: s.web })),
    seconds: Math.round((Date.now() - t0) / 1000),
  };
  fs.writeFileSync(path.join(DIR, "World.json"), JSON.stringify(world));
  for (const w of workers) w.terminate();
  log("done");
}
