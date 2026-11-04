#!/usr/bin/env python3
"""
Downloads the baked heightmaps (too big for git) from this repo's GitHub
release "world-data-v1" into Data/World/ and unzips them, plus the dragon
model (release "dragon-v1") into Data/Dragon/.

    python Tools/fetch_world.py            # 8129 (default) and 4033
    python Tools/fetch_world.py --max      # also 16321, the sharpest terrain

Uses the GitHub CLI (`gh`) if it is installed and logged in; otherwise a
GITHUB_TOKEN environment variable (the repo is private). If neither is
available, download the zips by hand from the Releases page and unzip them
into Data/World/.
"""
import argparse, json, os, shutil, subprocess, sys, urllib.request, zipfile

REPO = "faarisaahmed/dragon-flight-ue5"
TAG = "world-data-v2"   # v2: eroded + cliff-detailed terrain (matches Masks2/Flow/World.json in git)
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "Data", "World")


DRAGON_TAG = "dragon-v1"
DATA = os.path.join(ROOT, "Data")


def via_gh(names, tag=TAG, out=OUT):
    if not shutil.which("gh"):
        return False
    args = ["gh", "release", "download", tag, "-R", REPO, "-D", out, "--clobber"]
    for n in names:
        args += ["-p", n]
    return subprocess.call(args) == 0


def via_token(names, tag=TAG, out=OUT):
    tok = os.environ.get("GITHUB_TOKEN")
    if not tok:
        return False
    h = {"Authorization": f"Bearer {tok}", "Accept": "application/vnd.github+json", "User-Agent": "fetch_world"}
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
    return 0


if __name__ == "__main__":
    sys.exit(main())
