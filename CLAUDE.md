# Helldrop

Reverse-Doom: an AI Doom Guy fights through real Doom levels rendered
isometrically; the player drops demons to kill him before the exit switch.
Full design rationale and roadmap: `DESIGN.md`.

## Build & run

- `./run.sh` — incremental build (make) + launch, attached to the terminal.
  First run clones/builds static **raylib 6.2** into `vendor/` and downloads
  `assets/freedoom1.wad` (BSD-licensed IWAD). No Homebrew deps.
- raylib is pinned via `RAYLIB_TAG` in the Makefile; upgrade = bump tag +
  `rm -rf vendor/raylib-src`.
- **Web build** (`make web`, needs Emscripten and Python on PATH):
  cross-compiles to WebAssembly + WebGL2 into `dist-web/`. The preload WAD
  includes the seven training lessons and Episode 1. The optional
  `models.pack` downloads when the player enters Episode 1. `make web-serve`
  serves on port 8000. `make test-web` requires Node.js 22+ and Chrome to run
  desktop and touch regression tests.
- **On every new release, rebuild and publish all five browser artifacts**.
  Copy `dist-web/index.{html,js,wasm,data}` and `dist-web/models.pack` into
  ximg-web's `helldrop-html/game/`, then follow its release procedure in
  `CLAUDE.md` under *WebAssembly Apps → Helldrop*. The live build at
  helldrop.ximg.app updates through that release process.
- **iOS App Store release** (bundle `app.ximg.helldrop`): the version and
  build number live in `ios/project.yml` (`MARKETING_VERSION`,
  `CURRENT_PROJECT_VERSION`; bump the build for every upload).
  `scripts/ios-appstore.sh` archives, exports and uploads;
  `scripts/asc-submit.py <version> <build>` attaches the build and submits
  (or resubmits a rejected version; `ASC_REVIEW_NOTES_FILE` replaces the
  review notes). The iPhone build skins models on the CPU, like the web
  build: GPU skinning under OpenGL ES 3 stretches vertices into spikes.
- Builds `Helldrop.app` (minimal bundle: Info.plist + icon.icns from
  `assets/icon.png`) so cmd+tab shows the game icon; run.sh execs the binary
  inside it. The window spawns at the monitor's left edge (user preference —
  keeps their terminal visible).
- **Authentic art**: every visual/sound decodes from the loaded WAD, so the
  game looks exactly like Doom when given id's IWAD. WAD precedence when no
  `--wad`: an id IWAD (auto-detected — `assets/DOOM.WAD` any case/doom1.wad,
  or the macOS Steam paths; `assets/DOOM2.WAD` for `--map MAPxx`) →
  `assets/helldrop.wad` (our pack) → `assets/freedoom1.wad` (raw fallback).
  id's assets are copyrighted — never vendor them into the repo; the
  gitignore blocks `assets/*.wad`.
- **The Helldrop asset pack** (`assets/helldrop.wad`): our own
  redistributable WAD, assembled by `tools/mkwad` from the Freedoom base
  (BSD) plus the override lumps committed under `assets/helldrop/`
  (`CREDITS.txt` → CREDITS lump; `graphics/*.png` → Doom patches via the
  base PLAYPAL; raw `*.lmp`/`*.txt`). Unlike an id IWAD this is legal to
  ship. `make assets/helldrop.wad` builds it (a gitignored, reproducible
  artifact); `make bake-hud` re-derives the tinted HUD source PNGs
  (STBAR/STARMS — the hell-forged reskin) from the current Freedoom base
  after a version bump. mkwad reuses the engine's own `wad.c`/`doomgfx.c`
  decoders, so the patch codec lives in one place. To reskin anything, drop
  a PNG named for the lump into `assets/helldrop/graphics/` (optional
  `NAME.png.ofs` = "left top" for sprite offsets) and rebuild.
- Default launch is the **campaign** ("Knee-Deep in His Blood"): E1M1→E1M8
  with the bestiary unlocking per level (`UnlockMaskFor`); win + SPACE
  advances, progress persists in `helldrop.sav` (gitignored). Killing him
  in a secret sector (special 9) on E1M3 routes through E1M9. `--map` pins
  a single level with everything unlocked — use it for ALL testing, since
  campaign mode reads the save file (baselines: `--map E1M1`).
- **`make test`** — the headless gameplay regression matrix
  (`tools/playtest.sh`): every E1 map unaided and with `--autodrop` via
  `--simonly`, ~25 s total. Contract: every match ends, scripted drops kill
  him on every map, he exits unaided on >= 6/9. Run it after ANY sim, nav or
  balance change. Test runs execute a copy outside the .app bundle
  (`build/helldrop-test`): a bundled binary registers as a Dock app per launch.
- Test launches are hidden (`kit_background.h`): `--selftest`, `--exit-after`,
  `--dump-frames` and `--simonly` imply `--bg` (no window, focus, Dock tile or
  sound); `--show` overrides. Never launch a visible window to test.
- Binary: `Helldrop.app/Contents/MacOS/helldrop`. Flags: `--wad <path>`,
  `--map E1M2`, `--no-log`, `--exit-after <frames>`, `--turbo <N>` (sim speed
  multiplier, great for fast AI testing), `--autodrop` (scripted monster
  waves dropped on his route every 250 tics — the combat/balance smoke test;
  expected outcomes: unaided run -> he exits, autodrop run -> he dies),
  `--simonly <tics>` (play the match headless as fast as the CPU allows and
  print one JSON outcome line — the balance harness), `--garrison-skill
  <1|2|4>` / `--garrison-dmg <pct>` (override the map-population tuning).
- `--selftest` — loads the map, renders 60 frames, writes `selftest.png`,
  exits 0. Combine with `--turbo 8` to screenshot mid-simulation. Use this
  (then Read the png) to verify changes without user interaction.
- **Trailer pipeline** (`tools/trailer/`): `--dump-video out.mp4` streams 35 fps
  capture straight into ffmpeg (use `--size 1440x810`; HiDPI makes it 2880x1620,
  scaled to 1080p) and writes `sound.txt` + `snd/*.wav` beside it so the game's
  real sound effects are rebuilt in sync. `HELLDROP_CLEAN_HUD=1` hides the key
  hints, `HELLDROP_FOVY` sets the zoom. Cut lists `cut-long.txt` /
  `cut-short.txt`, narration in `narration.txt` (ElevenLabs), `cards.py`
  (title/end/Short frame), `mix.py` (numpy soundtrack, -14 LUFS, faded edges),
  `edit.py` (ffmpeg assembly — run on the mini host). Outputs in build/trailer/.
- `--dump-frames <dir> [--dump-every N]` — writes `frame_%05d.png` per (Nth)
  rendered frame for stitching gameplay GIF/MP4 with ffmpeg. In this mode the
  sim advances a FIXED `turbo` tics per frame (ignoring wall clock), so
  footage is smooth and reproducible. Media for the README lives in `media/`.
- In-game: G restarts the sim (deterministic — same playout). 1–9 and 0
  select a monster (zombieman, shotgunguy, imp, pinky, caco, lost soul,
  revenant, baron, arch-vile, spectre), LMB drops it at the cursor.
  Darkness is ambush terrain: monsters in sectors under light 120 are
  spotted at half range and score lower on his threat ledger; spectres
  carry their own darkness (treated as light 80 anywhere). Doom 2 heavies
  (shift+1..4: chaingunner, hell knight, mancubus, pain elemental) need
  `make assets/freedoom2.wad` for sprites/sounds; they are excluded from
  the E1 campaign unlocks. Known limit: some Doom 2 maps' progression
  (e.g. MAP01) still dead-ends at NO_GOAL — more floor specials needed. TAB toggles
  the Know-Your-Enemy codex (his published tells — every line must stay
  true to the implementation); a floating intent label over his head
  telegraphs his current drive and flashes on change. C toggles the
  doomcam: a 320x200 first-person PiP from his eyes (the same DrawWorld
  pass with a perspective camera); on his death it takes the screen,
  drops to the floor, and fades red before the intermission. Match end shows the
  intermission report card (vanilla E1 PAR times in `ParSeconds`).
  `--selftest` screenshots at frame 60, or at `--exit-after N` when given.
  Camera follows him by default (SPACE re-follows after any pan; R = whole
  map). F1 = developer overlay (the old always-on debug text). M toggles the
  3D models <-> authentic WAD sprites. A how-to card opens each match. **Hell's Hand** (each on a 12s
  cooldown): F slams/holds the door — or recalls the lift — nearest the
  cursor (15/20 souls); V detonates the nearest barrel (30 souls; barrels
  are live entities, shootable, chain-reacting). Drops cost souls (the Pain Tithe:
  base drip ramps 4→10/s with his progress toward the exit, +1 soul per
  point of damage dealt to him, no refund for healing he picks up). Drops
  inside his line of sight cost ×1.5 and telegraph for 1.5s before
  spawning; hidden drops are instant. `SimDrop` is the free dev spawn,
  `SimTryDrop` the player path.

## Play stats

`src/playstats.c` (shared with the other ximg.app games) sends anonymous play
sessions to `https://ximg.app/api/play/`: game, version, platform, a random
install id, and play time (a beat every 60 s and at exit). The server adds a
country from the IP and does not keep the IP. Dashboard (owner only):
https://stats.ximg.app/games/. Hidden runs (`--selftest`, `--simonly`,
`--exit-after`, `--dump-*`) and `--viewer` never send; `PLAYSTATS_OFF=1`
turns it off anywhere. The App Store privacy label and `ios/PrivacyInfo.xcprivacy`
declare Product Interaction + Coarse Location, not linked, not tracking — keep
them in step if what is sent ever changes. Keep `HELLDROP_VERSION` equal to
`MARKETING_VERSION` in `ios/project.yml`.

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
  shots like real walls. Map things spawn at the EASY skill filter
  (0x01) and the native garrison deals 45% damage (`SimTuning`, tuned with
  `--simonly`); player-dropped demons always hit at full strength. At the old
  HMP/100% baseline he died unaided on 7/8 maps — the player won by watching. Doom Guy unaided should
  usually clear the map population and exit; player drops are what kill
  him. His targeting is the **threat ledger** (`DgThreatScore`): class
  weights (arch-vile 4x), time-to-melee for melee-only kinds, charging
  lost souls spike, a live projectile bumps its owner, infighting pairs
  are deprioritized ("let them work"), hysteresis prevents flicker, and
  an engage threshold lets him walk past far stragglers. Balance knobs
  live in `MOB_INFO`, `CLASS_W`, the monster whiff curve in `MobAttack`,
  and his cadence in `DgFire`.
- **Nav robustness** (all in sim.c, all logged): radius-aware shortcut
  pruning; `OBJ_SWITCH` switch hunting when the exit is unreachable (one
  Dijkstra field picks the nearest reachable unused switch/walkover);
  stair builders (7/8/100/127) as chains of riser lifts; a progress
  watchdog (`nav_skip`) that blinks him past a dead-end after 12 s with no
  new closest approach; a mid-match rescue that relocates him off
  graph islands. `sim_init` logs `unhandled_specials`; `switch_hunt_none`
  logs the `fence` lines around his reachable region — read those first
  when a map's progression dead-ends. `TriAtPoint` uses a uniform grid
  (build with `-DTRI_GRID_CHECK` to verify it against the linear scan).
- `src/wadtex.c` — wall textures (PNAMES + TEXTURE1/2 composites) and
  flats, decoded via PLAYPAL. levelmesh.c groups render geometry per
  texture (Doom pegging/offsets, sector-light vertex tint, wall-base
  contact shadow, animated flats); the untextured `floors` mesh remains
  for the sim and mouse picking.
- `src/models.c` — GPU-skinned GLB renderer for the Blender-built models
  in `assets/models/` (built by `tools/models/`): clips idle/walk/attack/
  pain/death chosen from sim state, key + rim light, HUD portraits. Every
  model is optional — missing ones fall back to the WAD sprite.
  `HELLDROP_MODEL_DIR` overrides the directory. Rebuild the roster with
  `tools/models/build_all.sh [names...]` (Blender on the mini host by default,
  `BLENDER_HOST=local` to run here); it validates every GLB in raylib and
  writes `assets/models/manifest.json` + contact sheets in
  `build/model_previews/`. All designs are original (role-equivalents, not
  id's monsters). `--viewer` (+ `--selftest`) shows every model cycling its
  clips on a depth-tested floor — check new models there first.
- Dynamic point lights (main.c `UpdateDynamicLights`): blasts, missiles,
  arch-vile fire and muzzle flashes light the level and the models.
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
