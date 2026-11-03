# How the world is made

The archipelago is not re-authored: it is **baked from the web game's own
terrain code**, so every island, fjord, glacier, river and lake is where it was.

```
dragon-flying-sim/js/terrain.js (+ glaciers, hydrology, berklayout, worldscale)
        │  copied verbatim to Tools/WorldBake/terrain/
        ▼
Tools/WorldBake/bake.mjs  (Node, worker threads; ~1 min on the M4 Pro)
        ├─ Heightmap_{4033,8129,16321}.r16/.png   16-bit, 45 km square
        ├─ Masks_4096.png    R forest  G vegetation  B glacier ice  A rivers/lakes
        ├─ Masks2_4096.png   R slope   G firn         B village pads
        └─ World.json        islands (renamed), rivers, lakes, falls, glacier fronts, spawn, village plots
        ▼
DragonFlightEditor: BuildArchipelago
        ├─ Landscape (45 km, Z scale 400 → ±1024 m at 3 cm steps)
        ├─ M_Archipelago     one HLSL node: 9 scanned PBR layers + far-field aerials,
        │                    triplanar cliffs, distance-blended tiling, masks by world position
        ├─ Water: ocean + zone, a lake body per lake, a river spline per river
        ├─ ADragonSky        sun/moon/atmosphere/clouds/fog, clock + weather
        ├─ ADragonScatter    trees, saplings, deadwood, rocks, cliffs, grass — streamed tiles from the masks
        └─ /Game/Maps/Archipelago
```

**Coordinates.** Web is three.js (Y up, right-handed, metres); Unreal is Z up,
left-handed, centimetres. One rule everywhere: `UE (X, Y, Z) = web (x, z, y) × 100`
— a single axis swap, so nothing is mirrored (`DragonUnits::FromWeb`).

**Names.** The web game borrowed its place names from a film series; this
version renames them (table in `bake.mjs`, `RENAME`). Home is *Hearthholm*.

**Art.** All CC0 from Poly Haven via `Tools/fetch_assets.py`: 12 ground texture
sets at 4K (8K with `--tex 8k`) and ~38 models (fir/pine trees and saplings,
stumps, mossy rocks, coastal cliffs, grass, ferns, shrubs, flowers).
