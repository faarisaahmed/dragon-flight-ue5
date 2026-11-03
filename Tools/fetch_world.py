#!/usr/bin/env python3
"""
Downloads the baked heightmaps (too big for git) from this repo's GitHub
release "world-data-v1" into Data/World/ and unzips them.

    python Tools/fetch_world.py            # 8129 (default) and 4033
    python Tools/fetch_world.py --max      # also 16321, the sharpest terrain

Uses the GitHub CLI (`gh`) if it is installed and logged in; otherwise a
GITHUB_TOKEN environment variable (the repo is private). If neither is
available, download the zips by hand from the Releases page and unzip them
into Data/World/.
"""
import argparse, json, os, shutil, subprocess, sys, urllib.request, zipfile

REPO = "faarisaahmed/dragon-flight-ue5"
TAG = "world-data-v1"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "Data", "World")


def via_gh(names):
    if not shutil.which("gh"):
        return False
    args = ["gh", "release", "download", TAG, "-R", REPO, "-D", OUT, "--clobber"]
    for n in names:
        args += ["-p", n]
    return subprocess.call(args) == 0


def via_token(names):
    tok = os.environ.get("GITHUB_TOKEN")
    if not tok:
        return False
    h = {"Authorization": f"Bearer {tok}", "Accept": "application/vnd.github+json", "User-Agent": "fetch_world"}
    rel = json.load(urllib.request.urlopen(urllib.request.Request(f"https://api.github.com/repos/{REPO}/releases/tags/{TAG}", headers=h)))
    for a in rel["assets"]:
        if a["name"] in names:
            print("downloading", a["name"])
            req = urllib.request.Request(a["url"], headers={**h, "Accept": "application/octet-stream"})
            with urllib.request.urlopen(req) as r, open(os.path.join(OUT, a["name"]), "wb") as f:
                shutil.copyfileobj(r, f, 1 << 20)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--max", action="store_true", help="also fetch the 16321 heightmap (~430 MB)")
    a = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    names = ["Heightmap_8129.zip", "Heightmap_4033.zip"] + (["Heightmap_16321.zip"] if a.max else [])
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
    print("World data ready.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
