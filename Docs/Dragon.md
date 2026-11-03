# The dragon

The game picks the dragon's look in this order:

1. `ADragonPawn::DragonAsset`, if set (any skeletal mesh).
2. `/Game/Dragon/SK_Dragon` — imported by the builder from `Data/Dragon/SK_Dragon.fbx`.
3. The primitive stand-in (`UDragonStandInComponent`), so the game always runs.

A skeletal dragon needs no animation assets: `UDragonRigComponent` drives its
bones every frame from the flight state (wingbeat, flap-glide, folding on the
ground, tail rudder, neck lean, leg swing), as the web game's rig did.

## Using a different dragon (e.g. one from Fab)

Import it, set `DragonAsset` on the pawn (or name it `/Game/Dragon/SK_Dragon`),
set `DragonMeshYaw` so it faces +X, then in the pawn's **Rig** component type its
bone names into the joints (wing shoulder/elbow/wrist per side, neck chain, head,
tail chain, four legs). Axes are in the dragon's body frame — X forward, Y right,
Z up — so they rarely need changing; flip `Sign` on a joint if it moves the wrong
way. A model that ships with real flap/glide/walk animations would look better
still; wiring those through an Animation Blueprint is a later step.

## The scripted dragon

`Tools/DragonModel/build_dragon.py` builds an original dragon procedurally in
Blender (no hand modelling): skin-modifier body, membranes, horns and spines,
automatic weights, baked scale textures. See `Data/Dragon/README.md`.
