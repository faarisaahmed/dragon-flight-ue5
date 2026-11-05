# Dragon Flight on Windows

## Install

1. **Install Unreal Engine 5.8** (free; needs an Epic Games account):
   Epic Games Launcher → Unreal Engine → Library → **+** next to *Engine
   Versions* → **5.8** → Install. Click **Options** first and untick *Starter
   Content*, *Templates*, *Engine Source*, *Editor symbols for debugging* and
   the Android / iOS / Linux platforms — saves about 20 GB.
2. **Open PowerShell** (Start menu → type `PowerShell` → Enter) and paste:

   ```
   irm https://raw.githubusercontent.com/faarisaahmed/dragon-flight-ue5/main/Install.ps1 | iex
   ```

   Leave the window open — the first time takes 1–2 hours (mostly compiling
   shaders). Say **Yes** if Windows asks to install Visual Studio's C++ tools.

That's it. When it's finished there's a **Dragon Flight** shortcut on the
desktop and the build files are deleted by themselves. It asks once whether to
remove Unreal Engine and Visual Studio as well (frees ~50 GB; you'd only need
them again to build a newer version). The game itself is ~4 GB, in
`%USERPROFILE%\Games\Dragon Flight`.

**Disk:** about 50 GB free while it builds (plus ~40 GB for Unreal itself);
~4 GB afterwards.

**If it stops with an error:** paste the same command again — it carries on
where it stopped. If it fails the same way, send Faaris a screenshot of the
window and the newest file in `C:\DragonFlightBuild\Saved\Logs`.

**Newer version:** paste the same command again.

## Playing

F1 shows the controls (same as the browser game's WASD layout; keyboard +
mouse or an Xbox / PlayStation pad). **F2** cycles graphics Low → Max, **F5**
the render resolution Auto → 720p → … → Native. Both are saved. On a big GPU:
F2 to **Max**, F5 to **Native**.

To share the game, zip `%USERPROFILE%\Games\Dragon Flight` (~4 GB) — whoever
gets it just unzips and runs `DragonFlight.exe`.

Needs a DirectX 12 GPU (RTX 2060 / RX 6600 or better; RTX 3070 / RX 9070 class
for Max) and 16 GB RAM.

## For developers

`Setup.bat` in a clone does the build steps directly (`Setup.bat editor` opens
the Unreal editor instead of packaging; `Setup.bat max` builds 16k terrain and
8k ground — untested, 32 GB+ RAM). Delete `Saved\IslandBuilt.txt` to force the
island to rebuild.
