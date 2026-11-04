# Setting up on a Windows gaming PC

This is the machine the game is meant to be seen on — max settings, Lumen with
hardware ray tracing, Nanite, virtual shadow maps.

## Once

1. **Access**: the repo is private. Ask Faaris to add your GitHub account as a
   collaborator, then clone it (GitHub Desktop is fine). Install
   [Git LFS](https://git-lfs.com) first.
2. **Unreal Engine 5.8** (or newer 5.x) from the Epic Games Launcher → Unreal
   Engine → Library → +. Default install folder is fine.
3. **Visual Studio 2022** (Community is free) with the workload
   *Game development with C++* (it includes the Windows SDK). Needed because the
   game is C++.
4. **GitHub CLI** (`winget install GitHub.cli`, then `gh auth login`) so the
   setup script can download the terrain from the private release. Without it,
   download the `Heightmap_*.zip` files from the repo's *Releases* page and
   unzip them into `Data\World\`.

## Every time you want to (re)build

Double-click **`Setup.bat`** (or `Setup.bat max` for the 16k terrain and 8k
ground textures — recommended on an RTX 3080 or better). It:

1. downloads the terrain heightmaps and ~3 GB of CC0 textures and models,
2. compiles the C++,
3. builds the map — landscape, ocean, lakes, rivers, sky, forests
   (first run: 20–60 min, mostly shader compilation and Nanite builds),
4. opens the editor. Press **Play** (or Alt+P). For a standalone game:
   *Platforms → Windows → Package Project*.

## Graphics

The first launch benchmarks the PC and, if it rates *Epic*, goes straight to
**Max (Cinematic)**. **F2** cycles Low → Medium → High → Epic → Max at any time.
In the console (`~`): `Quality 4` for Max, `r.ScreenPercentage 100` for native
resolution, `t.MaxFPS 0` to uncap.

## Controls

See the in-game card (F1). Keyboard + mouse and DualSense/Xbox pads both work.

## If something breaks

Send the newest file in `Saved\Logs\` to Faaris. Most likely culprits in this
early version: a compile error after an engine version mismatch (we build on
5.8), or the build step failing on a missing download (re-run `Setup.bat`; it
skips what's already there).
