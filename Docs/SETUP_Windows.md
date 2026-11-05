# Setting up on a Windows gaming PC

This is the machine the game is meant to be seen on — max settings, Lumen with
hardware ray tracing, Nanite, virtual shadow maps.

## What you need

- A DirectX 12 GPU (RTX 2060 / RX 6600 or better; RTX 3070 / RX 9070 class for
  Max), 16 GB RAM (32 GB is comfier), and about **100 GB free disk** (the engine
  is ~45 GB, Visual Studio ~10 GB, the project, its build and the packaged game ~35 GB).
- An Epic Games account (free) — the engine only installs through the launcher.

## Setting it up (one time)

1. **Unreal Engine 5.8**: Epic Games Launcher → Unreal Engine → Library → the
   **+** next to *Engine Versions* → 5.8 → Install. (If you skip this,
   `Setup.bat` installs the launcher and tells you what to click.)
2. **Get the project**: on https://github.com/faarisaahmed/dragon-flight-ue5
   click **Code → Download ZIP** and unzip it somewhere with a short path, like
   `C:\DragonFlight` (or clone it with git — no Git LFS needed).
3. **Double-click `Setup.bat`** in that folder. It installs Visual Studio's C++
   tools if they are missing (say *Yes* when Windows asks), downloads the
   terrain, dragon, village and textures, compiles, builds the island, packages
   the game and puts a **Dragon Flight** shortcut on the desktop. First run:
   1–2 hours, mostly shader compiling. Leave it running.

Then play from the desktop shortcut. If it stops on an error, run `Setup.bat`
again (it skips the finished steps); if it fails the same way, send Faaris a
screenshot of the window and the newest file in `Saved\Logs\`.

Other options: `Setup.bat editor` opens the Unreal editor instead of packaging;
`Setup.bat max` builds 16k terrain and 8k ground (untested; only after a normal
build works, and with 32 GB+ RAM). After downloading a newer version, delete
`Saved\IslandBuilt.txt` so the island is rebuilt.

## Graphics

The first launch benchmarks the PC and, if it rates *Epic*, goes straight to
**Max (Cinematic)**. **F2** cycles Low → Medium → High → Epic → Max at any time,
and **F5** cycles the render resolution: Auto → 720p → 900p → 1080p → 1440p →
Native (upscaled to the window by TSR). Both are saved. On a big GPU: F2 to Max,
F5 to Native. In the console (`~`): `Quality 4`, `RenderRes 5`, `t.MaxFPS 0` to
uncap.

## Sharing the game

The packaged game is in `Packaged\Windows\` (`DragonFlight.exe`). Zip that
whole folder (~4 GB) to give it to someone — they just unzip and run it, no
engine needed.

## Controls

See the in-game card (F1). Keyboard + mouse and DualSense/Xbox pads both work.

## If something breaks

Send the newest file in `Saved\Logs\` to Faaris. Most likely culprits in this
early version: a compile error after an engine version mismatch (we build on
5.8), or the build step failing on a missing download (re-run `Setup.bat`; it
skips what's already there).
