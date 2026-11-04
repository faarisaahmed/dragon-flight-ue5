// ---------------------------------------------------------------------------
// lib.mjs — helpers shared by the world bake tools (erode.mjs, cliffs.mjs):
// height encoding, value noise, dependency-free PNG I/O, hillshade and a small
// software rasteriser for oblique previews of a heightmap crop.
// ---------------------------------------------------------------------------
import fs from "node:fs";
import zlib from "node:zlib";

export const S = 45000;                // world size, metres
export const ZSCALE = 400;
export const enc = (m) => { const v = Math.round((m * 100) / ZSCALE * 128) + 32768; return v < 0 ? 0 : v > 65535 ? 65535 : v; };
export const dec = (v) => (v - 32768) * ZSCALE / 128 / 100;

// ---------------------------------------------------------------------------
// Noise.
// ---------------------------------------------------------------------------
export function hash2(ix, iy, seed) {
  let h = (Math.imul(ix, 0x27d4eb2d) ^ Math.imul(iy, 0x165667b1) ^ Math.imul(seed, 0x9e3779b1)) >>> 0;
  h = Math.imul(h ^ (h >>> 15), 0x85ebca6b);
  h = Math.imul(h ^ (h >>> 13), 0xc2b2ae35);
  return (h ^ (h >>> 16)) >>> 0;
}
const GX = new Float32Array(16), GY = new Float32Array(16);
for (let k = 0; k < 16; k++) { GX[k] = Math.cos(k * Math.PI / 8); GY[k] = Math.sin(k * Math.PI / 8); }
export function gnoise(x, y, seed) {   // gradient noise, about -1..1
  const ix = Math.floor(x), iy = Math.floor(y), fx = x - ix, fy = y - iy;
  const u = fx * fx * fx * (fx * (fx * 6 - 15) + 10), v = fy * fy * fy * (fy * (fy * 6 - 15) + 10);
  const h00 = hash2(ix, iy, seed) & 15, h10 = hash2(ix + 1, iy, seed) & 15;
  const h01 = hash2(ix, iy + 1, seed) & 15, h11 = hash2(ix + 1, iy + 1, seed) & 15;
  const n00 = GX[h00] * fx + GY[h00] * fy, n10 = GX[h10] * (fx - 1) + GY[h10] * fy;
  const n01 = GX[h01] * fx + GY[h01] * (fy - 1), n11 = GX[h11] * (fx - 1) + GY[h11] * (fy - 1);
  const a = n00 + (n10 - n00) * u, b = n01 + (n11 - n01) * u;
  return (a + (b - a) * v) * 1.4;
}
export function fbm(x, y, oct, seed) {
  let s = 0, a = 1, w = 0;
  for (let o = 0; o < oct; o++) { s += a * gnoise(x, y, seed + o * 101); w += a; a *= 0.5; x *= 2.03; y *= 2.03; }
  return s / w;
}
export function ridged(x, y, oct, seed) {
  let s = 0, a = 1, w = 0;
  for (let o = 0; o < oct; o++) { s += a * (1 - Math.abs(gnoise(x, y, seed + o * 131))); w += a; a *= 0.5; x *= 2.07; y *= 2.07; }
  return s / w;   // 0..1, sharp crests
}
export const smooth = (e0, e1, x) => { const t = Math.min(1, Math.max(0, (x - e0) / (e1 - e0))); return t * t * (3 - 2 * t); };

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
export function writePNG(file, w, h, channels, depth, pix) {
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
export function readPNG8(file) {
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
// Previews.
// ---------------------------------------------------------------------------
// Hillshade of a square crop of grid G (gn^2, cell `step` m), light from the NW.
export function hillshade(G, gn, step, ci0, cj0, sz) {
  const img = new Uint8Array(sz * sz * 3);
  const lx = -Math.SQRT1_2 * Math.cos(Math.PI / 4), lz = -Math.SQRT1_2 * Math.sin(Math.PI / 4), ly = Math.SQRT1_2;
  for (let y = 0; y < sz; y++) for (let x = 0; x < sz; x++) {
    const X = Math.min(gn - 2, Math.max(1, ci0 + x)), Y = Math.min(gn - 2, Math.max(1, cj0 + y)), i = Y * gn + X;
    const gx = (G[i + 1] - G[i - 1]) / (2 * step), gz = (G[i + gn] - G[i - gn]) / (2 * step);
    const len = Math.hypot(gx, 1, gz), nx = -gx / len, ny = 1 / len, nz = -gz / len;
    let s = Math.max(0, nx * lx + ny * ly + nz * lz);
    s = 0.15 + 0.85 * s;
    const h = G[i];
    const c = h < 0 ? [70 * s + 20, 100 * s + 30, 150 * s + 40] : [235 * s, 230 * s, 220 * s];
    img.set(c.map((v) => Math.max(0, Math.min(255, Math.round(v)))), (y * sz + x) * 3);
  }
  return img;
}

// Oblique view of a crop: the crop's quads are rasterised as two triangles each
// with interpolated vertex normals (as the UE landscape shades them), lit by a
// low sun with a shadow map, grass/rock by slope, a flat sea at 0 and haze.
//   G, gn, step : full grid; i0, j0, cn : crop origin and size in cells
//   cam = { x, y, z, tx, ty, tz, fov } in world metres (x east, y up, z = grid row axis)
export function renderOblique(G, gn, step, i0, j0, cn, cam, W = 960, Hh = 540, gi0 = 0, gj0 = 0) {
  // (gi0, gj0): 8129-grid cell of G's (0,0) when G is itself a crop
  const X0 = -S / 2 + (gi0 + i0) * step, Z0 = -S / 2 + (gj0 + j0) * step;
  const at = (i, j) => G[Math.min(gn - 1, Math.max(0, j0 + j)) * gn + Math.min(gn - 1, Math.max(0, i0 + i))];
  // vertex normals
  const N = cn * cn, nx = new Float32Array(N), ny = new Float32Array(N), nz = new Float32Array(N), hy = new Float32Array(N);
  for (let j = 0; j < cn; j++) for (let i = 0; i < cn; i++) {
    const k = j * cn + i, gx = (at(i + 1, j) - at(i - 1, j)) / (2 * step), gz = (at(i, j + 1) - at(i, j - 1)) / (2 * step);
    const l = Math.hypot(gx, 1, gz); nx[k] = -gx / l; ny[k] = 1 / l; nz[k] = -gz / l; hy[k] = at(i, j);
  }
  // camera basis
  let fx = cam.tx - cam.x, fy = cam.ty - cam.y, fz = cam.tz - cam.z; { const l = Math.hypot(fx, fy, fz); fx /= l; fy /= l; fz /= l; }
  // right = forward x world-up, up = right x forward
  let rx = -fz, rz = fx; { const l = Math.hypot(rx, rz); rx /= l; rz /= l; }
  const upx = -rz * fy, upy = rz * fx - rx * fz, upz = rx * fy;
  const foc = (W / 2) / Math.tan((cam.fov || 50) * Math.PI / 360);
  const sun = (() => { const az = cam.sunAz ?? 2.3, el = cam.sunEl ?? 0.45; return [Math.cos(az) * Math.cos(el), Math.sin(el), Math.sin(az) * Math.cos(el)]; })();
  // --- shadow map (orthographic along the sun) ---
  const SM = 2048;
  let s1x = -sun[2], s1z = sun[0]; { const l = Math.hypot(s1x, s1z); s1x /= l; s1z /= l; }
  // shadow-map axes: s1 horizontal and perpendicular to the sun, t2 = sun x s1
  const t2x = sun[1] * s1z, t2y = sun[2] * s1x - sun[0] * s1z, t2z = -sun[1] * s1x;
  let amin = 1e9, amax = -1e9, bmin = 1e9, bmax = -1e9;
  for (let j = 0; j < cn; j += 8) for (let i = 0; i < cn; i += 8) {
    const wx = X0 + i * step, wz = Z0 + j * step, wy = hy[j * cn + i];
    const a = wx * s1x + wz * s1z, b = wx * t2x + wy * t2y + wz * t2z;
    amin = Math.min(amin, a); amax = Math.max(amax, a); bmin = Math.min(bmin, b - 50); bmax = Math.max(bmax, b + 1200);
  }
  const sa = (SM - 1) / (amax - amin), sb = (SM - 1) / (bmax - bmin);
  const SD = new Float32Array(SM * SM).fill(-1e30);   // largest "towards sun" coordinate
  const proj = new Float32Array(N * 3);
  for (let k = 0; k < N; k++) {
    const i = k % cn, j = (k / cn) | 0, wx = X0 + i * step, wz = Z0 + j * step, wy = hy[k];
    proj[k * 3] = (wx * s1x + wz * s1z - amin) * sa; proj[k * 3 + 1] = (wx * t2x + wy * t2y + wz * t2z - bmin) * sb;
    proj[k * 3 + 2] = wx * sun[0] + wy * sun[1] + wz * sun[2];
  }
  const rasterSM = (a, b, c) => {
    const ax = proj[a * 3], ay = proj[a * 3 + 1], bx = proj[b * 3], by = proj[b * 3 + 1], cx = proj[c * 3], cy = proj[c * 3 + 1];
    const area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax); if (Math.abs(area) < 1e-9) return;
    const xa = Math.max(0, Math.floor(Math.min(ax, bx, cx))), xb = Math.min(SM - 1, Math.ceil(Math.max(ax, bx, cx)));
    const ya = Math.max(0, Math.floor(Math.min(ay, by, cy))), yb = Math.min(SM - 1, Math.ceil(Math.max(ay, by, cy)));
    for (let y = ya; y <= yb; y++) for (let x = xa; x <= xb; x++) {
      const w0 = ((bx - x) * (cy - y) - (by - y) * (cx - x)) / area, w1 = ((cx - x) * (ay - y) - (cy - y) * (ax - x)) / area, w2 = 1 - w0 - w1;
      if (w0 < -1e-4 || w1 < -1e-4 || w2 < -1e-4) continue;
      const d = w0 * proj[a * 3 + 2] + w1 * proj[b * 3 + 2] + w2 * proj[c * 3 + 2];
      if (d > SD[y * SM + x]) SD[y * SM + x] = d;
    }
  };
  for (let j = 0; j < cn - 1; j++) for (let i = 0; i < cn - 1; i++) { const k = j * cn + i; rasterSM(k, k + 1, k + cn + 1); rasterSM(k, k + cn + 1, k + cn); }
  // --- main view ---
  const ZB = new Float32Array(W * Hh).fill(1e30);
  const PX = new Float32Array(W * Hh * 6);   // world pos + normal
  const vs = new Float32Array(N * 3);
  for (let k = 0; k < N; k++) {
    const i = k % cn, j = (k / cn) | 0, dx = X0 + i * step - cam.x, dy = hy[k] - cam.y, dz = Z0 + j * step - cam.z;
    const zc = dx * fx + dy * fy + dz * fz, xc = dx * rx + dz * rz, yc = dx * upx + dy * upy + dz * upz;
    vs[k * 3 + 2] = zc;
    vs[k * 3] = zc > 0.5 ? W / 2 + foc * xc / zc : NaN; vs[k * 3 + 1] = zc > 0.5 ? Hh / 2 - foc * yc / zc : NaN;
  }
  const raster = (a, b, c) => {
    const ax = vs[a * 3], ay = vs[a * 3 + 1], bx = vs[b * 3], by = vs[b * 3 + 1], cx = vs[c * 3], cy = vs[c * 3 + 1];
    if (!(ax === ax && bx === bx && cx === cx)) return;
    const area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax); if (Math.abs(area) < 1e-9) return;
    const xa = Math.max(0, Math.floor(Math.min(ax, bx, cx))), xb = Math.min(W - 1, Math.ceil(Math.max(ax, bx, cx)));
    const ya = Math.max(0, Math.floor(Math.min(ay, by, cy))), yb = Math.min(Hh - 1, Math.ceil(Math.max(ay, by, cy)));
    if (xa > xb || ya > yb) return;
    const iza = 1 / vs[a * 3 + 2], izb = 1 / vs[b * 3 + 2], izc = 1 / vs[c * 3 + 2];
    for (let y = ya; y <= yb; y++) for (let x = xa; x <= xb; x++) {
      const px = x + 0.5, py = y + 0.5;
      let w0 = ((bx - px) * (cy - py) - (by - py) * (cx - px)) / area, w1 = ((cx - px) * (ay - py) - (cy - py) * (ax - px)) / area, w2 = 1 - w0 - w1;
      if (w0 < 0 || w1 < 0 || w2 < 0) continue;
      const iz = w0 * iza + w1 * izb + w2 * izc, z = 1 / iz;
      const p = y * W + x; if (z >= ZB[p]) continue;
      ZB[p] = z;
      w0 *= iza * z; w1 *= izb * z; w2 *= izc * z;
      const o = p * 6;
      const ia = a % cn, ja = (a / cn) | 0, ib = b % cn, jb = (b / cn) | 0, ic = c % cn, jc = (c / cn) | 0;
      PX[o] = X0 + (w0 * ia + w1 * ib + w2 * ic) * step; PX[o + 2] = Z0 + (w0 * ja + w1 * jb + w2 * jc) * step;
      PX[o + 1] = w0 * hy[a] + w1 * hy[b] + w2 * hy[c];
      PX[o + 3] = w0 * nx[a] + w1 * nx[b] + w2 * nx[c]; PX[o + 4] = w0 * ny[a] + w1 * ny[b] + w2 * ny[c]; PX[o + 5] = w0 * nz[a] + w1 * nz[b] + w2 * nz[c];
    }
  };
  for (let j = 0; j < cn - 1; j++) for (let i = 0; i < cn - 1; i++) { const k = j * cn + i; raster(k, k + 1, k + cn + 1); raster(k, k + cn + 1, k + cn); }
  const img = new Uint8Array(W * Hh * 3);
  const haze = [178, 196, 214];
  for (let y = 0; y < Hh; y++) for (let x = 0; x < W; x++) {
    const p = y * W + x;
    // view ray
    const sx = (x + 0.5 - W / 2) / foc, sy = -(y + 0.5 - Hh / 2) / foc;
    let dx = fx + rx * sx + upx * sy, dy = fy + upy * sy, dz = fz + rz * sx + upz * sy; { const l = Math.hypot(dx, dy, dz); dx /= l; dy /= l; dz /= l; }
    const tSea = dy < 0 ? -cam.y / dy : 1e30;
    let col;
    let dist;
    if (ZB[p] < 1e29) {
      const o = p * 6, wx = PX[o], wy = PX[o + 1], wz = PX[o + 2];
      let ux = PX[o + 3], uy = PX[o + 4], uz = PX[o + 5]; { const l = Math.hypot(ux, uy, uz); ux /= l; uy /= l; uz /= l; }
      // shadow
      const a = (wx * s1x + wz * s1z - amin) * sa, b = (wx * t2x + wy * t2y + wz * t2z - bmin) * sb;
      const d = wx * sun[0] + wy * sun[1] + wz * sun[2];
      const ndl = Math.max(0, ux * sun[0] + uy * sun[1] + uz * sun[2]);
      const bias = 1 + 3 / sa * (1 + 3 * Math.sqrt(Math.max(0, 1 - ndl * ndl)) / Math.max(0.15, ndl));
      let lit = 1;
      if (a >= 0 && b >= 0 && a < SM && b < SM) {
        let sh = 0;
        for (let q = 0; q < 4; q++) { const aa = Math.min(SM - 1, (a + (q & 1)) | 0), bb = Math.min(SM - 1, (b + (q >> 1)) | 0); if (SD[bb * SM + aa] > d + bias) sh++; }
        lit = 1 - sh / 4;
      }
      const rock = smooth(0.22, 0.36, 1 - uy);   // ~ 39-50 deg, about where Masks2 slope saturates
      const nn = 0.5 + 0.5 * gnoise(wx / 23, wz / 23, 3);
      const grass = [92 + 30 * nn, 104 + 20 * nn, 52], stone = [92, 86, 78];
      const beach = smooth(2.5, 0.5, wy);
      let alb = [0, 1, 2].map((c) => grass[c] * (1 - rock) + stone[c] * rock);
      alb = alb.map((v, c) => v * (1 - beach) + [70, 66, 62][c] * beach);
      const amb = 0.32 + 0.18 * uy;
      col = alb.map((v) => v / 255 * (amb + 1.25 * ndl * lit));
      dist = ZB[p];
      if (tSea < dist && wy < 0) {
        const depth = (dist - tSea) * -dy;     // vertical-ish water column above the point
        const tr = Math.exp(-depth / 3);
        const sea = [0.13, 0.25, 0.33];
        col = col.map((v, c) => v * tr + sea[c] * (1 - tr));
        dist = tSea;
      }
    } else if (tSea < 1e29) { col = [0.13, 0.25, 0.33]; dist = tSea; }
    else { const t = Math.max(0, dy); col = [0.62 - 0.3 * t, 0.72 - 0.25 * t, 0.85 - 0.1 * t]; dist = 0; }
    if (dist > 0 && tSea <= dist + 1e-3 && ZB[p] >= 1e29 || (dist === tSea && dist > 0)) {
      // sea surface: fresnel-ish sky reflection
      const f = 0.25 + 0.6 * Math.pow(1 - Math.abs(dy), 5);
      col = col.map((v, c) => v * (1 - f) + [0.62, 0.72, 0.85][c] * f);
    }
    const fog = dist > 0 ? 1 - Math.exp(-dist / 9000) : 0;
    for (let c = 0; c < 3; c++) {
      let v = col[c] * (1 - fog) + haze[c] / 255 * fog;
      v = Math.pow(Math.max(0, v), 1 / 1.6);
      img[p * 3 + c] = Math.max(0, Math.min(255, Math.round(v * 255)));
    }
  }
  return img;
}

// ---------------------------------------------------------------------------
// Protection, as erode.mjs defines it, on an n x n window of the 8129 grid
// whose (0,0) is cell (oi, oj): P = 255 where the ground may change, fading to
// 0 over rivers and lakes (water mask, dilated a little) and village/pad areas
// (Masks2 B); lake basins below their level are 0 and their dry shores fade in
// over 0.5-4 m above it. Also ICE (Masks B) and WF (1 = water cell).
// Pass `buffer` (a function bytes -> ArrayBuffer-like) to place the arrays in
// shared memory.
// ---------------------------------------------------------------------------
export function buildProtection({ dir, H0, n, oi, oj, step, world, synth = false, buffer = (b) => new ArrayBuffer(b) }) {
  const m1 = readPNG8(`${dir}/Masks_4096.png`), m2 = readPNG8(`${dir}/Masks2_4096.png`);
  const MR = m1.w;
  const wat = new Float32Array(MR * MR), padm = new Float32Array(MR * MR), icem = new Float32Array(MR * MR);
  for (let k = 0; k < MR * MR; k++) { wat[k] = m1.data[k * 4 + 3] / 255; icem[k] = m1.data[k * 4 + 2] / 255; padm[k] = m2.data[k * m2.ch + 2] / 255; }
  // Dilate water a little (rivers get calm banks).
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
  const N2 = n * n;
  const P = new Uint8Array(buffer(N2)), ICE = new Uint8Array(buffer(N2)), WF = new Uint8Array(buffer(N2));
  for (let y = 0; y < n; y++) {
    const wz = -S / 2 + (oj + y) * step;
    for (let x = 0; x < n; x++) {
      const i = y * n + x;
      if (H0[i] < -20) continue;
      const wx = -S / 2 + (oi + x) * step;
      if (synth) { P[i] = 255; continue; }
      const w = msamp(watD, wx, wz), pd = msamp(padm, wx, wz);
      const p = (1 - smooth(0.05, 0.6, w)) * (1 - smooth(0.02, 0.5, pd));
      P[i] = Math.round(255 * p);
      ICE[i] = Math.round(255 * msamp(icem, wx, wz));
      WF[i] = msamp(wat, wx, wz) > 0.35 ? 1 : 0;
    }
  }
  // Lakes: the basin below its level, plus a 2 m shore, is left alone.
  if (!synth) for (const L of world.lakes) {
    const ia = Math.max(0, Math.floor((L.x0 + S / 2) / step) - oi - 2), ib = Math.min(n - 1, Math.ceil((L.x0 + L.n * L.s + S / 2) / step) - oi + 2);
    const ja = Math.max(0, Math.floor((L.z0 + S / 2) / step) - oj - 2), jb = Math.min(n - 1, Math.ceil((L.z0 + L.n * L.s + S / 2) / step) - oj + 2);
    for (let y = ja; y <= jb; y++) for (let x = ia; x <= ib; x++) {
      const i = y * n + x;
      if (H0[i] < L.level) { P[i] = 0; WF[i] = 1; }
      else P[i] = Math.round(P[i] * smooth(L.level + 0.5, L.level + 4, H0[i]));
    }
  }
  return { P, ICE, WF, m1, m2 };
}
