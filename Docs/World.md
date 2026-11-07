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
Tools/WorldBake/erode.mjs, cliffs.mjs   stream-power erosion, then crags on the steep coasts
        ▼
Tools/WorldBake/naturalise.py          North Atlantic relief (below)
Tools/WorldBake/lakes.py               lakes of the final ground (World.json "lakes2")
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

## Making it look like a real North Atlantic archipelago

The islands are modelled on the Faroes, Lofoten, Shetland and the Hebrides: steep
green turf and heath, sheer sea cliffs and stacks, woods only in shelter.

- **Relief** (`naturalise.py`). The baked terrain was covered in 100-300 m noise lumps
  (median slope 30 deg, half the land steeper than 33 deg, so it rendered as bare
  brown crag). The pass splits the ground into the landform (300 m Gaussian) and the
  lumps, keeps ~36% of the lumps on low ground rising to ~85% on the mountains, all of
  them on high coasts (sea cliffs) and at the villages, a little more on the biggest
  knolls (rocky tops), relaxes slopes over 46 deg inland, carves stream gullies from a
  D8 flow accumulation, and adds a metre of hummocks. Coastlines and the sea floor are
  untouched. Median slope 0.65 -> 0.39.
- **Lakes** (`lakes.py`). A priority flood of the final heightmap: every hollow that would
  hold water deeper than 5 m over more than 2.5 ha becomes a flat lake at its spill level.
  (The web game's lakes predate erosion; flooding them spilled sheets of water over cliffs.)
- **Rivers** are laid on the ground as it is now at load time (never uphill).
- **Climate.** The northern edge lies in the drift ice of a cold current, as north Iceland's
  does: from ~9 km north of the middle the snow line falls from 480 m to 40 m
  (`UDragonWorldSubsystem::ColdAt / SnowLineAt`, mirrored in the landscape and rock
  shaders). The northern isles are snow, ice and tundra, with icebergs offshore; the tree
  line follows the snow line; nothing grows on lying snow.
- **Spacing.** Straits of 0.2-4 km between islands of 3-10 km, as between the Faroes.
- **Plants and rocks.** Conifer woods only in shelter, thinning to the tree line and kept
  off the salt shore; boulders on scree and rocky foreshores, not on the turf; rock faces
  only on sheer ground; moss and lichen on the up-facing sides of rocks, snow on them in
  the cold, a dark wet band at the waterline.
- **Island characters** (`biomes.py` -> `Masks3_4096.png`, World.json `biomes`). Most isles
  are turf and heath. *Volcanic* (Wyrmspire, Emberwick): black sand, near-black basalt and
  lava fields, yellow-green fringe moss, lava boulders, no trees, fumaroles steaming in the
  wind (`ADragonVents`). *Woodland* (the sheltered southern isles): conifer forest over the
  slopes below the tree line. *Moorland* (Quietmoor, Mirefall Bog, Greywater Sound, Sheep
  Island): heather drifts and peat bog in the wet hollows. The *arctic* north is climate.
