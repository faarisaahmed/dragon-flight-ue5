# Setting up on the Mac (dev machine)

Development moved to the **Mac mini** (M4 Pro, 24 GB, macOS 26), which runs
**UE 5.8**. The old M1 (macOS 13.5) can only run UE 5.5, so it is no longer
used for the engine. A Mac still shows a reduced picture compared with the
Windows PC (no DX12 hardware ray tracing path), so judge final looks there.

1. Epic Games Launcher → sign in → Unreal Engine → Library → **+** → **5.8** →
   Options: macOS only → install to `/Users/Shared/Epic Games` (~40 GB).
   - Greyed-out **+**: the account has no Unreal licence yet. Accept the EULA at
     unrealengine.com (or click Download there), sign out of the launcher, ⌘Q, reopen.
   - **IS-0007** on install: the launcher didn't create
     `~/Library/Application Support/Epic/EpicGamesLauncher/Data/Manifests/Pending`.
     Create it, delete the half-made `UE_5.8` folder, install again.
2. Xcode: the mini builds fine with the App Store **Xcode 26.6** (Epic's docs say 26.1.1;
   if a compile ever fails on the toolchain, `xcodes install 26.1.1 --select`). Xcode 26 ships
   the Metal shader compiler separately — without it the game quits at launch with
   "missing Metal Toolchain": `xcodebuild -downloadComponent MetalToolchain` (~700 MB).
3. Data: `python3 Tools/fetch_world.py` (terrain + dragon) and `python3 Tools/fetch_assets.py` (CC0 art).
4. Build + world:
   ```sh
   UE=/Users/Shared/Epic\ Games/UE_5.8
   "$UE/Engine/Build/BatchFiles/Mac/Build.sh" DragonFlightEditor Mac Development -Project="$PWD/DragonFlight.uproject"
   "$UE/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor" "$PWD/DragonFlight.uproject" -run=BuildArchipelago -Res=4033 -unattended
   open "$UE/Engine/Binaries/Mac/UnrealEditor.app" --args "$PWD/DragonFlight.uproject"
   ```
The Mac uses the 4033 heightmap (11 m/sample); the PC uses 8129 or 16321.
