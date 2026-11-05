# Dragon Flight on Windows

## Install

1. **Install Unreal Engine 5.8** (free; needs an Epic Games account). If
   5.9 is already installed it is used instead, but 5.8 is what the game is made
   and tested with, so installing 5.8 next to it is the safer bet. UE 6
   previews are ignored.
   In the Epic Games Launcher → Unreal Engine → Library → **+** next to *Engine
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
desktop (the game is in `%USERPROFILE%\Games\Dragon Flight`).

**Newer version:** paste the same command again. The build folder
(`C:\DragonFlightBuild`) is kept, so updates only redo what changed —
usually 10–30 minutes, not hours.

**Disk:** ~40 GB for Unreal, ~10 GB for Visual Studio, ~45 GB for the build
folder and the game. To uninstall everything: delete `C:\DragonFlightBuild`
and `%USERPROFILE%\Games\Dragon Flight`, and remove Unreal Engine (Epic
launcher) and Visual Studio (Settings → Apps).

**If it stops with an error:** paste the same command again — it carries on
where it stopped. If it fails again, answer **Y** when it offers to send an
error report: a GitHub page opens with everything filled in, press **Submit
new issue** (needs a free GitHub account). The full log is also put on the
desktop.

**If the game crashes:** start it again — it says so at the top of the screen;
press **F8** and Submit. **F8** at any other time sends feedback (what you
were doing, your settings and where you are are filled in; type what you saw,
and drag a screenshot in if you like). Reports are public issues on the repo;
your Windows user name is removed from them.


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
