# Status

_Last updated: 2026-11-02 (first overnight session)._

## Honest summary

Everything below has been **written but not yet compiled or run** — Unreal
Engine isn't installed on the dev Mac yet (the Epic launcher needs a login). The
first job of the next session is to install UE 5.8 on the Mac mini, compile, fix what the
compiler finds, and see it.

## Done

| Area | What | Where |
|---|---|---|
| World data | 45 km archipelago baked from the web game's terrain code: heightmaps at 4k/8k/16k, 2 mask textures, World.json; released as `world-data-v1` | `Tools/WorldBake`, `Docs/World.md` |
| Art | 12 ground texture sets (4K) + 38 CC0 models, alpha maps, re-centred glTFs | `Tools/fetch_assets.py` |
| Flight | `controls.js` ported constant for constant: speed ladder, g-limited turns, zoom/stall/dive/drop with the double-tap grammar, knife edge, barrel roll, strafe, world-edge current, snare | `DragonFlightModel.*` |
| Dragon pawn | terrain look-ahead floor + impacts, camera-relative walking with reverse cone, health/respawn, plasma (6-shot magazine) | `DragonPawn.*`, `DragonPlasma.*` |
| Body physics | on foot, landing and swimming he is a mass on four spring-damper legs (traced feet, friction-limited traction, sliding past the grip, active levelling by leg length, rigid-body pitch/roll); landings keep their momentum and are absorbed by the legs (hard ones crouch, thud, dust, shake); leap take-off; walks off edges into a glide; floats with buoyancy on the CPU copy of the sea's waves and swims (hold I over water to come down, Space to haul out) | `DragonPawn.cpp` TickGround/StepBody |
| Water interaction | GPU wave-equation field (768², 0.5 m) round the camera: wingtips in a knife edge, feet, downwash, swimming, plasma and splashes press the sea and the ripples run out; foam trails, spray and mist; skim hiss; the flight floor over the sea lets him skim and lets a knife-edged wingtip cut ~0.5 m in | `DragonWake.*`, M_WakeSim, M_Sea |
| Scripted tests | AutoShot 40-44: knife skim, feet skim, hover, landing (3 moments), walk, gallop, swim, on the dragon's own clock, with [Body]/[Fly]/[Wake] telemetry (-DragonTelemetry in play) | `DragonPhotoDirector.cpp` |
| Camera | player-owned orbit, focus-relative smoothing, auto-trail, occlusion, FOV with speed (vertical→horizontal), lean, aim shoulder | `DragonPawn.cpp` |
| Sky | physically based sun/moon at 64°N, sky atmosphere, volumetric clouds, height fog, real-time sky light, auto exposure, 6 weather presets that roll in | `DragonSky.*` |
| Vegetation | streamed deterministic HISM scatter from the masks: trees, saplings, deadwood, rocks, cliffs, ground cover | `DragonScatter.*` |
| Dragon visuals | primitive stand-in with procedural wingbeat/glide/fold/tail/walk; procedural bone rig for a real skeletal dragon | `DragonStandInComponent.*`, `DragonRigComponent.*` |
| Builder | one command builds everything: textures, landscape material (HLSL), foliage/rock/membrane materials, model import with Nanite, landscape, ocean/lakes/rivers, sky, scatter, map | `Source/DragonFlightEditor` |
| Setup | `Setup.bat` one-click for Windows; Mac steps | `Docs/SETUP_*.md` |
| Rendering config | Lumen (HW RT), VSM, Nanite, TSR, DX12 SM6, Cinematic scalability pushed further | `Config/` |

## Next

1. UE 5.8 on the Mac mini (installing 2026-11-03): compile, fix errors, run the builder at 4033, look at it.
2. Wire the volumetric cloud material's parameters to the weather (the sky logs their names).
3. Rain and storm (Niagara), lightning, wind audio, music.
4. Water polish: river widths/flow, surf on rocks, underwater.
5. Village and props (Hearthholm), from the web game's layout in World.json.
6. Story, quests and the rest of the web game's systems (STORY.md etc.), in passes.
7. ~~A real dragon~~: done. The scripted Blender dragon v1 (350k tris, 4K) ships as release `dragon-v1`. A Fab model can still be swapped in (see Docs/Dragon.md).
