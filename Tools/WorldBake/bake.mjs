#!/usr/bin/env node
// ---------------------------------------------------------------------------
// WorldBake — turns the web game's archipelago (terrain/*.js, copied verbatim
// from dragon-flying-sim) into data Unreal can import:
//
//   Heightmap_<N>.png     16-bit greyscale, N x N vertices, for the Landscape
//   Heightmap_<N>.r16     same, raw little-endian (UE's other import format)
//   Masks_<M>.png         RGBA8: R forest density, G vegetation (fertility),
//                         B glacier ice cover, A inland water (rivers/lakes)
//   Masks2_<M>.png        RGB8:  R slope, G firn (snow on ice), B village pad
//   Preview.png           1024px colour chart, for eyeballing the bake
//   World.json            islands (renamed), rivers, lakes, falls, glacier
//                         fronts, pads, village plots, spawn, and the exact
//                         height encoding so the importer can't get it wrong
//
// Coordinates: the web game is three.js (Y up, right-handed, metres). Unreal is
// Z up, left-handed, centimetres. We map  UE(X, Y, Z) = (x, z, y) * 100,
// a single axis swap, which flips handedness exactly once — so nothing is
// mirrored. Heightmap column i is +X (web x), row j is +Y (web z).
//
// Usage: node bake.mjs [--res 8129] [--mask 4096] [--out ../../Data/World]
//        [--threads N]
// ---------------------------------------------------------------------------
import { Worker, isMainThread, parentPort, workerData } from "node:worker_threads";
import { fileURLToPath } from "node:url";
import os from "node:os";
import fs from "node:fs";
import path from "node:path";
import zlib from "node:zlib";

const HERE = path.dirname(fileURLToPath(import.meta.url));

// Landscape Z scale (percent). Height in cm = (v - 32768) / 128 * ZSCALE.
// 400 gives +-1024 m of range at 3.125 cm per step; the world spans about
// -270 m (sea floor) to +945 m (Wyrmspire).
const ZSCALE = 400;
const encode = (hMetres) => {
  const v = Math.round((hMetres * 100) / ZSCALE * 128) + 32768;
  return v < 0 ? 0 : v > 65535 ? 65535 : v;
};

// The web game borrowed its names from a film series. This version is its own
// world, so every borrowed name is replaced; generic and mythological ones stay.
export const RENAME = {
  "Berk": "Hearthholm",
  "Raven Point": "Crowhead",
  "Dragon Peak": "Wyrmspire",
  "Fireworm Island": "Emberwick",
  "Dragon Hunter Island": "Trapper's Hold",
  "Freewing Rock": "Windfree Rock",
  "Berserker Island": "Wrathholm",
  "Wild Dragon Cliffs": "Wildwing Cliffs",
  "Breakneck Bog": "Mirefall Bog",
  "Thor's Beach": "Thunder Strand",
  "Hobblegrunt Island": "Mossback Isle",
  "Itchy Armpit": "Nettle Hollow",
  "Auction Island": "Marketstead",
  "Glacier Island": "Glacier Island",
  "Bog-Burglar Island": "Reedthief Isle",
  "Outcast Island": "Exile's Rock",
  "Impossible Island": "Vanishing Isle",
  "Sullen Sound": "Greywater Sound",
  "Healer Island": "Mender's Isle",
  "Scuttleclaw Island": "Clawshore",
  "Gronckle Isle": "Boulderback Isle",
  "Changewing Island": "Shifting Isle",
  "Death Song Island": "Sirenstone",
  "Melody Island": "Lullaby Isle",
  "Vanaheim": "Vanaheim",
  "Wingmaiden Island": "Shieldmaiden Isle",
  "Sandbuster Island": "Dunebreak Isle",
  "Peaceable Country": "Quietmoor",
  "Dark Deep": "Blackwater Deep",
  "Villainy Island": "Rogue's Isle",
  "Sheep Island": "Sheep Island",
  "Odin's Respite": "Odin's Rest",
  "Eel Isle": "Eel Isle",
};

// ---------------------------------------------------------------------------
// Worker: computes a band of rows.
// ---------------------------------------------------------------------------
if (!isMainThread) {
  const T = await import(path.join(HERE, "terrain", "terrain.js"));
  const S = T.TERRAIN_SIZE;
  parentPort.on("message", (job) => {
    if (job.kind === "height") {
      const { N, y0, y1 } = job;
      const out = new Uint16Array((y1 - y0) * N);
      const step = S / (N - 1);
      for (let j = y0; j < y1; j++) {
        const z = -S / 2 + j * step;
        const row = (j - y0) * N;
        for (let i = 0; i < N; i++) out[row + i] = encode(T.terrainHeight(-S / 2 + i * step, z));
      }
      parentPort.postMessage({ kind: "height", y0, y1, buf: out.buffer }, [out.buffer]);
    } else if (job.kind === "mask") {
      const { M, y0, y1 } = job;
      const a = new Uint8Array((y1 - y0) * M * 4);
      const b = new Uint8Array((y1 - y0) * M * 3);
      const step = S / M; // pixel centres, so the mask texture maps 0..1 over the square
      const b8 = (v) => Math.max(0, Math.min(255, Math.round(v * 255)));
      for (let j = y0; j < y1; j++) {
        const z = -S / 2 + (j + 0.5) * step;
        for (let i = 0; i < M; i++) {
          const x = -S / 2 + (i + 0.5) * step;
          const h = T.terrainHeight(x, z);
          const k = (j - y0) * M + i;
          if (h < T.SEA_LEVEL - 2) { // open sea: nothing to say, skip the expensive calls
            a[k * 4 + 3] = 0;
            continue;
          }
          const slope = T.terrainSlope(x, z);
          const fert = T.fertility(x, z, h, slope);
          const wood = T.woodland(x, z, h);
          const ice = T.iceAt(x, z, h);
          const wet = T.waterWet(x, z, h);
          a[k * 4 + 0] = b8(fert * wood);
          a[k * 4 + 1] = b8(fert);
          a[k * 4 + 2] = b8(ice ? ice.cover : 0);
          a[k * 4 + 3] = b8(wet);
          b[k * 3 + 0] = b8(slope);
          b[k * 3 + 1] = b8(ice ? ice.firn * Math.max(ice.cover, 0) : 0);
          b[k * 3 + 2] = b8(T.padWeight(x, z));
        }
      }
      parentPort.postMessage({ kind: "mask", y0, y1, a: a.buffer, b: b.buffer }, [a.buffer, b.buffer]);
    } else if (job.kind === "quit") {
      process.exit(0);
    }
  });
  parentPort.postMessage({ kind: "ready" });
}

// ---------------------------------------------------------------------------
// PNG writer (no dependencies).
// ---------------------------------------------------------------------------
const CRC = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();
function crc32(buf) {
  let c = 0xffffffff;
  for (let i = 0; i < buf.length; i++) c = CRC[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}
function chunk(type, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type, "ascii"), data]);
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td));
  return Buffer.concat([len, td, crc]);
}
// channels: 1 (grey), 3 (RGB), 4 (RGBA); depth 8 or 16. `pix` is row-major.
function writePNG(file, w, h, channels, depth, pix) {
  const colorType = { 1: 0, 3: 2, 4: 6 }[channels];
  const bpp = channels * (depth / 8);
  const raw = Buffer.alloc((w * bpp + 1) * h);
  for (let y = 0; y < h; y++) {
    const o = y * (w * bpp + 1);
    raw[o] = 0;
    if (depth === 8) {
      raw.set(pix.subarray(y * w * channels, (y + 1) * w * channels), o + 1);
    } else {
      for (let i = 0; i < w * channels; i++) raw.writeUInt16BE(pix[y * w * channels + i], o + 1 + i * 2);
    }
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4);
  ihdr[8] = depth; ihdr[9] = colorType; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  const idat = zlib.deflateSync(raw, { level: 6 });
  fs.writeFileSync(file, Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk("IHDR", ihdr), chunk("IDAT", idat), chunk("IEND", Buffer.alloc(0)),
  ]));
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
if (isMainThread) {
  const arg = (k, d) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : d; };
  const N = +arg("--res", 8129);
  const M = +arg("--mask", 4096);
  const OUT = path.resolve(arg("--out", path.join(HERE, "..", "..", "Data", "World")));
  const THREADS = +arg("--threads", Math.max(1, os.cpus().length - 1));
  const only = arg("--only", "all"); // all | height | mask | json
  fs.mkdirSync(OUT, { recursive: true });
  const t0 = Date.now();
  const log = (...a) => console.log(`[${((Date.now() - t0) / 1000).toFixed(1)}s]`, ...a);

  const T = await import(path.join(HERE, "terrain", "terrain.js"));
  const S = T.TERRAIN_SIZE;
  log(`world ${S} m square, ${T.ISLANDS.length} islands, ${THREADS} threads, out ${OUT}`);

  // --- pool ---
  const workers = await Promise.all(Array.from({ length: THREADS }, () => new Promise((res) => {
    const w = new Worker(fileURLToPath(import.meta.url));
    w.once("message", () => res(w));
  })));
  async function runBands(kind, rows, extra, onResult) {
    const band = Math.max(4, Math.ceil(rows / (THREADS * 16)));
    const jobs = [];
    for (let y = 0; y < rows; y += band) jobs.push({ kind, y0: y, y1: Math.min(rows, y + band), ...extra });
    let done = 0, next = 0;
    await Promise.all(workers.map((w) => new Promise((res) => {
      const feed = () => {
        if (next >= jobs.length) { w.off("message", onMsg); res(); return; }
        w.postMessage(jobs[next++]);
      };
      const onMsg = (m) => {
        onResult(m); done++;
        if (done % Math.max(1, Math.floor(jobs.length / 10)) === 0) log(`${kind} ${Math.round(done / jobs.length * 100)}%`);
        feed();
      };
      w.on("message", onMsg);
      feed();
    })));
  }

  if (only === "all" || only === "height") {
    const H = new Uint16Array(N * N);
    await runBands("height", N, { N }, (m) => H.set(new Uint16Array(m.buf), m.y0 * N));
    log("heights done; writing");
    fs.writeFileSync(path.join(OUT, `Heightmap_${N}.r16`), Buffer.from(H.buffer));
    writePNG(path.join(OUT, `Heightmap_${N}.png`), N, N, 1, 16, H);
    // Preview, from the heightmap itself.
    const P = 1024, prev = new Uint8Array(P * P * 3);
    for (let y = 0; y < P; y++) for (let x = 0; x < P; x++) {
      const v = H[Math.floor(y / P * N) * N + Math.floor(x / P * N)];
      const h = (v - 32768) * ZSCALE / 128 / 100;
      let c;
      if (h < 0) { const d = Math.min(1, -h / 200); c = [20 + 40 * (1 - d), 60 + 70 * (1 - d), 110 + 60 * (1 - d)]; }
      else if (h < 8) c = [205, 190, 150];
      else if (h < 300) { const t = h / 300; c = [70 + 60 * t, 110 + 20 * t, 55 + 40 * t]; }
      else if (h < 600) { const t = (h - 300) / 300; c = [130 + 30 * t, 125 + 30 * t, 110 + 40 * t]; }
      else c = [235, 238, 242];
      prev.set(c.map(Math.round), (y * P + x) * 3);
    }
    writePNG(path.join(OUT, "Preview.png"), P, P, 3, 8, prev);
    log(`wrote Heightmap_${N}.png/.r16 and Preview.png`);
  }

  if (only === "all" || only === "mask") {
    const A = new Uint8Array(M * M * 4), B = new Uint8Array(M * M * 3);
    await runBands("mask", M, { M }, (m) => {
      A.set(new Uint8Array(m.a), m.y0 * M * 4);
      B.set(new Uint8Array(m.b), m.y0 * M * 3);
    });
    writePNG(path.join(OUT, `Masks_${M}.png`), M, M, 4, 8, A);
    writePNG(path.join(OUT, `Masks2_${M}.png`), M, M, 3, 8, B);
    log(`wrote Masks_${M}.png and Masks2_${M}.png`);
  }

  for (const w of workers) w.postMessage({ kind: "quit" });

  if (only === "all" || only === "json") {
    // Everything in METRES and WEB axes (x, z horizontal, y up); the importer
    // converts with UE = (x, z, y) * 100. Rounded to 0.1 m to keep it small.
    const r1 = (v) => Math.round(v * 10) / 10;
    const W = T.inlandWater();
    const named = T.ISLANDS.filter((i) => i.name);
    const world = {
      version: 1,
      source: "dragon-flying-sim terrain.js",
      terrainSize: S,
      seaLevel: T.SEA_LEVEL,
      snowLine: T.SNOW_LINE,
      windBearing: T.WIND_BEARING,
      heightmap: { res: N, zScalePercent: ZSCALE, zeroValue: 32768,
        note: "height_cm = (v - 32768) / 128 * zScalePercent; vertex i spans x = -S/2 + i*S/(res-1)" },
      masks: { res: M, note: "pixel centres; uv (0,0) = web (-S/2,-S/2); R forest G veg B ice A water | R slope G firn B pad" },
      spawn: { x: r1(T.SPAWN_XZ.x), z: r1(T.SPAWN_XZ.z), y: r1(T.terrainHeight(T.SPAWN_XZ.x, T.SPAWN_XZ.z)) },
      home: { name: RENAME["Berk"], x: r1(T.BERK.x), z: r1(T.BERK.z) },
      islands: named.map((i) => ({
        name: RENAME[i.name] ?? i.name, x: r1(i.x), z: r1(i.z), r: r1(i.r), peak: r1(i.h),
        top: r1(T.terrainHeight(i.x, i.z)),
      })),
      rivers: (W?.rivers ?? []).map((rv) => ({
        pts: rv.pts.filter((_, k, a) => k % 3 === 0 || k === a.length - 1)
          .map((p) => [r1(p.x), r1(p.z), r1(p.y), r1(p.w)]),
      })),
      lakes: (W?.lakes ?? []).map((l) => {
        const o = {};
        for (const [k, v] of Object.entries(l)) if (typeof v === "number") o[k] = r1(v);
        return o;
      }),
      falls: (W?.falls ?? []).map((f) => {
        const o = {};
        for (const [k, v] of Object.entries(f)) if (typeof v === "number") o[k] = r1(v);
        return o;
      }),
      glacierFronts: T.glacierFronts().map((f) => ({ x: r1(f.x), z: r1(f.z), dx: r1(f.dx), dz: r1(f.dz), w: r1(f.w) })),
      pads: T.PADS.map((p) => ({ name: p.name, x: r1(p.x), z: r1(p.z), r: r1(p.r), h: r1(p.h) })),
      village: T.berkVillage().plots.map((p) => ({
        x: r1(p.x), z: r1(p.z), h: r1(p.h), yaw: Math.round(p.yaw * 1000) / 1000, kind: p.kind, s: p.s, r: r1(p.r), stilt: !!p.stilt,
      })),
    };
    fs.writeFileSync(path.join(OUT, "World.json"), JSON.stringify(world));
    log(`wrote World.json (${named.length} islands, ${world.rivers.length} rivers, ${world.lakes.length} lakes, ${world.village.length} plots)`);
  }
  log("done");
}
