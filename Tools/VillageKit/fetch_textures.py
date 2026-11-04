#!/usr/bin/env python3
"""
Downloads the CC0 Poly Haven scans the village kit is textured with into
Data/Downloads/VillageTextures/ (gitignored). Safe to re-run: files already on
disk are skipped.

    python3 Tools/VillageKit/fetch_textures.py [--res 2k]

build_kit.py calls this itself when a texture is missing.
"""
import json
import os
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "Data", "Downloads", "VillageTextures")
UA = {"User-Agent": "dragon-flight-ue5 village kit"}

# kit key -> (Poly Haven id, real-world tile size in metres)
TEXTURES = {
    "wood": ("rough_wood", 0.5),          # weathered board, grain runs along V
    "stone": ("mossy_rock", 3.0),         # lichen/moss rock surface for drystone
    "turf": ("forrest_ground_01", 2.0),   # mossy turf and litter under the grass blades
    "ground": ("leafy_grass", 2.0),       # preview ground only (not exported)
}
HDRI = "kloofendal_48d_partly_cloudy_puresky"  # midday, broken cloud, sun ~48 deg
MAPS = {"diff": "Diffuse", "nor_gl": "nor_gl", "arm": "arm"}


def _get(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=60) as r:
        return json.load(r)


def _download(url, dest):
    if os.path.exists(dest) and os.path.getsize(dest) > 0:
        return
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    print("  fetch", os.path.relpath(dest, ROOT), flush=True)
    tmp = dest + ".part"
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=300) as r, open(tmp, "wb") as f:
        while True:
            b = r.read(1 << 20)
            if not b:
                break
            f.write(b)
    os.replace(tmp, dest)


def tex_path(key, m):
    return os.path.join(OUT, key, f"{TEXTURES[key][0]}_{m}.jpg")


def hdri_path():
    return os.path.join(OUT, "HDRI", f"{HDRI}_4k.hdr")


def fetch(res="2k"):
    for key, (pid, _) in TEXTURES.items():
        if all(os.path.exists(tex_path(key, m)) for m in MAPS):
            continue
        files = _get(f"https://api.polyhaven.com/files/{pid}")
        for m, api in MAPS.items():
            e = files[api].get(res) or files[api]["2k"]
            _download(e["jpg"]["url"], tex_path(key, m))
    if not os.path.exists(hdri_path()):
        files = _get(f"https://api.polyhaven.com/files/{HDRI}")
        _download(files["hdri"]["4k"]["hdr"]["url"], hdri_path())
    with open(os.path.join(OUT, "CREDITS.txt"), "w") as f:
        f.write("CC0 assets from Poly Haven (https://polyhaven.com) used by Tools/VillageKit.\n\n")
        for key, (pid, _) in TEXTURES.items():
            f.write(f"{key}: https://polyhaven.com/a/{pid}\n")
        f.write(f"sky: https://polyhaven.com/a/{HDRI}\n")


if __name__ == "__main__":
    res = sys.argv[sys.argv.index("--res") + 1] if "--res" in sys.argv else "2k"
    fetch(res)
    print("Done.")
