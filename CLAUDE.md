# Helldrop

Reverse-Doom: an AI Doom Guy fights through real Doom levels rendered
isometrically; the player drops demons to kill him before the exit switch.
Full design rationale and roadmap: `DESIGN.md`.

## Build & run

- `./run.sh` — incremental build (make) + launch, attached to the terminal.
  First run clones/builds static **raylib 6.0** into `vendor/` and downloads
  `assets/freedoom1.wad` (BSD-licensed IWAD). No Homebrew deps.
- raylib is pinned via `RAYLIB_TAG` in the Makefile; upgrade = bump tag +
  `rm -rf vendor/raylib-src`.
- Builds `Helldrop.app` (minimal bundle: Info.plist + icon.icns from
  `assets/icon.png`) so cmd+tab shows the game icon; run.sh execs the binary
  inside it. The window spawns at the monitor's left edge (user preference —
  keeps their terminal visible).
- Binary: `Helldrop.app/Contents/MacOS/helldrop`. Flags: `--wad <path>`,
  `--map E1M2`, `--no-log`, `--exit-after <frames>`, `--turbo <N>` (sim speed
  multiplier, great for fast AI testing), `--autodrop` (scripted monster
  waves dropped on his route every 250 tics — the combat/balance smoke test;
  expected outcomes: unaided run -> he exits, autodrop run -> he dies).
- `--selftest` — loads the map, renders 60 frames, writes `selftest.png`,
  exits 0. Combine with `--turbo 8` to screenshot mid-simulation. Use this
  (then Read the png) to verify changes without user interaction.
- In-game: G restarts the sim (deterministic — same playout). 1–6 select a
  monster, LMB drops it at the cursor. **Hell's Hand** (each on a 12s
  cooldown): F slams/holds the door — or recalls the lift — nearest the
  cursor (15/20 souls); V detonates the nearest barrel (30 souls; barrels
  are live entities, shootable, chain-reacting). Drops cost souls (the Pain Tithe:
  base drip ramps 4→10/s with his progress toward the exit, +1 soul per
  point of damage dealt to him, no refund for healing he picks up). Drops
  inside his line of sight cost ×1.5 and telegraph for 1.5s before
  spawning; hidden drops are instant. `SimDrop` is the free dev spawn,
  `SimTryDrop` the player path.

## Gamestate log (the debugging contract)

Every run appends NDJSON to `logs/run-<timestamp>.ndjson`
(`logs/latest.ndjson` symlinks the live file; `./debug.sh` tails it).
Events are written as they happen; full gamestate snapshots at 5 Hz.
When the user reports a bug, read the log first — it must always contain
enough state to reconstruct what the game was doing. **Any new gameplay
state added to the code must also be added to the snapshot in the same
change** (`statelog.h` has the writer API).

## Architecture

- `src/wad.c` / `src/map.c` — WAD container + vanilla map lump decoding.
  No NODES/SEGS dependency: floors are tessellated from sector boundary
  loops (libtess2, vendored in `third_party/libtess2`), so any node format
  works.
- `src/levelmesh.c` — extruded flat-shaded geometry (floors + wall quads,
  vertex-color lighting, no ceilings) for the orthographic-Camera3D iso view.
  Also emits `floorTriSector` (sector id per floor triangle) — the sim's
  point->sector lookup and triangle nav graph depend on it.
- `src/sim.c` — fixed 35 Hz deterministic sim (`SimTick`; render loop feeds
  whole tics, lerps visuals between the last two). All randomness goes
  through `SimRandom`. Contains: door state machines (vanilla DR/D1 specials;
  key doors open unlocked for now, logged), Doom Guy FSM
  (ADVANCE/WAIT_DOOR/FIGHT/RETREAT/EXITED/DEAD/NO_GOAL), and navigation.
  Nav is A* over **floor triangles** (convex — straight lines between
  adjacent crossings never hit walls; sector-level portal routing fails
  inside concave sectors and is kept only as fallback), with line-of-sight
  waypoint pruning and a uniform-grid line blockmap for circle-vs-wall
  sliding collision. Nav is **self-healing**: edges he gets stuck against
  are banned for the run (tEdgeBlocked) and he replans around them.
  **Movers & triggers**: doors (DR/D1 + tagged remote), lifts/plats
  (down-wait-up), one-shot floor lowers, floor raises ("risers"), and
  teleports, driven by the vanilla W/S trigger system (`ActionForSpecial`).
  He plays triggers like a speedrunner: crossing fires walkovers, and he
  presses any switch within reach every few tics. Floor heights are live
  everywhere (`SimSectorFloorNow`); lift-sector floors render at their LOW
  height in the static mesh with a dynamic cap drawn on top. Combat: `MOB_INFO` bestiary table (zombieman,
  shotgunguy, imp, pinky, caco — vanilla-ish dice), monster AI
  (IDLE/CHASE/ATTACK/PAIN/DEAD, sight+sound wake, blocked-path wiggle),
  hitscan with tracers, projectiles, the vanilla **infighting** retarget
  rule (different species only), corpses, pickups (he loots zombie clip
  drops — Feeding the Slayer), and his weapon/ammo/armor sim. **The Corpse
  Bank**: corpses persist as a resource; the arch-vile (key 6, 220 souls)
  raises non-gibbed bodies within 120 units at half HP, one per ~3s, free.
  Counterplay hooks: overkill past -spawnhealth gibs a corpse beyond
  raising (vanilla rule), and beyond CORPSES_MAX raisable bodies the
  oldest decay. His flame attack is a 24-tic channel that whiffs if line
  of sight breaks before it lands. Sight and
  hitscan are height-aware: `SightT` interpolates the ray's z through every
  crossed line's vertical opening, so ledge steps and half-open doors block
  shots like real walls. Map things spawn at the Hurt-Me-Plenty skill
  filter (0x02) — that's the tuned baseline. Doom Guy unaided should
  usually clear the map population and exit; player drops are what kill
  him. Balance knobs live in `MOB_INFO`, the monster whiff curve in
  `MobAttack`, and his cadence in `DgFire`.
- `src/doomgfx.c` — Doom picture-format (patch) decoder via PLAYPAL: the
  classic status bar (STBAR), mugshot faces (STFST/STFOUCH/STFEVL/STFDEAD),
  and big red digits (STTNUM). Every lump is optional with text fallbacks,
  so any IWAD works. The mugshot bloodies as the player wins — it IS the
  progress bar.
- `src/sprites.c` — WAD sprites as billboards with the authentic 8-rotation
  system (NAME+frame+rotation lumps, mirrored pairs), per-kind animation
  tables (walk/attack/pain/death; death holds the corpse frame), and an
  alpha-test discard shader so billboards depth-sort like opaque geometry.
  Missing sets fall back to the capsule renderer per entity.
- `src/sound.c` — DMX sound lumps -> raylib Sounds. The sim emits SndEvents
  into a ring (`Snd()` in sim.c, headless/deterministic); GameAudioDrain
  plays them with distance attenuation from the camera target.
- `src/statelog.c` — NDJSON writer.
- `src/main.c` — game loop, iso camera (pan/zoom/rotate), thing markers,
  Doom Guy capsule + route + door panel rendering, HUD.
- Doom coords are x-east/y-north in map units; world mapping is
  `(x, height, -y)`, 1 unit = 1 map unit, everywhere.
