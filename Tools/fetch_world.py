#!/usr/bin/env python3
"""
Downloads the baked heightmaps (too big for git) from this repo's GitHub
release (TAG below) into Data/World/ and unzips them, plus the dragon
model (release "dragon-v1") into Data/Dragon/.

    python Tools/fetch_world.py            # 8129 (default) and 4033
    python Tools/fetch_world.py --max      # also 16321, the sharpest terrain

Uses the GitHub CLI (`gh`) if it is installed and logged in; otherwise plain
HTTPS (the repo is public; GITHUB_TOKEN is used if set). If neither works,
download the zips by hand from the Releases page and unzip them into Data/World/.
"""
import argparse, json, os, shutil, subprocess, sys, urllib.request, zipfile

REPO = "faarisaahmed/dragon-flight-ue5"
TAG = "world-data-v3"   # v3: + naturalise.py North Atlantic relief (matches Masks2/Masks3/World.json in git)
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "Data", "World")


DRAGON_TAG = "dragon-v1"
ASSETS_TAG = "game-assets-v2"   # village kit + the web dragon (SK_Dragon.glb); v2: muted turf roofs and grass
DATA = os.path.join(ROOT, "Data")


def via_gh(names, tag=TAG, out=OUT):
    if not shutil.which("gh"):
        return False
    args = ["gh", "release", "download", tag, "-R", REPO, "-D", out, "--clobber"]
    for n in names:
        args += ["-p", n]
    return subprocess.call(args) == 0


def via_token(names, tag=TAG, out=OUT):
    # The repo is public, so no token is needed; GITHUB_TOKEN is used when set (rate limits).
    tok = os.environ.get("GITHUB_TOKEN")
    h = {"Accept": "application/vnd.github+json", "User-Agent": "fetch_world"}
    if tok:
        h["Authorization"] = f"Bearer {tok}"
    rel = json.load(urllib.request.urlopen(urllib.request.Request(f"https://api.github.com/repos/{REPO}/releases/tags/{tag}", headers=h)))
    for a in rel["assets"]:
        if a["name"] in names:
            print("downloading", a["name"])
            req = urllib.request.Request(a["url"], headers={**h, "Accept": "application/octet-stream"})
            with urllib.request.urlopen(req) as r, open(os.path.join(out, a["name"]), "wb") as f:
                shutil.copyfileobj(r, f, 1 << 20)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--max", action="store_true", help="also fetch the 16321 heightmap (~430 MB)")
    a = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    names = ["Heightmap_8129.zip", "Heightmap_4033.zip"] + (["Heightmap_16321.zip"] if a.max else [])
    # Maps from an older release don't match the masks in git: fetch again when the stamp differs.
    stamp = os.path.join(OUT, "Heightmap.release")
    current = open(stamp).read().strip() if os.path.exists(stamp) else ""
    if current != TAG:
        names = names
    else:
        names = [n for n in names if not os.path.exists(os.path.join(OUT, n.replace(".zip", ".r16")))]
    if names and not (via_gh(names) or via_token(names)):
        print(f"Could not download automatically. Get {', '.join(names)} from\n  https://github.com/{REPO}/releases/tag/{TAG}\nand unzip them into {OUT}")
        return 1
    for n in names:
        z = os.path.join(OUT, n)
        if os.path.exists(z):
            with zipfile.ZipFile(z) as f:
                f.extractall(OUT)
            os.remove(z)
            print("unpacked", n)
    with open(stamp, "w") as f:
        f.write(TAG)
    print("World data ready.")
    # The dragon (SK_Dragon.fbx + 4K textures) unpacks into Data/Dragon/.
    if not os.path.exists(os.path.join(DATA, "Dragon", "SK_Dragon.fbx")):
        z = os.path.join(DATA, "Dragon.zip")
        if via_gh(["Dragon.zip"], DRAGON_TAG, DATA) or via_token(["Dragon.zip"], DRAGON_TAG, DATA):
            if os.path.exists(z):
                with zipfile.ZipFile(z) as f:
                    f.extractall(DATA)
                os.remove(z)
                print("unpacked Dragon.zip")
        else:
            print(f"Get Dragon.zip from https://github.com/{REPO}/releases/tag/{DRAGON_TAG} and unzip it into {DATA}")
    # The web game's dragon (SK_Dragon.glb, preferred by the builder) and the village kit.
    # A newer release replaces what an older one unpacked (the stamp records which one that was).
    astamp = os.path.join(DATA, "Assets.release")
    acur = open(astamp).read().strip() if os.path.exists(astamp) else ""
    ok = True
    for zname, probe in (("DragonWeb.zip", os.path.join(DATA, "Dragon", "SK_Dragon.glb")),
                         ("Village.zip", os.path.join(DATA, "Village", "longhouse_a.glb")),
                         # the Night Fury by Fulonimation Studios (exported by Tools/DragonModel/export_fulo.py);
                         # optional: without it the game uses the web dragon
                         ("DragonFulo.zip", os.path.join(DATA, "Dragon", "Fulo", "SK_DragonFulo.glb"))):
        if os.path.exists(probe) and (acur == ASSETS_TAG or (acur == "" and ASSETS_TAG == "game-assets-v1")):
            continue
        z = os.path.join(DATA, zname)
        if via_gh([zname], ASSETS_TAG, DATA) or via_token([zname], ASSETS_TAG, DATA):
            if os.path.exists(z):
                with zipfile.ZipFile(z) as f:
                    f.extractall(DATA)
                os.remove(z)
                print(f"unpacked {zname}")
        elif zname == "DragonFulo.zip":
            print("No DragonFulo.zip in this release: the game will use the web dragon.")
        else:
            ok = False
            print(f"Get {zname} from https://github.com/{REPO}/releases/tag/{ASSETS_TAG} and unzip it into {DATA}")
    if ok:
        with open(astamp, "w") as f:
            f.write(ASSETS_TAG)
    return 0


if __name__ == "__main__":
    sys.exit(main())
