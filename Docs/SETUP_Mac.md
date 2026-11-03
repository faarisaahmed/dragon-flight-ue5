# Setting up on the Mac (dev machine)

The dev Mac is an M1 on macOS 13.5, which caps us at **UE 5.5** (5.6+ needs
macOS 14). M1 has no Nanite or virtual shadow maps and only software Lumen, so
the Mac shows a reduced picture; the Windows PC shows the real one.

1. Install the Epic Games Launcher: `brew install --cask epic-games`
   (asks for your password), sign in, install **UE 5.5** with only the
   macOS target platform ticked (~45 GB).
2. Xcode 15.2+ must be selected: `sudo xcode-select -s /Applications/Xcode.app`.
3. Terrain: `python3 Tools/fetch_world.py` (or bake it: `node Tools/WorldBake/bake.mjs --res 4033`).
4. Art: `python3 Tools/fetch_assets.py`.
5. Build + world:
   ```sh
   UE=/Users/Shared/Epic\ Games/UE_5.5
   "$UE/Engine/Build/BatchFiles/Mac/Build.sh" DragonFlightEditor Mac Development -Project="$PWD/DragonFlight.uproject"
   "$UE/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor" "$PWD/DragonFlight.uproject" -run=BuildArchipelago -Res=4033 -unattended
   open "$UE/Engine/Binaries/Mac/UnrealEditor.app" --args "$PWD/DragonFlight.uproject"
   ```
The Mac uses the 4033 heightmap (11 m/sample); the PC uses 8129 or 16321.
