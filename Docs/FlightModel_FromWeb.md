# Player Dragon Movement Model — specification extracted from the web build

Source: `/Users/faaris/WORKSPACE/Games/dragon-flying-sim` (three.js), commit `8c0b5e4`.
All file:line references are to that repo. **The code is the authority**; the
README is stale in places (noted where it matters).

Primary files:
- `js/controls.js` — the whole flight model (speed ladder, turning, vertical axis, zoom/stall/dive state machine, knife edge, barrel roll, strafe, world edge, snare, attitude).
- `js/main.js` — frame loop, collision floor / crash, landing, takeoff, walking, camera rig, mouse/pad look, firing.
- `js/groundbody.js` — the on-foot physics body (4 spring legs, friction, flare).
- `js/keymap.js` — keyboard schemes. `js/gamepad.js` — pad deadzones/curves/button ids.
- `js/plasma.js`, `js/aim.js` — fire and aiming. `js/speedfx.js` — speed visuals.
- `js/health.js` — impact damage. `js/wings.js`, `js/flightrig.js`, `js/gait.js` — animation only (summarised in §10).
- `js/flights.js` is the NPC wild-dragon flocks, NOT the player. `js/player.js` is story state (food/sleepfire charge), not movement. `js/input.js` is the menu/room input layer, not flight.

---

## 0. Conventions, units, and conversion to UE5

- **Units:** 1 world unit = 1 metre; time in seconds; angles in radians. Speeds in m/s (`MPH = 0.44704` m/s per mph, `controls.js:89`).
- **Axes (three.js):** right-handed, **Y up**. Ground plane is XZ.
- **Heading `h`** (`controls.js:594`) is the direction of TRAVEL. Forward vector = `(sin h, 0, cos h)`. `h = 0` faces +Z. **Increasing `h` turns LEFT** (toward +X). The dragon's left vector is `(cos h, 0, -sin h)`; right is `(-cos h, 0, sin h)`.
- **North on the HUD is −Z** (`main.js:734`). Compass bearing in degrees = `atan2(sin h, −cos h)`.
- The GLB's nose points down local −Z, so the mesh yaw is `h + π` (`controls.js:896`). Rotation order is `YXZ` (yaw, then pitch, then roll in body frame — aircraft convention, `controls.js:479`).
- **Pitch/roll pivot:** the model origin is at his feet. Pitch and roll are applied to a child node ("attitude") placed at `BODY_CENTRE = 1.45 m` above the origin, with the mesh hung back down by −1.45 (`controls.js:499-511`). Yaw stays on the root. **In UE: put the pitch/roll on a scene component 1.45 m (145 cm) above the actor origin; yaw on the actor.** On the ground the walk writes the root's full rotation and the attitude node is reset to identity (`releaseAttitude`, `controls.js:1439`).
- **Dragon size:** 8.6 m nose-to-tail, 15.1 m wingspan at scale 1 (life size; `controls.js:471-475`).
- **Pitch sign:** positive = nose up. **Roll sign:** positive `currentRoll` = LEFT wing down (bank into a left / +yawRate turn) (`controls.js:1159-1161`). **Barrel roll `dir` +1 = roll to the left.** Knife edge `+1` = left wing down.
- **Damping idiom used everywhere:** `damp(λ, dt) = 1 − exp(−λ·dt)`; `x += (target − x) · damp(λ, dt)`. This is frame-rate independent exponential smoothing; λ is in 1/s. Port it literally (`controls.js:645`, `main.js:484`).
- **dt clamps:** frame dt clamped to 0.1 s (`main.js:2360`); flight update clamps again to **0.05 s** (`controls.js:683`); ground body to 1/30 s (`groundbody.js:152`).

**Suggested three → UE mapping** (UE is left-handed, Z up, X forward, cm):
`UE.X = 100·three.z`, `UE.Y = −100·three.x`, `UE.Z = 100·three.y`.
Then UE yaw (degrees) = `−degrees(h)` (three `h=0` → UE +X, yaw 0; three "left-positive" becomes UE "right-positive", hence the minus). Pitch: same sign (nose up positive). Roll: re-derive from the *semantics* above (bank toward the turn), don't copy raw signs.
**FOV:** three.js `camera.fov` is **vertical**; UE `FieldOfView` is **horizontal**. Convert: `hFOV = 2·atan(tan(vFOV/2)·aspect)`.

Recommendation: implement flight as a custom kinematic movement (Pawn + custom component, or `UCharacterMovementComponent` custom mode) — the model below sets position directly from speeds; there are no forces, no rigid body, no lift/drag integration (except the ground body in §4.4).

---

## 1. State variables (`controls.js:452-599`)

| Var | Init | Meaning |
|---|---|---|
| `position` (dragon.position) | spawn `(SPAWN_XZ.x, 300, SPAWN_XZ.z)` (`main.js:74`) | actor origin (feet), m |
| `heading` | `camYaw + π` on first update (`controls.js:687`) — i.e. facing away from the camera | travel direction, rad |
| `yawRate` | 0 | rad/s, smoothed |
| `yawMaxNow` | 1.45 | current turn-rate cap |
| `airspeed` | `SPEED_CRUISE` = 55 | m/s. In level/drop modes = horizontal ground speed along heading. In zoom/stall/dive/recover = speed along the flight path |
| `activeSpeed` | = airspeed | the speed reported to camera/HUD (`getSpeed`) |
| `climbVel` | 0 | m/s vertical, owned by the up/down axis |
| `strafeVel` | 0 | m/s sideways (+ = right) |
| `pathAngle` | 0 | flight-path angle off horizontal, rad (+ up). Derived in level mode, driven in manoeuvres |
| `currentPitch`, `currentRoll` | 0 | visual attitude, rad |
| `mode` | `"level"` | `level | drop | zoom | stall | dive | recover` |
| `modeT` | 0 | s in current mode |
| `recoverHold` | 0 | s during which the throttle cannot change airspeed |
| `diveCommit` | 0 | s of post-stall dive that cannot be exited |
| `knifeAmount` | 0 | smoothed −1..+1 (+ = left wing down) |
| `knifeCharge` | 3.0 | wing stamina, s (shared by knife edge and barrel roll) |
| `rollDir, rollT, rollTime, rollBulge, rollRise, rollShift, rollEntry, rollLat, rollUp, rollRate` | 0 / defaults | barrel-roll state |
| `bursting`, `turbo` | false | top gear / turbo held |
| `snared`, `snareDir` | 0 | bola snare seconds left, last shake direction |
| `alignTarget` | null | heading target set by H / D-pad up |
| `currentForwardSpeed` | 55 | the "W" target speed (retrimmable by debug; pad L3 resets to 55) |
| `vertAccel` | 0 | smoothed vertical accel (animation only) |
| `edgeT` | 0 | 0..1 how far past the world soft edge |

Derived helpers:
- `speedRatio = clamp(activeSpeed / 335.28, 0, 1)` (speedT; 1 = 750 mph) (`controls.js:1313`). Drives camera, wings, HUD.
- `turnT = clamp(yawRate / yawMaxNow, −1, 1)`.
- `getEnergy(agl) = clamp(airspeed/DIVE_MAX·0.6 + min(1, agl/900)·0.4, 0, 1)` (HUD only, `controls.js:1352`).
- `getStallT = 1 − clamp((airspeed − 15)/(105 − 15), 0, 1)` while in zoom (HUD + wing "hang" pose).

There is **no** angle-of-attack/lift/drag/gravity simulation in level flight. It is an arcade "flying mount / helicopter" model: forward speed along heading + an independent vertical speed. An aircraft-like single-flight-path model is used only during the zoom/stall/dive/recover manoeuvres.

---

## 2. Flight

### 2.1 Order of operations each flight frame (`controls.update`, `controls.js:679-1309`)
1. dt = min(dt, 0.05). Seed heading if null.
2. Align request (H / D-pad up) → `alignTarget = camYaw + π`.
3. Read forward/back + sprint → `pedalTarget`.
4. Read burst (+turbo gate) → `speedTarget`, `rate`; apply speed smoothing (only if `mode == level && recoverHold <= 0`). Decrement `recoverHold`.
5. Knife-edge input, barrel-roll gate, stamina, `knifeAmount`.
6. Turn input → snare shake → `yawRate` → `heading`. World-edge current. Knife-edge yaw. Set mesh yaw.
7. Strafe.
8. Vertical input → double-tap gate → mode state machine → `climbVel` (incl. drop / snare overrides).
9. Translate position (level or manoeuvre model) + strafe.
10. Barrel-roll helix offset.
11. Bank (roll) and pitch attitude.
12. Rumble.

The flight update is **skipped entirely** while grounded, in a cutscene, caged, or "down" (dead) (`main.js:2439`).

### 2.2 Forward speed — the speed ladder (`controls.js:103-142`, `707-766`)

| Gear | Input | Target m/s | mph |
|---|---|---|---|
| Hover | nothing held | `SPEED_MIN = 0` | 0 |
| Reverse | S / back | `REVERSE_SPEED = 9` (i.e. target = moveInput·9, so −9 → he *backs up*) | 20 |
| Cruise | W | `SPEED_CRUISE = 55` (`currentForwardSpeed`) | 123 |
| Sprint | W + Shift | `PEDAL_MAX = 180` | 403 |
| Flat out ("burst", a held gear) | J/V/B or pad Cross | `BURST_SPEED = 750 mph = 335.28` | 750 |
| Turbo | flat-out double-tapped-and-held, OR flat-out + Shift | `TURBO_SPEED = 1600 mph = 715.26` | 1600 |
| Dive only | — | `DIVE_MAX = 1.2·335.28 = 402.3` | 900 |

Input: `moveInput = (+1 forward) + (−1 back)`; pad adds `sign(f)·f²` where `f = −leftStick.y`, if `|f| > 0.02`, clamped to [−1, 1] (`controls.js:716-719`). Squaring makes mid-stick ≈ cruise.

```
top = sprinting ? max(180, currentForwardSpeed) : currentForwardSpeed
pedalTarget = moveInput > 0 ? moveInput·top : moveInput < 0 ? moveInput·9 : 0
bursting = burstHeld
turbo = bursting && (turboGate.armed || sprinting)        // turboGate = tapGate(0.5, 0.6)
speedTarget = turbo ? 715.26 : bursting ? max(335.28, pedalTarget) : pedalTarget
rate = turbo ? 0.75
     : bursting ? 5.5                                     // BURST_GAIN
     : speedTarget > airspeed ? 1.5                       // PEDAL_GAIN
     : airspeed > 180 ? 1.7                               // BURST_BLEED
     : 0.9                                                // PEDAL_BLEED
if (mode == "level" && recoverHold <= 0) airspeed += (speedTarget − airspeed)·damp(rate, dt)
recoverHold = max(0, recoverHold − dt)
```
Notes:
- **Letting go of W stops him: stopped IS the hover.** No hover mode, no stall in level flight at zero speed.
- Flat out is a held gear: **no timer, no cooldown, no charge meter** (it used to have them; removed deliberately, `controls.js:123-140`). Its cost is the turn-radius growth (§2.3).
- Flat out with no W still goes to 335. Turbo winds up slowly (λ 0.75 → ~1.3 s time constant; "a few seconds to break Mach 1").
- Speed changes do not happen in `drop` mode either (mode ≠ level) — drop has its own airspeed rule (§2.5).

### 2.3 Turning (yaw) and the turn cap (`controls.js:429-436`, `809-866`)

Constants: `YAW_RATE_MAX = 1.45 rad/s`, `TURN_G = 196 m/s²` (~20 g), `YAW_LAMBDA = 3.6`, `YAW_DAMP = 6.0`, `YAW_DEADZONE = 0.02 rad/s`, `ALIGN_TOLERANCE = 0.02 rad`.

```
turnInput = (turnL ? +1 : 0) + (turnR ? −1 : 0)          // + is left
if pad: turnInput = clamp(turnInput − leftStick.x, −1, 1)  // stick right (+x) = turn right
[snare: see §2.10 — turnInput *= 0.22, alignTarget = null]
if turnInput != 0: alignTarget = null
yawMaxNow = min(1.45, 196 / max(airspeed, 1))
if alignTarget != null:
    d = angleDelta(heading, alignTarget)        // shortest signed angle
    if |d| < 0.02: alignTarget = null
    else: yawRate += (clamp(d·1.6, ±yawMaxNow) − yawRate)·damp(3.6, dt)
elif turnInput != 0:
    yawRate += (turnInput·yawMaxNow − yawRate)·damp(3.6, dt)   // partial stick → proportionally lazier arc
else:
    yawRate *= exp(−6·dt); if |yawRate| < 0.02: yawRate = 0
yawRate = clamp(yawRate, ±yawMaxNow)
heading += yawRate·dt
```
- Crossover speed where the g-cap takes over: 196/1.45 ≈ **135 m/s**. Radius = v/ω: cruise 55 m/s → 1.45 rad/s → ~38 m; sprint 180 → 1.09 rad/s → ~165 m; flat out 335 → 0.585 rad/s → ~573 m. At a hover he spins on the spot at 1.45 rad/s. "A supersonic pass has to be lined up, not steered."
- Debug `setTurnRate(v)`: `YAW_RATE_MAX = clamp(v, 0.1, 4)`, `TURN_G = YAW_RATE_MAX·135` (`controls.js:1510-1514`).
- **There is no direct pitch control and no direct roll control in flight.** Pitch is derived from vertical speed; roll (bank) is derived from yaw rate (§2.8).

### 2.4 Up/down axis (`controls.js:161-165`, `214-226`, `906-1037`)

Space = up, K/C (scheme) = down; pad `v = R2 − L2` (with default `padClimbInvert = true`, R2 is up), added to keys and clamped to [−1, 1]. **This axis never changes forward speed, and W never changes altitude.**

Constants: `VERT_HOVER = 20`, `VERT_PER_SPEED = 0.62`, `CLIMB_RATE_CAP = 140`, `VERT_LAMBDA = 7.5` (press), `VERT_RELEASE = 5.5` (release), `TRIM_RATE = 52`, `CLIMB_ENTRY_SPEED = 105`.

```
manoeuvring = mode not in {level, drop}
trimming = !manoeuvring && airspeed >= 105
vertRate = trimming ? 52 : min(20 + |airspeed|·0.62, 140)
vertTarget = verticalInput · vertRate
vertK = (|vertTarget| > 0.98·|climbVel| && verticalInput != 0) ? 7.5 : 5.5
climbVel += (vertTarget − climbVel)·damp(vertK, dt)
if mode == drop:  climbVel += (−75 − climbVel)·damp(3.0, dt)
if snared:        climbVel += (−26 − climbVel)·damp(6.0, dt);  airspeed += (32 − airspeed)·damp(2.2, dt)
```
So: hover lift 20 m/s; at cruise ≈ 54 m/s; just under 105 m/s ≈ 85 m/s; at/above 105 m/s an ordinary (un-double-tapped) press is a gentle fixed 52 m/s "trim".

### 2.5 The vertical manoeuvres: double-tap gate, zoom, stall, dive, recover, drop

**Grammar (essential to the feel): press-and-hold = gentle; double-tap-and-hold = committed.** Same grammar on keyboard, pad triggers and touch buttons, and on the knife-edge keys (barrel roll).

**Tap gate** (`makeTapGate(tapMax, gap)`, `controls.js:29-63`), one per axis. Input each frame is −1/0/+1:
```
state: held=0, dir=0, tapDir=0, age=99, armed=false, fired=false
update(dt, want):
  age += dt; fired = false
  if want != 0 && want == dir: held += dt; return
  // released or reversed: classify the press that just ended
  if dir != 0: if 0 < held <= tapMax: tapDir = dir; age = 0  else tapDir = 0
  held = 0; dir = want
  armed = want != 0 && want == tapDir && age <= gap
  if armed: tapDir = 0; fired = true          // one tap arms one press; armed lasts the whole 2nd press
```
Vertical gate: `tapMax = TAP_MAX = 0.30 s`, `gap = DOUBLE_GAP = 0.34 s`. Axis direction: `wantUp = verticalInput > 0.55`, `wantDown = verticalInput < −0.55`.
`committed = !snared && vertGate.armed && vertGate.held >= MANOEUVRE_HOLD (0.10 s)`.
HUD helpers: `commitT = armed ? min(1, held/0.10) : 0`; `isTrimming = mode==level && gate.dir != 0 && !gate.armed`.

**State machine** (`controls.js:932-995`). `setMode(m)` resets `modeT` on change. If snared and mode ≠ level → force level, clear diveCommit/recoverHold.

| Mode | Entry | Per-frame | Exit |
|---|---|---|---|
| `level` | default | normal model | committed && wantUp && airspeed ≥ 105 → `zoom`; committed && wantDown → `dive` if airspeed ≥ 105 else `drop` |
| `drop` | see above | `airspeed += (DROP_SPEED − airspeed)·damp(1.4, dt)` where `DROP_SPEED = 0.86·105 = 90.3`; climbVel driven to −75 (λ 3.0). **Runs on the LEVEL translation model**: heading, turning, strafe all still work | `!wantDown || wantUp` → level |
| `zoom` | | `airspeed = max(0, airspeed − (34 + 6e-4·airspeed²)·dt)`; `pathAngle += (π/2 − pathAngle)·damp(2.2, dt)` (stands on his tail, goes straight up) | `!wantUp` → recover; `airspeed ≤ 15` → stall (sets `stalled` true for one frame) |
| `stall` | | `airspeed = max(0, airspeed − 34·0.35·dt)`; after `modeT > 0.5` (STALL_HANG): `pathAngle += (−π/2 − pathAngle)·damp(2.2, dt)` | `pathAngle < −0.7` → dive with `diveCommit = 1.1` |
| `dive` | | `pathAngle → −π/2` at damp(2.2); `airspeed = min(402.3, airspeed + 36·dt·(−sin pathAngle))`; `diveCommit −= dt` | `(!wantDown || wantUp) && diveCommit ≤ 0` → recover |
| `recover` | | `pathAngle += (0 − pathAngle)·damp(2.6, dt)`; `recoverHold = 1.1` every frame (airspeed is NOT changed in recover) | `|pathAngle| < 0.10` → level, `climbVel = 0` |

Constants (`controls.js:190-236`, `384-388`): `CLIMB_DRAG = 34 m/s²`, `CLIMB_DRAG_V2 = 6.0e-4 /m`, `STALL_SPEED = 15`, `STALL_HANG = 0.5 s`, `NOSE_OVER_RATE = 2.2`, `DIVE_GRAVITY = 36 m/s²`, `DIVE_MAX = 402.3`, `DIVE_ENTRY_SPEED = 105`, `STALL_DIVE_COMMIT = 1.1 s`, `RECOVER_RATE = 2.6`, `RECOVER_HOLD = 1.1 s`, `DROP_SINK = 75`, `DROP_LAMBDA = 3.0`, `DROP_DRAG = 1.4`.

Design intent: energy trade — a full-height zoom and the dive out of it roughly break even; diving from height you did not pay for is what makes you fast; the speed a dive earns is kept through the pull-up (recoverHold); a stall is the one moment the controls don't answer (~0.5 s hang + 1.1 s committed dive). Going vertical at 750 mph runs out in about four seconds. The throttle cannot top up a zoom or bleed a dive (speed smoothing only runs in `level` with `recoverHold ≤ 0`).

### 2.6 Translation (`controls.js:1053-1090`)

```
if !manoeuvring:                         // level, drop
    activeSpeed = airspeed
    climbRate = climbVel − |knifeAmount|·22 + sin(t·1.1·2π)·BOB_SPEED   // BOB_SPEED = 0 (disabled)
    pathTarget = atan2(climbRate, max(|activeSpeed|, PATH_REF_SPEED=34))
    pathAngle += (pathTarget − pathAngle)·damp(PITCH_LAMBDA=6.0, dt)
    pos.x += sin(h)·activeSpeed·dt;  pos.z += cos(h)·activeSpeed·dt;  pos.y += climbRate·dt
else:                                    // zoom, stall, dive, recover
    activeSpeed = airspeed·cos(pathAngle);  climbRate = airspeed·sin(pathAngle);  climbVel = climbRate
    pos += (sin h·activeSpeed, climbRate, cos h·activeSpeed)·dt
    activeSpeed = airspeed                // report path speed
// strafe (all modes), + = right:
pos.x −= cos(h)·strafeVel·dt;  pos.z += sin(h)·strafeVel·dt
```
Negative airspeed (reverse) moves him backwards along his heading.

### 2.7 Strafe / sideslip (`controls.js:439-441`, `898-904`)
`STRAFE_MAX = 26 m/s`, `STRAFE_ACCEL = 90 m/s²`, `STRAFE_DAMP = 4.2`.
```
strafeIn = (strafeL ? −1 : 0) + (strafeR ? +1 : 0)     // pad Square = L, Circle = R
strafeVel += strafeIn·90·dt
if strafeIn == 0: strafeVel *= exp(−4.2·dt)
strafeVel = clamp(strafeVel, ±26)
```
Heading is unchanged by strafe.

### 2.8 Attitude: bank and pitch (visual, but the shot aims off the head so it matters) (`controls.js:434-445`, `1158-1207`)

Bank: `MAX_BANK = 1.00 rad` (~57°), `BANK_LAMBDA = 4.6`. Knife: `KNIFE_ANGLE = 1.45`, `KNIFE_TREMBLE = 0.022`.
```
bankScale = clamp(activeSpeed / 55, 0, 1)                  // no bank when hovering (spins flat like a helicopter)
bank = ((yawRate / max(yawMaxNow, 1e-4))·1.0 − (strafeVel/26)·0.30)·bankScale
kb = |knifeAmount|
tremble = sin(t·18.6)·sin(t·7.8)·0.022·kb
targetRoll = knifeAmount·1.45 + (1 − kb)·bank + tremble
if barrel roll running: currentRoll = rollEntry + rollDir·(θ − 0.35·sin θ),  θ = rollT·2π
else: currentRoll += (targetRoll − currentRoll)·damp(4.6, dt)
```
Pitch: `PITCH_MAX = 0.72` (41°), `PITCH_LAMBDA = 6.0`, `AOA_SLOW = 0.22`.
```
slowT = 1 − clamp(activeSpeed / (55·2), 0, 1)
pitchLimit = manoeuvring ? π/2 + 0.08 : 0.72
targetPitch = clamp(pathAngle + (manoeuvring ? 0 : slowT·0.22) + kb·0.14
                    + (rollT > 0 ? sin(rollT·2π)·0.13 : 0), ±pitchLimit)
currentPitch += (targetPitch − currentPitch)·damp(6.0, dt)
```
Nose is held high when slow ("hanging off his wings"), nearly on the path when fast ("a thrown spear").

### 2.9 Knife edge (hold) and barrel roll (double-tap) (`controls.js:263-353`, `457-469`, `563-585`, `768-807`, `1092-1146`)

**Knife edge** (hold knifeL / knifeR; pad L1/R1): `KNIFE_IN = 3.2`, `KNIFE_OUT = 5.2`, `KNIFE_HOLD = 3.0 s`, `KNIFE_RECOVER = 2.0 s`, `KNIFE_SINK = 22 m/s`, `KNIFE_YAW = 0.35 rad/s`.
```
knifeWant = knifeL ? +1 : knifeR ? −1 : 0      (R overrides L if both)
rollGate.update(dt, knifeWant); if rollGate.fired: startRoll(rollGate.dir)
if rollDir != 0: knifeWant = 0
if knifeWant != 0 || rollDir != 0: knifeCharge = max(0, knifeCharge − dt)
else: knifeCharge = min(3, knifeCharge + dt·(3/2))           // 1.5 s per s refill
stamina = smoothstep(knifeCharge/3, 0, 0.4)                    // sags out of it as he tires
knifeTarget = knifeWant·stamina
knifeAmount += (knifeTarget − knifeAmount)·damp(|knifeTarget| > |knifeAmount| ? 3.2 : 5.2, dt)
heading += knifeAmount·0.35·dt                                  // slices toward low wing, OUTSIDE the yaw clamp
climbRate −= |knifeAmount|·22                                    // loses lift (level model only)
```
**Barrel roll** (double-tap a knife key; `rollGate = tapGate(ROLL_TAP_MAX 0.26, ROLL_GAP 0.32)`). It is a dodge: a helix that steps him sideways and LEAVES him there.
- Refused if: already rolling, snared, mode not `level`/`drop`, or `knifeCharge < ROLL_COST (0.85 s)`. On start: mode = level; `knifeCharge −= 0.85`; `airspeed *= (1 − ROLL_DRAG 0.07)`; `rollEntry = currentRoll`; with `t = speedT`: `rollTime = lerp(0.58, 0.46, t)`, `bulge = lerp(2.5, 5, t)`, `rise = lerp(4, 8, t)`, `shift = lerp(20, 34, t)` metres.
- Per frame:
```
rollT = min(1, rollT + dt/rollTime);  θ = rollT·2π
swept = (θ − 0.35·sin θ)/(2π)                         // ROLL_EASE = 0.35
lat = rollDir·(bulge·(1 − cos θ) + shift·swept)       // + = toward his LEFT for rollDir +1
up  = rise·sin θ·(sin θ > 0 ? 1 : ROLL_UNDER 0.45)   // flattened underneath
pos += left·(lat − rollLat);  pos.y += up − rollUp;  rollLat = lat; rollUp = up   // applied as deltas
rollRate = rollDir·(1 − 0.35·cos θ)/(1 + 0.35)        // −1..1, for the wing rig
on rollT >= 1: currentRoll −= rollDir·2π; clear roll state
```
- Pitch adds `sin(θ)·0.13` (nose up over the top, down the far side). Positive g throughout; never inverted.
- Roll is cancelled (with `currentRoll −= rollDir·2π`) by snare, bleedSpeed (crash), landing (`clearRoll`), cutscenes.

### 2.10 Snare (enemy bola hit) (`controls.js:390-416`, `816-833`, `1393-1403`)
`snare(seconds = 4.0)`: `snared = max(snared, seconds)`; cancels roll and manoeuvre (mode = level); `airspeed *= 0.55`. While snared: `snared −= dt`; turn input crossing ±0.55 in the opposite direction of the last crossing removes `SNARE_SHAKE = 0.62 s` (wiggle out); `turnInput *= SNARE_STEER 0.22`; climbVel driven to −26 (λ 6), airspeed to 32 (λ 2.2); double-taps refused. `SNARE_SINK = 26` deliberately equals the impact model's safe closing speed, so a snare ending on the ground does no damage. Cleared on landing and cutscenes.

### 2.11 Speed loss API used by collisions
- `scrapeSpeed(f)`: `airspeed *= 1−f; climbVel *= 1−f; if f > 0.2: recoverHold = max(recoverHold, min(1.2, 1.6f))` (`controls.js:1370`).
- `bleedSpeed(f)`: `airspeed *= 1−f; climbVel *= 0.2; mode = level`, clear recoverHold/diveCommit, cancel roll; `if f >= 1: recoverHold = max(recoverHold, 1.0)` (`controls.js:1376`).
- `clearKeys()`: zero keys, strafeVel, pathAngle, yawRate; mode level (used on pause/console).

### 2.12 Terrain & water collision (the "flight floor") (`main.js:2476-2612`)
Runs every frame after the flight update when airborne (and `tuning.collide`).
```
rock = solidAt(pos.x, pos.z)                          // heightfield, or max with building tops
reach = speed·0.25                                    // a quarter-second look-ahead (14 m cruise, 84 m flat out)
for i in 1..3: rock = max(rock, solidAt(pos + fwdXZ·(reach·i/3)))
floor = max(rock + 6, seaLevel + 10)                  // seaLevel = 0
if pos.y < floor:
    under = solidAt(pos)
    // surface normal from slopes over a fixed 25 m baseline (IMPACT_BASELINE), NOT the look-ahead
    a = fwdXZ = (sin h, cos h)
    gF = (solidAt(pos + a·25) − under)/25;  gS = (solidAt(pos + (−a.z, a.x)·25) − under)/25
    gx = a.x·gF − a.z·gS;  gz = a.z·gF + a.x·gS;  inv = 1/hypot(gx, gz, 1)
    n = (−gx·inv, inv, −gz·inv);  v = (a.x·sp, vy, a.z·sp)   // sp = getSpeed, vy = climbVel (strafe ignored)
    hit = health.impact(v, n, water = under <= seaLevel)
    if hit > 0: bleedSpeed(1)                        // a real crash stops him dead
    else:
        into = max(0, −(v·n)/|v|)
        if firstContactFrame && into > 0.3: bleedSpeed(1)
        else: lose = firstContactFrame ? min(0.85, 1.4·into) : 1 − exp(−dt·(0.9 + 4·into))
              scrapeSpeed(lose); if !firstContactFrame: health.scrape(sp, dt)
    pos.y = floor
```
- The look-ahead lifts him over ridges "the way air does" (pilot aid); he only hits hard when flying into a cliff face.
- **Water:** no swimming and no water skimming — the floor holds him ≥ 10 m above the sea. Fishing is a scripted 4.2 s cutscene path (`storyCtx.fishDive`, `main.js:1627`, path at `2440-2469`).
- **Health impact** (`health.js:25-35`, `118-129`): `closing = −(v·n)`; ≤ `SAFE_CLOSING 26 m/s` → no damage; damage = `100·t^1.5` with `t = min(1, (closing−26)/(130−26))`, ×0.5 into water, ×1.25 for a dive into flat ground (`n.y > 0.75 && vy < 0`). 0.9 s hit grace. Regen after 6 s at 7 HP/s. Scrape: above 45 m/s, every 0.3 s of contact, `(speed−45)·0.02`. Damage can be turned off in settings (`damage: true` default). At 0 HP: "down" screen, respawn 28 m above nearest gentle dry ground, `bleedSpeed(1)` (`main.js:576-607`).
- Camera boom also avoids terrain (§5).

### 2.13 Altitude limits and world bounds
- **No altitude ceiling** in the player model (NPC flocks have one; not the player).
- Floor: §2.12.
- **World edge = a current, not a wall** (`controls.js:355-378`, `868-890`). Chebyshev (square) distance. `chart(v) = v·4.5` (`terrain.js:57`, `worldscale.js`), so `EDGE_SOFT = 24,300 m`, `EDGE_HARD = 28,800 m`, `EDGE_TURN = 0.62 rad/s`. World heightfield is 45 km square centred on 0.
```
cheb = max(|x|, |z|);  edgeT = clamp((cheb − 24300)/(28800 − 24300), 0, 1)
if edgeT > 0:
   home = atan2(−x, −z);  d = angleDelta(heading, home)
   outward = clamp(−cos d, 0, 1)                    // only when pointed outward
   pull = 0.62·edgeT²·outward
   heading += sign(d)·min(pull·dt, |d|)             // outside the yaw clamp
```
HUD prompt at edgeT > 0.02 ("The last of the chart. He is drifting back.") / > 0.45 ("He will not go further…").
- **Wind:** a prevailing wind bearing exists (`terrain.js:70`, 2.15 rad) for visuals only. **Wind does not affect the player's flight.**

### 2.14 Flapping / gliding
Purely animation (`wings.js`), no effect on motion except a small vertical "heave" added to position (§10). There is no flap-to-thrust or glide physics; hover, glide and flap are all expressions of the speed/climb state.

---

## 3. Takeoff, landing, ground

### 3.1 Landing (`main.js:1183-1236`, `2769-2825`, `land()` `1316-1360`)
Evaluated each airborne frame:
```
ground = surfaces.at(x, z, y)            // terrain or roofs/decks/crates (raycast BVH)
overLand = ground.solid && ground.y > seaLevel + 0.5
agl = y − (ground.y + 6)
slow = speedT < LAND_SPEED (0.18)        // ≈ 60 m/s, i.e. at/below cruise
busy = an interactable within its range  // context key prefers "use"
tooSteep = overLand && agl < 22 && groundAttitude(...).steep > LAND_SLOPE_MAX (0.95 rad ≈ 54°)
canLand = overLand && agl < LAND_AGL (22) && slow && !busy && !tooSteep
sinking = verticalSpeed < −3
if canLand && (holdLandKey || sinking):
    landHold += holdLandKey ? dt : 0.6·dt;  if landHold > 0.4: land()
elif !holdLandKey: landHold = max(0, landHold − 2·dt)
```
So **holding the land/use key for 0.4 s, or simply descending (down key/trigger) for ~0.67 s over good ground at low speed, lands him.** Prompt layer names the failing condition and the fixing key: "Hold I or K to land", "No land below — find an island" (if agl < 140), "Too fast to land — let go of W", "Too steep to stand", "Too high to land — hold K to drop" (agl < 260).

`groundAttitude(x, z, nx, nz)` (`main.js:1248-1274`): samples drawn height at ±`FOOT_R = 3.2 m` fore/aft/left/right; `pitch = atan2(hF − hB, 6.4)`, `roll = atan2(hR − hL, 6.4)`, `steep = hypot(pitch, roll)`.

`land()`: grounded = true; `walkYaw = heading`; clear snare and roll; reset attitude node; remove wing heave from y; release wing/flight rigs; `groundBody.begin(dragon, camVel, walkYaw)` (the incoming velocity is the camera's smoothed measured velocity, `main.js:1342`); begin wing fold animation; release aim.

### 3.2 Takeoff (`main.js:1596-1618`)
Triggers: up key pressed while grounded (keydown, `main.js:1302`); pad up-trigger value > 0.6 (`main.js:2766`); automatically when he walks/falls off an edge or onto water (§3.4); `teleport`/`respawn`.
Effect: grounded = false; `controls.setHeading(walkYaw)`; `position.y += 3`; begin wing unfold (wings blend open over the flight animation). The flight model resumes with whatever `airspeed`/`climbVel` it had when he landed (it was frozen while grounded; landing required < ~60 m/s), and with the up key still held he rises at the lift rate.

### 3.3 On foot: walking/running (`main.js:1188-1218`, `2667-2768`) — CAMERA-RELATIVE
Constants: `WALK_SPEED = 5.5 m/s` (brisk trot), `RUN_SPEED = 15.0` (gallop, hold Shift), `WALK_TURN = 7.0 rad/s`, `RUN_TURN = 3.6 rad/s`, `WALK_ACCEL = 7.0`, `RUN_ACCEL = 3.6` (1/s, the drive gain), `REVERSE_CONE = 120°`, `REVERSE_FRAC = 0.5`. (README's 4.2 / 14 m/s are stale.)
```
fwdIn = W − S;  sideIn = D − A;  running = Shift
pad: lx, ly with extra 0.12 per-axis deadzone; fwdIn −= ly; sideIn += lx; if hypot(lx,ly) > 0.92: running = true
mag = min(1, hypot(fwdIn, sideIn))
camFwd = camYaw + π                                 // away from camera
wish = (sin camFwd·fwdIn + sin(camFwd − π/2)·sideIn,  cos camFwd·fwdIn + cos(camFwd − π/2)·sideIn)
gearT = clamp((|walkSpeed| − 5.5)/(15 − 5.5), 0, 1)
align = 1; reversing = false
if mag > 0.01 && !flaring:
    off = angleDelta(walkYaw, atan2(wish.x, wish.z))
    if |off| > 120° && walkSpeed < 1.5:              // asked to go behind him from a walk/stand: BACK UP
        reversing = true
        back = angleDelta(walkYaw, atan2(−wish.x, −wish.z))
        walkYaw += clamp(back, ±7.0·0.35·dt)
        align = −max(0, cos back)
    else:
        walkYaw += clamp(off, ±lerp(7.0, 3.6, gearT)·dt)
        align = max(0, cos(angleDelta(walkYaw, atan2(wish))))   // goes along his nose, never crabs
gear = reversing ? 5.5·0.5 : (running ? 15 : 5.5)
want = (sin walkYaw, cos walkYaw)·mag·align·gear
groundBody.step(dt, {want, accel: running ? 3.6 : 7.0, yaw: walkYaw}); groundBody.apply(dragon)
walkSpeed = groundBody.velocity · (sin walkYaw, cos walkYaw)
```
There is **no jump** on foot. Up = take off.

### 3.4 Ground body physics (`groundbody.js`)
Rigid body on four spring-damped legs. Feet (model frame, nose at −z): `(±0.42, −1.88)`, `(±0.39, −0.86)`; `hipY = 1.0` (`main.js:1175-1179`). COM = mean of feet at hip height.
- `G = 9.81`, `MASS = 1`, `LEG_HZ = 2.0`, `LEG_ZETA = 0.85`, `K_LEG = ω²/4`, `D_LEG = 2ζω/4` (ω = 2π·2). Rest length `L0 = hipY + G/(4K)`; `LMAX = 1.18·hipY`.
- Each leg: hip → `surfaces.at(hip.x, hip.z, hip.y + 0.25)`; `len = hip.y − ground.y`. If `len < LMAX`: force `max(0, K·(L0 − len) − D·dlen/dt)` along the surface normal (contributes torque, load, friction-weighted mu). "Stick" (reaching) leg: when not flaring, airborne < `STICK_TIME 0.3 s`, `LMAX ≤ len < hipY + STICK_DROP 1.4`, normal.y > `STICK_SLOPE 0.55`: pulls down along −normal with `min(STICK_PULL 3g/4, K·4·(len − L0) + max(0, D·dlen))` — keeps him glued over brows at a gallop.
- Grip (supported): `f = (want − vel)·accel − sumForces` (horizontal), capped at `mu·load` (Coulomb). Surface friction: terrain 0.9, registered props default 0.75 (`surfaces.js`). Steep slopes beat friction → he slides.
- Flare (after landing, until first contact; gravity off): horizontal `vel *= exp(−2.2·dt)` (`FLARE_BRAKE`); vertical driven toward `−lerp(1.8, 9, clamp(heightAboveGround/8, 0, 1))` at damp(4) (`FLARE_SINK = 9`, `FLARE_TOUCH = 1.8`).
- Walls: if something ahead (1.75 m along velocity) is taller than `STEP_MAX = 1.1 m` above the feet, remove the velocity component into it.
- Belly floor: COM never below ground + 0.5·hipY (then vy = 0, horizontal ×0.92).
- Attitude: damped spring toward the plane through the four foot heights: `ATT_HZ = 3.0`, `ATT_ZETA = 0.8`, clamp `ATT_MAX = 0.7 rad`. Topple: with ≤ 2 feet on and COM > 0.08 m outside them, accelerate outward at `0.7·g`.
- Auto-takeoff from the ground (`main.js:2737-2741`): (`!flaring && airborne > 0.55 s`) OR (no solid under && !flaring && !supported) OR (no solid under && height above it < 1.2 m, i.e. walked onto water).
- Touchdown rumble scaled by `|vy|/9 + 0.2`.

### 3.5 State transitions summary
```
FLYING --(canLand && (hold land 0.4 s || sinking ~0.67 s))--> GROUNDED(flaring) --(first foot contact)--> GROUNDED(walking)
GROUNDED --(Space / up trigger > 0.6 / off an edge / onto water)--> FLYING (y += 3, heading = walkYaw)
FLYING.mode: level <-> drop ; level -> zoom -> (recover | stall -> dive) ; level -> dive -> recover -> level
Any -> level on snare / crash (bleedSpeed)
```
The "S reverses" room fix in commit `8eb74dd` concerns the indoor prologue scene (`prologue.js`), a separate small controller; outdoors the equivalent is the reverse cone above.

---

## 5. Camera (`main.js:213-305`, `354-358`, `430-480`, `1053-1077`, `2391-2432`, `2945-3226`)

### 5.1 Philosophy — what "camera-consistent controls" means here
- **Yaw/pitch of the camera belong to the player.** Only mouse/right stick, the explicit recenter key, and the slow idle auto-trail change them. What is smoothed is the tracking position and boom length, never the orientation toward his heading ("smoothing orientation toward the target's heading is what makes a camera feel like it's fighting you").
- **In the air**, the mouse never steers the dragon. W/S/A/D act on the dragon's own heading (A/D turn him); the camera trails in behind him automatically ~0.7 s after you stop looking around. H (D-pad up) turns the dragon to face where the camera looks; Q (D-pad down / R3) swings the camera behind him.
- **On the ground**, keys are camera-relative (W = away from camera, A = camera-left) and the camera only drifts behind him very slowly (so it doesn't move the ground under the keys).
- **Aiming does not change what the mouse does** — the mouse still swings the camera; aim only pulls the camera over his right shoulder and turns his head toward the crosshair. (A previous version where aim made the mouse drive his neck, with a scope inside his head, slowed time and had aim stamina, was explicitly hated and removed.)

### 5.2 Orbit parameters
- `camYaw` (init 0) is the **bearing from the focus to the camera** in the same convention as heading (so "behind him" is `heading + π`). `camPitch` (init 0.3) positive = camera above focus. `PITCH_LIMIT = 1.5`.
- Boom direction: `boomDir = (cos p·sin y, sin p, cos p·cos y)`.

### 5.3 Look input
- **Mouse** (pointer-locked; click to capture): discard events with `|movementX|` or `|movementY| > 160 px` (`LOOK_SPIKE`); `sens = 0.003 rad/px · lookSpeed (default 1)`; `pendingYaw −= movementX·sens·invX`; `pendingPitch −= movementY·sens·invY` (invert settings default off). Each frame drain: `k = damp(34, dt)`; apply `dYaw = pendingYaw·k`, `dPitch = pendingPitch·k`, subtract from pending. Then `camYaw += dYaw`, `camPitch = clamp(camPitch + dPitch, ±1.5)`.
  - Result: mouse right → view turns right. Mouse **up** (movementY < 0) → `camPitch` increases → camera rises over him and the view tilts **down** (an "orbit-drag" convention). Pad matches the mouse exactly. Preserve this default but expose invert.
- **Right stick** (radial deadzone + curve, §6.3): `camYaw −= rx·2.9·padSpeed·dt·padX`; `camPitch = clamp(camPitch − ry·2.0·padSpeed·dt·padY, ±1.5)` (`PAD_LOOK_YAW = 2.9`, `PAD_LOOK_PITCH = 2.0` rad/s). Not buffered.
- Any look input stamps `lastLookAt` and cancels recentering.
- **D-pad left/right**: boom base distance `distBase = clamp(distBase ± 22·dt, 6, 46)` (default 11).

### 5.4 Auto-trail (air) and recenter
- Air, not recentering, not cutscene, after `TRAIL_DELAY = 0.7 s` of no look input:
```
ramp = clamp(idle − 0.7, 0, 1);  lam = lerp(0.8, 3.6, speedT)
camYaw += angleDelta(camYaw, heading + π)·damp(lam·ramp, dt)
camPitch += ((0.26 − camPath·0.55) − camPitch)·damp(lam·ramp·0.5, dt)   // follow flight path: dive shows the sea
```
- Recenter (Q, pad R3 or D-down): `camYaw += angleDelta(camYaw, heading + π)·damp(7, dt)` until within 0.01 rad.
- Ground: after 1.5 s idle, `ramp = clamp(idle − 1.5, 0, 1)`; `away = max(0, cos(angleDelta(camYaw + π, walkYaw)))`; `moving = clamp(groundSpeed/5.5, 0, 1)`; `camYaw → walkYaw + π` at `damp(0.9·ramp·away²·moving)`; `camPitch → 0.06` at `damp(1.2·ramp)`.

### 5.5 Focus point (what the camera looks at)
```
speed = grounded ? 0 : getSpeed();  speedT = grounded ? 0 : speedRatio
camPath += (pathAngle − camPath)·damp(2.4, dt)                    // grounded: 0
lead = 10·speedT^1.5                                              // LEAD_MAX 10 m, LEAD_CURVE 1.5
camLead.xz += ((sin h, cos h)·cos(camPath)·lead − camLead.xz)·damp(5, dt)
camLead.y  += (sin(camPath)·lead − camLead.y)·damp(3, dt)
camTarget = pos + camLead + (0, 1.5 − wingHeave, 0)               // LOOK_HEIGHT 1.5; wing heave removed
camVel = lerp(camVel, Δpos/dt, damp(8, dt));  teleport if |Δpos| > max(60, 0.6·speed + 20) → camVel = lag = 0
trackXZ = max(18, speed/2);  trackY = max(7, speed/6)             // FOCUS_LAMBDA_XZ 18, _Y 7, FOCUS_MAX_LAG 2 m
focusLag.xz += (−camVel.xz/trackXZ − focusLag.xz)·damp(10, dt)
focusLag.y  += (−camVel.y/trackY − focusLag.y)·damp(6, dt)
focus = camTarget + focusLag
```
The camera is smoothed **relative to the focus** (not in world space) so uneven frame times don't jitter him on screen.

### 5.6 Boom, occlusion, lag
```
dist = grounded ? 10.5 : distBase(11) + 10·speedT                 // DIST_SPEED 10
lift = grounded ? 1.8 : 2
// occlusion: march 12 steps (OCCLUDE_STEPS) along the boom
for i in 1..12: t = i/12; p = focus + boomDir·dist·t + (0, lift·t, 0)
    margin = p.y − (max(height(p.xz), sea) + 5)                    // OCCLUDE_CLEAR 5 m
    if margin < 0: clear = (i − 1 + prevMargin/(prevMargin − margin))/12; break
boomScale += (clear − boomScale)·damp(clear < boomScale ? 9 : 2.2, dt);  clamp [0.1, 1]
armed = max(dist·boomScale·(1 − 0.45·aimBlend), 9)                 // AIM_BOOM 0.55, BOOM_HARD_MIN 9
desired = focus + boomDir·armed + (0, lift, 0) + right·4.2·aimBlend + (0, 1.6·aimBlend, 0)
          // right = normalize(boomDir.z, 0, −boomDir.x); AIM_SHOULDER 4.2, AIM_RISE 1.6
lagV = −camVel / max(9, speed/12), length capped at 12 m          // BOOM_LAMBDA 9, BOOM_MAX_LAG 12
camRel = lerp(camRel, desired − focus + lagV, damp(9, dt))
camera = focus + camRel
// terrain/sea floor for the camera
minY = max(height(cam) + 3, sea + 4); if cam.y < minY: cam.y += (minY − cam.y)·damp(18, dt); cam.y = max(cam.y, minY − 2.5)
lookAt(focus)   // aiming: lookAt(focus + right·4.2·k + flatFwd·30·k + (0, 1.6·k, 0))
```
First frame: place focus and camera directly (no spring-in).

### 5.7 FOV and lean
```
vFOV_target = fovForAspect(70 + 24·speedT² + 14·turboT − 12·aimBlend, aspect)   // fovBase 70, FOV_SPEED_GAIN 24, AIM_FOV 12
camera.fov += (target − fov)·damp(3, dt)                                          // FOV_LAMBDA 3
turboT = clamp((activeSpeed − 335.28)/(715.26 − 335.28), 0, 1)
```
70° vertical at a hover/low speed, ~72° at cruise, 94° flat out, up to 108° at full turbo. `fovForAspect` only widens for screens taller than aspect 1.2, max 96° (`main.js:124-132`) — irrelevant on desktop. Remember these are **vertical** FOVs.

Horizon lean in a carve (not while aiming): `camLean += (−turnT·0.20 − camLean)·damp(3.4, dt)`; applied after lookAt as a roll about the view axis (three `rotateZ`, positive = camera rolls counter-clockwise as seen from behind it). Net effect: max 0.2 rad; in a left turn the camera rolls clockwise (to its right). Verify visually in UE and keep it subtle ("any more than this reads as a bug").

Camera shake: a `shake` variable decays at `exp(−6·dt)` with ±0.3·shake random offset (`main.js:3243-3247`), but **nothing currently sets it** — there is effectively no camera shake.

---

## 6. Controls

### 6.1 Keyboard — two schemes (`keymap.js:80-117`), default `wasd`. Rule: one hand steers, the other acts.

| Action | `wasd` scheme (default) | `arrows` scheme | Effect |
|---|---|---|---|
| forward / back | W / S | ↑ / ↓ | speed ladder (§2.2); on foot camera-relative fwd/back |
| turnL / turnR | A / D | ← / → | yaw (§2.3); on foot camera-relative left/right |
| up | Space | Space | lift / trim / double-tap zoom; on ground: take off |
| down | K | C | sink / trim / double-tap drop or dive; lands when sinking over good ground |
| sprint | Shift L/R | Shift L/R | 180 m/s gear; on foot gallop; with flat out = turbo |
| burst (flat out) | J, B | V, B | hold for 335 m/s; double-tap-hold = turbo |
| landUse | I, R | D, R | hold to land (0.4 s) or use nearby thing (0.9 s) |
| fire | U (+ LMB) | F (+ LMB) | plasma blast on key-down, repeats while held |
| sleepfire | Y | T | hold to charge (0.8 s) |
| strafeL / strafeR | O / P | A / S | sideslip |
| knifeL / knifeR | L / ; | Z / X | hold knife edge; double-tap barrel roll |
| aim | E (+ RMB) | E (+ RMB) | over-shoulder aim (air only) |
| alignDragon | H | H | turn him to face camera direction |
| alignCamera | Q | Q | swing camera behind him |
| grid / padView | G / M | G / M | debug overlays |
| map | Tab | Tab | chart |
| legend / console / pause | `/`, `` ` ``, `-` `=` `Esc` | same | UI |

Mouse: move = look (pointer lock, click to capture); LMB = fire (on mousedown); RMB = aim (held). Ctrl is intentionally unbound (macOS Mission Control conflict). Index finger carries fire (upper) and burst (lower) — firing while flat out is meant to be done with LMB, or B+U.

### 6.2 Gamepad (DualSense layout; standard mapping indices `gamepad.js:13-31`)

| Input | Effect |
|---|---|
| Left stick Y | forward/back speed, `sign(f)·f²` (on foot: camera-relative, >0.92 magnitude = run) |
| Left stick X | turn (on foot: camera-relative strafe-direction) |
| R2 / L2 | up / down (analog, `R2 − L2`; `padClimbInvert` default true = R2 up). Double-tap rule applies at 0.55 threshold. On ground R2 > 0.6 takes off |
| Cross (✕) | flat out (held); double-tap-hold = turbo |
| Square / Circle | strafe left / right (Circle also closes the map) |
| L1 / R1 | knife edge left / right; double-tap = barrel roll |
| Right stick | camera look (§5.3) |
| D-pad up | align dragon to camera |
| D-pad down, R3 | recenter camera behind him |
| D-pad left/right | boom distance −/+ |
| L3 | reset cruise speed to 55 (legend claims L3 = aim; there is NO pad aim in code) |
| Triangle | toggle NPC wild flights |
| Touchpad | map; Create: hide HUD; Options: debug console |

### 6.3 Pad shaping (`gamepad.js:34-72`)
- Sticks: radial deadzone `0.10`; `t = min(1, (mag − 0.10)/(0.98 − 0.10))`; `shaped = 0.78·t² + 0.22·t`; output = input·shaped/mag.
- Triggers: deadzone `0.045`, rescaled `(v − 0.045)/(1 − 0.045)`.
- Rumble: rich, event-based (no DC hum in level flight). Brief list (weak, strong): buffet above 55% of 0–180 m/s range `0.40·b²·shake`; carve `0.22·c²`; knife `0.30 + 0.48·strain`; burst hold `(0.30, 0.62)` with kick pulse 0.75/1.0 on press; turbo pulse (0.9, 1, 0.8 s); ground-rush below 110 m AGL `t²·(0.12 + 0.88·speedT)` grained; floor contact thump (0.7, 1.0); crash (1, 1, 0.5); wingbeat downstroke `0.05/0.30·amp·stroke³`; adaptive trigger resistance 0.52–0.82 (R2) / 0.66–1.0 (L2) scaled by climb effort (`controls.js:601-643`, `1209-1308`, `main.js:2501-2630`).

### 6.4 Touch (`touch.js`)
On-screen buttons synthesise the same keyboard events (so the double-tap grammar is identical); drag = look via a custom event using mouse units.

---

## 7. Fire / plasma blast and aiming (brief)

- **Plasma** (`plasma.js:43-81`, `265-340`; `main.js:1385-1415`, `2888-2895`): fires on the press edge (key-down / mousedown), not on release; repeats while held. `FIRE_COOLDOWN = 0.22 s` between shots, `MAX_SHOTS = 6`, one shot returns every `RECHARGE_TIME = 2.6 s`. Muzzle speed `600 m/s` **plus** `max(0, carry·dir)` where `carry = airspeed·(sin h·cos path, sin path, cos h·cos path)` (0 on the ground). Range 1800 m. Hit radius `BLAST_R = 26 m` vs targets; ground hit when `y ≤ max(height, sea) + 1`. Swept in ≤ 6 m sub-steps. Visual core radius 0.62 m, halo ×2.6. Muzzle = head bone + dir·1.4 (fallback origin + dir·5.2 + 0.9 up). Available at all times except cutscenes (standing, walking, mid-roll). Empty magazine = soft "empty" click on press.
- **Direction:** not aiming → along the **head bone's forward axis** (zero error vs. what you see; fallback `heading + aimYaw`, `clamp(pathAngle + aimPitch, ±1.45)`). Aiming → from head to the crosshair world point.
- **Aim** (`aim.js`, `main.js:2252-2256`, `3198-3214`): held (RMB / E), **air only**. `blend` eases at λ 9; head turns toward crosshair at λ 10, limited ±1.20 rad yaw / ±0.78 rad pitch relative to heading/path, distributed 0.20 per neck bone ×3 + 0.40 head. Crosshair point = screen-centre ray marched over terrain/sea from t = 20 m, step 4 m (to 200 m) then 12 m, to 1500 m. FOV −12°, boom ×0.55, shoulder offset 4.2 m right + 1.6 m up. **No time slow-down (`timeScale = 1`), no aim stamina.**
- **Sleepfire** (`player.js:19`, `99-119`): hold the key, charges over `CHARGE_TIME = 0.8 s` then fires; releasing early drains charge at 1.7/s with no penalty. Gated only on having learned it (food/rest costs removed).

---

## 8. Speed effects (brief) (`speedfx.js`, `main.js:3071-3083`, `3238-3242`)
- **What is actually live:** FOV widening (§5.7: 70 → 94° vertical at flat out, +14° turbo); boom lengthens +10 m with speed; camera lead up to 10 m along the path (dragon sits lower in a frame "full of oncoming archipelago"); focus lag capped at 2 m; boom pull-away lag up to 12 m under hard acceleration; horizon lean; music switches to the big cue when flat out/zoom/dive (held 9 s).
- **Disabled in code:** world-fixed wind streaks (`ENABLED = false` — "read as a cheap effect"; were 900 motes in a ±90 m box, opacity smoothstep 90→330 m/s, length `min(60, 0.05·speed)`), the vapour cone ("read as a goofy dome"), and the Mach ring ("read as an impact wave"). Crossing Mach 1 (343 m/s, hysteresis +5/−20) still fires a rumble pulse (1, 1, 0.45). README's mention of streaks/shake is stale.
- No camera shake is applied (§5.7).

---

## 9. Feel essentials (from code comments, README, and the user's stated preferences)

Keep:
1. **Flying-mount scheme, one key one job.** W moves and never changes altitude; Space/down change altitude and never change speed. Let go of W and he hovers — no mode, no toggle.
2. **Press = gentle, double-tap-and-hold = committed**, everywhere (zoom/dive/drop, barrel roll, turbo). Ordinary use of a key must never turn into a committed manoeuvre by accident.
3. **Honest speeds in m/s and life-size dragon** (cruise 123 mph, sprint 403, flat out 750 = canon top speed, turbo 1600). Frame-rate independent everything.
4. **The turn cap is lateral-g limited**, so top speed is self-limiting (huge radius) — this is why flat out needs no cooldown.
5. **Energy trade** in the zoom/stall/dive; the stall is the only time control is taken away.
6. **Camera owned by the player**, smoothed in position not orientation, auto-trails only after you stop looking, never fights you. Ground controls camera-relative.
7. **Terrain look-ahead pilot aid** lifts him over ridges; only real cliff impacts hurt; skimming must be free.
8. **Weapon always answers** (fires on key-down, works in every state; limit is the 6-shot magazine, not a long cooldown).
9. Heavy, slow wingbeat (< 1 Hz) and flap-glide at cruise; nose high when slow, spear-like when fast; bank fades in with speed (flat spin at hover).
10. Context prompts name the failing condition and the key that fixes it.

**Explicitly disliked / removed (do NOT reintroduce without asking):**
- Chore/upkeep mechanics: sleepfire food & rest costs ("fishing is annoying"), brazier wing-gust snuffing ("super annoying"), side-trips between actions.
- **Cooldowns and charge meters**: flat out used to be a 1.9 s burst with a 5.5 s cooldown and charge meter — replaced by a held gear. Aim stamina/cooldown removed.
- **Modal controls**: the scoped head-cam aim where the mouse drove his neck and time slowed — removed; aim must keep the normal camera behaviour.
- Tap-vs-hold disambiguation on one key (fire on release lag) — fire must leave on key-down.
- Old W = pitch-up flight-path-angle control (a hovering dragon couldn't climb), tank steering on foot with a camera that swung itself, per-frame (non-dt) movement.
- Hair-trigger time gates for committing (0.42 s hold) — replaced by double-tap.
- Gimmicky speed visuals (streaks, vapour cone, shock ring).
- Note: the **knife-edge/barrel-roll wing stamina (3 s bar)** is still in the code; it is the one remaining stamina-like meter. Port faithfully, but be aware of the user's stance.
- (Also from user memory, for world-building, not movement: terrain must be natural/organic, never geometric or terraced.)

---

## 10. Animation-side hooks (only what the movement model must output)
`getFlightState()` (`controls.js:1467-1504`) feeds the wing/body rig every frame:
- `climb = clamp(climbVel/140·3, −1, 1)`, `speedT`, `knife = knifeAmount`, `turn = turnT`,
- `load = hypot(yawRate·airspeed, vertAccel)/9.81` (g; `vertAccel` = smoothed d(climbVel)/dt at λ 6),
- `hang = stall ? 1 : zoom ? max(0, (stallT − 0.45)/0.55) : 0`, `roll = rollRate`.

Wingbeat (`wings.js:56-88`, `117-197`): frequency `lerp(lerp(0.72, 0.6, fastK), 0.95, slowK) + 0.22·climbK` Hz × `(1 − 0.85·hang)(1 − 0.7·|roll|)(1 − 0.75·glideK)`, min 0.12 Hz; `slowK = 1 − smoothstep(speedT, 0.04, 0.32)`, `fastK = smoothstep(speedT, 0.4, 0.85)`. Flap-glide: when `slowK < 0.25 && climb < 0.15 && |roll| < 0.1 && hang < 0.1`, after 3–5 beats glide for `1.2 + rand·1.6 + 1.2·fastK` s. **Body heave** `= −wave·lerp(0.07, 0.24, slowK)·(amp/0.6)` metres is added to the actor's Y each frame as a delta (`main.js:2638-2640`) and removed again for the camera target and on landing — in UE apply it to the mesh, not the actor/collision.
Flight rig (`flightrig.js`): legs tucked, tail fins as rudder/elevator from `turn`/`climb`, head leans into turn (0.42 rad), membrane camber with load — cosmetic.
Gait (`gait.js:54-75`): Froude `v²/(g·hip)` blends walk→trot (0.32–0.62) → gallop (1.9–3.2); stride Hz per gait `(0.75+0.42v, 1.55+0.16v, 2.3+0.07v)·sqrt(1/hip)`; leg IK on planted feet; lean `tan = v·ω/g`.

---

## 11. Quick reference — all flight constants (`controls.js`)

```
SPEED_MIN 0 | SPEED_CRUISE 55 | PEDAL_MAX 180 | BURST_SPEED 335.28 | TURBO_SPEED 715.26 | REVERSE_SPEED 9
PEDAL_GAIN 1.5 | PEDAL_BLEED 0.9 | BURST_GAIN 5.5 | BURST_BLEED 1.7 | TURBO_GAIN 0.75 | turboGate(0.5, 0.6)
VERT_HOVER 20 | VERT_PER_SPEED 0.62 | CLIMB_RATE_CAP 140 | VERT_LAMBDA 7.5 | VERT_RELEASE 5.5 | TRIM_RATE 52
CLIMB_ENTRY_SPEED 105 | DIVE_ENTRY_SPEED 105 | TAP_MAX 0.30 | DOUBLE_GAP 0.34 | MANOEUVRE_HOLD 0.10 | axis threshold 0.55
CLIMB_DRAG 34 | CLIMB_DRAG_V2 6e-4 | STALL_SPEED 15 | STALL_HANG 0.5 | NOSE_OVER_RATE 2.2 | DIVE_GRAVITY 36 | DIVE_MAX 402.3
STALL_DIVE_COMMIT 1.1 | RECOVER_RATE 2.6 | RECOVER_HOLD 1.1 | stall→dive at pathAngle < −0.7 | recover→level at |pathAngle| < 0.10
DROP_SINK 75 | DROP_LAMBDA 3.0 | DROP_SPEED 90.3 | DROP_DRAG 1.4
YAW_RATE_MAX 1.45 | TURN_G 196 | YAW_LAMBDA 3.6 | YAW_DAMP 6.0 | YAW_DEADZONE 0.02 | align gain 1.6, tol 0.02
MAX_BANK 1.00 | BANK_LAMBDA 4.6 | strafe bank share 0.30 | bankScale speed 55
STRAFE_MAX 26 | STRAFE_ACCEL 90 | STRAFE_DAMP 4.2
PITCH_MAX 0.72 | PITCH_LAMBDA 6.0 | AOA_SLOW 0.22 (over 0..110 m/s) | PATH_REF_SPEED 34 | BODY_CENTRE 1.45
KNIFE_ANGLE 1.45 | KNIFE_IN 3.2 | KNIFE_OUT 5.2 | KNIFE_HOLD 3.0 | KNIFE_RECOVER 2.0 | KNIFE_SINK 22 | KNIFE_YAW 0.35 | KNIFE_TREMBLE 0.022 | knife pitch +0.14
ROLL_TIME 0.58→0.46 | BULGE 2.5→5 | RISE 4→8 | SHIFT 20→34 | ROLL_EASE 0.35 | ROLL_UNDER 0.45 | ROLL_PITCH 0.13 | ROLL_COST 0.85 | ROLL_DRAG 0.07 | ROLL_TAP_MAX 0.26 | ROLL_GAP 0.32
EDGE_SOFT 24300 | EDGE_HARD 28800 | EDGE_TURN 0.62
SNARE_TIME 4.0 | SNARE_SINK 26 | SNARE_SPEED 32 | SNARE_DRAG 2.2 | SNARE_STEER 0.22 | SNARE_SHAKE 0.62 | snare airspeed ×0.55
Floor: rock + 6, sea + 10; look-ahead 0.25 s × speed, 3 samples; impact baseline 25 m
Land: AGL < 22 (above ground+6), speedT < 0.18, slope < 0.95 rad, hold 0.4 s (sinking < −3 m/s at 0.6 rate); takeoff y += 3
Walk 5.5 | Run 15 | turn 7.0→3.6 rad/s | accel 7.0 / 3.6 | reverse cone 120°, ×0.5, below 1.5 m/s
```
