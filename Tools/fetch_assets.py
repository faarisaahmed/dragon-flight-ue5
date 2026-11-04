#!/usr/bin/env python3
"""
Downloads the CC0 art the world is dressed with from Poly Haven (polyhaven.com,
no account needed) into Data/Downloads/. The Unreal builder
(DragonFlightEditor: BuildArchipelago) imports from there.

    python3 Tools/fetch_assets.py              # 4k textures, 2k models (default)
    python3 Tools/fetch_assets.py --tex 8k     # sharper ground on a big GPU
    python3 Tools/fetch_assets.py --only textures

Everything here is CC0 (public domain): no attribution required, but credit is
kept in Data/Downloads/CREDITS.txt anyway.
"""
import argparse
import hashlib
import json
import os
import sys
import urllib.request
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "Data", "Downloads")
UA = {"User-Agent": "dragon-flight-ue5 asset fetch"}

# Ground layers the landscape material blends. key -> Poly Haven texture id.
TEXTURES = {
    "Cliff": "seaside_rock",          # dark grey basalt-like sea cliff (was cliff_side: orange sandstone)
    "Scree": "rocky_terrain_02",
    "Grass": "leafy_grass",
    "Heath": "forrest_ground_01",   # darker moor turf and litter (sparse_grass was pale sand that bleached in sun)
    "Forest": "forest_floor",
    "Sand": "coast_sand_01",
    "Shingle": "coast_sand_rocks_02",
    "Snow": "snow_02",
    "Mud": "brown_mud_02",
    # Aerial scans for the far distance, where a 4 m tile would repeat visibly.
    "FarRock": "aerial_rocks_04",
    "FarGrass": "aerial_grass_rock",
    "FarSnow": "snow_field_aerial",
}
TEX_MAPS = ["diff", "nor_gl", "arm", "disp"]
# The API names two of the maps differently from the file suffixes.
API_KEY = {"diff": "Diffuse", "disp": "Displacement", "nor_gl": "nor_gl", "arm": "arm"}

# Models, by what the scatter uses them for.
MODELS = {
    "trees": ["fir_tree_01", "pine_tree_01", "fir_sapling_medium", "pine_sapling_medium", "fir_sapling"],
    "deadwood": ["dead_tree_trunk", "dead_tree_trunk_02", "tree_stump_01", "tree_stump_02"],
    "rocks": ["boulder_01", "rock_moss_set_01", "rock_moss_set_02", "rock_07", "rock_09", "stone_01",
              "coast_rocks_01", "coast_rocks_02", "coast_rocks_03", "coast_land_rocks_02", "coast_land_rocks_03"],
    "cliffs": ["coastal_cliff_01", "coastal_cliff_02", "coastal_cliff_04", "rock_face_01", "rock_face_02", "mountainside"],
    "ground": ["grass_medium_01", "grass_medium_02", "fern_02", "moss_01", "shrub_01", "shrub_02", "shrub_03",
               "shrub_04", "nettle_plant", "celandine_01", "dandelion_01", "pine_roots"],
}


def get_json(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=60) as r:
        return json.load(r)


def download(url, dest, md5=None):
    if os.path.exists(dest) and md5:
        with open(dest, "rb") as f:
            if hashlib.md5(f.read()).hexdigest() == md5:
                return False
    elif os.path.exists(dest) and os.path.getsize(dest) > 0:
        return False
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    tmp = dest + ".part"
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=300) as r, open(tmp, "wb") as f:
        while True:
            b = r.read(1 << 20)
            if not b:
                break
            f.write(b)
    os.replace(tmp, dest)
    return True


def texture_jobs(res):
    jobs = []
    for key, pid in TEXTURES.items():
        files = get_json(f"https://api.polyhaven.com/files/{pid}")
        for m in TEX_MAPS:
            k = API_KEY[m]
            entry = files.get(k, {}).get(res) or files.get(k, {}).get("4k")
            if not entry:
                print(f"  ! {pid}: no {m} at {res}")
                continue
            fmt = "png" if m in ("nor_gl", "disp") and "png" in entry else "jpg"
            e = entry.get(fmt) or entry.get("jpg") or next(iter(entry.values()))
            ext = os.path.splitext(e["url"])[1]
            jobs.append((e["url"], os.path.join(OUT, "Textures", key, f"T_{key}_{m}{ext}"), e.get("md5")))
    return jobs


def model_jobs(res):
    jobs = []
    for group, ids in MODELS.items():
        for pid in ids:
            try:
                files = get_json(f"https://api.polyhaven.com/files/{pid}")
            except Exception as ex:  # noqa: BLE001
                print(f"  ! {pid}: {ex}")
                continue
            g = files.get("gltf", {}).get(res) or files.get("gltf", {}).get("1k")
            if not g:
                print(f"  ! {pid}: no gltf")
                continue
            g = g["gltf"]
            base = os.path.join(OUT, "Models", group, pid)
            jobs.append((g["url"], os.path.join(base, os.path.basename(g["url"])), g.get("md5")))
            for rel, inc in g.get("include", {}).items():
                jobs.append((inc["url"], os.path.join(base, rel), inc.get("md5")))
            # Cut-out maps (leaves, grass blades) are separate from the JPG diffuse;
            # the Unreal builder makes masked foliage materials from them.
            for key, by_res in files.items():
                if not key.lower().endswith("alpha"):
                    continue
                e = (by_res.get(res) or by_res.get("1k") or {})
                e = e.get("png") or e.get("jpg")
                if e:
                    jobs.append((e["url"], os.path.join(base, "textures", os.path.basename(e["url"])), e.get("md5")))
    return jobs


def recentre_gltf(path):
    """Write <name>_ue.gltf with every node moved to the origin.

    Poly Haven packs variants (tree a, b, c...) side by side in one file. Unreal
    imports each node as its own mesh, and the scatter needs each one's pivot at
    its own base, not metres off to the side."""
    with open(path) as f:
        d = json.load(f)
    for n in d.get("nodes", []):
        if "translation" in n:
            t = n["translation"]
            n["translation"] = [0.0, t[1], 0.0]  # keep height (ground offset), drop the spread
    out = path.replace(".gltf", "_ue.gltf")
    with open(out, "w") as f:
        json.dump(d, f)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tex", default="4k", help="texture resolution: 2k, 4k or 8k")
    ap.add_argument("--models", default="2k", help="model texture resolution: 1k, 2k or 4k")
    ap.add_argument("--only", choices=["textures", "models"], default=None)
    ap.add_argument("--threads", type=int, default=8)
    a = ap.parse_args()

    jobs = []
    if a.only in (None, "textures"):
        print("Listing textures...")
        jobs += texture_jobs(a.tex)
    if a.only in (None, "models"):
        print("Listing models...")
        jobs += model_jobs(a.models)
    print(f"{len(jobs)} files")

    done = [0]

    def run(j):
        url, dest, md5 = j
        try:
            got = download(url, dest, md5)
        except Exception as ex:  # noqa: BLE001
            print(f"  ! {url}: {ex}")
            return
        done[0] += 1
        if got:
            print(f"  [{done[0]}/{len(jobs)}] {os.path.relpath(dest, OUT)}")

    with ThreadPoolExecutor(a.threads) as ex:
        list(ex.map(run, jobs))

    import glob
    for g in glob.glob(os.path.join(OUT, "Models", "*", "*", "*.gltf")):
        if not g.endswith("_ue.gltf"):
            recentre_gltf(g)

    with open(os.path.join(OUT, "CREDITS.txt"), "w") as f:
        f.write("All assets in this folder are CC0 from Poly Haven (https://polyhaven.com).\n\n")
        for k, v in TEXTURES.items():
            f.write(f"Texture {k}: https://polyhaven.com/a/{v}\n")
        for g, ids in MODELS.items():
            for i in ids:
                f.write(f"Model ({g}): https://polyhaven.com/a/{i}\n")
    print("Done.")


if __name__ == "__main__":
    sys.exit(main())
