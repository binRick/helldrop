# HELLDROP — Design Document

> **Pitch:** Doom, played by an AI marine who knows E1M1 like a speedrunner — while you, the demonic
> invasion, drop monsters from an isometric god-view to kill him before he reaches the exit switch.
> His instincts are published and exploitable (he *always* breaks for health at 30%, *always* grabs
> the soulsphere), every drop is a bet paid off in his blood, and his cheapest counters — infighting
> your horde, looting your dead zombies' ammo — are the classic Doom systems turned against you.
> Thirty years of Doom folk knowledge is the skill tree; you finally get to be the monster closet.

This document synthesizes a multi-perspective ideation pass (core loop/economy, Doom Guy AI,
monster roster, presentation/tech, modes/progression), prior-art research, and an adversarial
design critique. Decisions marked **[DECIDED]** were adjudicated by the critique pass.

---

## 1. The Five Load-Bearing Systems

Cut any of these and the game collapses into a tech demo.

### 1.1 The Pain Tithe (economy)
Your resource is **Souls**. Small base drip (~4/sec, ramping with his map progress), but the real
income is a bounty on damage: **1 soul per point of damage dealt to Doom Guy**, ×3 on armor break,
plus a "Terror Bonus" when a monster survives an engagement 5+ seconds. Healing he picks up does
NOT refund you — a soulsphere is a pure loss. Every drop becomes an ROI question; the loop is
self-reinforcing but skill-gated. This kills the genre's documented degenerate strategy
(hoard passive income → dump one cyberdemon — Dungeon Keeper's known sore).

- Income ramp is keyed to **which checkpoint sector he's reached**, not wall-clock time, so the
  economy is fair whether his AI plays fast or cautious.
- **Death-spiral guard:** if income gates on damage and a high-skill Doom Guy takes none, the
  losing player gets poorer while he gets stronger. The progress-keyed drip ramp is the explicit
  comeback mechanism — tune it as such.

### 1.2 The Legibility Contract (Doom Guy AI)
The player has no aim and no direct units — their only skill is *predicting Doom Guy*. Loop Hero's
single biggest player complaint was opaque hero targeting; in Helldrop, **predictable AI is the
puzzle**. The architecture matters less than the contract: a small set of hard, published tells.

**Rule-of-30 (published in a "Know Your Enemy" codex, 3–5 rules max):**
- Below 30% health he ALWAYS disengages toward the nearest known health pickup.
- Below 8 shells he switches down and reroutes for ammo.
- He always commits through a door he has opened.
- He always grabs a berserk pack on sight.

These are intentional exploits — ambush the medkit route, mine the door, bait the berserk room.
Predictability in load-bearing places makes the unpredictable parts feel fair.

**Moment-to-moment intent telegraphy** (critique: a codex alone is not enough): a thought-bubble
icon or route-line flash when his drive switches ("→ MEDKIT", "→ SHOTGUN"). Players will not infer
internal state from movement alone.

### 1.3 Feeding the Slayer (anti-spam, two lenses invented it independently)
Faithful to Doom: zombiemen drop clips, shotgun guys drop shotguns/shells; demons drop nothing.
Spamming the cheapest unit **literally resupplies him**. Kills "spam zombiemen" without an
arbitrary cooldown, and creates the ammo-starvation metagame: starve his shells with pure-demon
waves to force him onto the pistol, THEN send the baron.

### 1.4 Hell's Hand (real-time verbs between drops)
The Anomaly: Warzone Earth lesson is unambiguous: inversion + placement + passive watching = boring;
the active-ability layer is what makes the genre work. Cheap environmental micro-spends on
~12-second cooldowns:
- **Hold a door shut** as he approaches (15 souls, 3s)
- **Recall a lift** mid-ride to strand him (20)
- **Lights out** in a sector for 8s (25 — spectres harder to hit, his aim cone widens)
- **Detonate a barrel** the map already has (30)

Combos write themselves: lights out + spectre pack + door slam behind him.
Consider a Dungeon Keeper-style release valve later: brief **possession** (steer one monster for
5 seconds) or a "focus here" pheromone ping.

### 1.5 The 2.5D Cheat (rendering architecture)
Don't build a 2D isometric engine. Render real 3D geometry — **extruded Doom sectors** — through a
raylib `Camera3D` with `CAMERA_ORTHOGRAPHIC` at the classic dimetric angle. The depth buffer solves
all occlusion; sector floor/ceiling heights become Z displacement for free; 90° camera rotation,
zoom (`fovy` = view height in ortho), and cutaway ceilings come almost free. This converts the
project's scariest problems into things the GPU or the WAD data solve.

> **Verified:** raylib **6.0 released 2026-04-23**; Camera3D/ortho/DrawBillboard/RenderTexture/shader
> APIs unchanged from 5.x. raylib 6's redesigned HiDPI/fullscreen handling makes low-res
> render-target blits more predictable on macOS retina.
> https://github.com/raysan5/raylib/releases/tag/6.0

---

## 2. Core Loop & Economy

- **Two-tier placement:** default drops restricted to sectors **outside his line of sight** (cheap,
  silent — monster-closet energy). In-LOS "hot drops" allowed at **+50% cost** with a telegraphed
  1.5s green teleport flash he will pre-aim at. Map-tagged closet sectors (real E1M1 closets/secret
  walls) give a 20% discount with an on-demand wall-lower reveal.
- **Bestiary cost curve priced against his arsenal, not monster HP:** zombieman 8, shotgun guy 14,
  imp 22, lost soul 28, pinky 38, spectre 50, cacodemon 65, hell knight 105, revenant 110,
  mancubus 130, arachnotron 140, baron 170, arch-vile 220, cyberdemon 600, mastermind 650.
  Effective value **decays as he upgrades** — a pinky is terrifying vs. pistol, free meat vs.
  shotgun, a joke vs. rockets. UI shows live "threat vs. current loadout" pips per monster.
  Monsters are melting assets on his upgrade clock — this punishes hoarding.
- **His progression is your timer.** No clock; the pressure is the arsenal ladder (pistol start,
  shotgun ~90s in, chaingun in the secret, rocket launcher before the exit). The macro decision is
  tempo: blitz the pistol stage or bank toward an arch-vile knowing he'll have rockets.
- **Win/lose:** he dies (you win) or flips the exit switch (you lose). One life, pistol-start
  stakes. Post-match intermission screen grades efficiency (souls per HP), attrition (time past
  PAR), and "Kills: 0%" as his joke stat. Secrets matter: the soulsphere is a 100+ HP swing, so
  garrisoning secret areas is a real spend.
- **[DECIDED] Full information.** Show his complete status bar — health, armor, weapon, ammo —
  at all times. The hidden-ammo/scout idea was cut: this is a single-player legibility game, and
  the counter-pick layer and ammo-starvation strategies are unplayable blind.
- **Director scoring (from L4D research):** reward sustained pressure, near-misses, and style
  (infight kills, crusher kills), not just the kill — otherwise optimal play is one boring
  alpha-strike after silent banking.

---

## 3. Doom Guy AI

**Architecture: two-layer Utility AI ("Drives + Reflexes"), ~600 lines of plain C.**
- **Layer 1 — Drive Selector (~10Hz):** scores 6 goals — AdvanceObjective, EngageThreat,
  GrabPickup, RetreatToHeal, Rearm, HuntSecret — each scorer a pure function of a WorldState struct
  (health %, ammo, threat map, distance-to-objective), with hysteresis + 1–2s commitment timers so
  he never dithers. Winner emits a target position + stance enum (PUSH / HOLD / KITE / FLEE).
- **Layer 2 — Reflexes (every frame):** target selection, weapon choice, strafe/dodge steering,
  trigger discipline — he shoots while looting and dodges while fleeing, like a human player.
- Rule-of-30 tells are priority overrides checked *before* the drive selector.
- Pathing: A* over the sector/portal graph (needed for doors/lifts anyway) + local steering
  (weighted sum of orbit-tangent, dodge-perpendicular, path-follow, threat-repulse vectors).

**Key behaviors:**
- **Arsenal ladder:** pickup desirability = strategic value × (1 − coverage) / path cost. Every
  acquisition fires a loud sting ("He took the chaingun!") and raises the threat tier. Pickup
  denial — a pinky wall on the shotgun closet — is a first-class player tactic.
- **Threat-ledger targeting:** scores visible monsters by projectile urgency, DPS-at-range,
  time-to-melee, and a class bonus (arch-viles/pain elementals top it; he stunlocks cacos via
  pain chance rather than killing nearer chaff). Composition becomes a language: pinkies are
  taunts, revenants are artillery, an arch-vile is a magnet.
- **Infight baiting — his signature outplay:** when outnumbered, he computes positions collinear
  with two different-species monsters, sidesteps late so the fireball connects, then kites and
  lets them grind. **[DECIDED]** the purchasable infight-immunity "Hellish Pact" is CUT (it deletes
  his best scene); at most a rare drafted card.
- **Tyson Protocol (desperation):** below 15% health with no reachable health, he flips to berserk
  aggression — punches out zombiemen for their drops, max-variance strafing, no looting detours.
  Distinct audio/visual state. Runs end in a frantic 10-second chase players retell.
- **Competence dials, one brain:** `struct DoomGuySkill { reaction_delay_ms, dodge_skill,
  aim_cone_degrees, infight_iq, secret_sense, ammo_discipline }`. Difficulty is behavioral, never
  stat-inflated. Ship a debug overlay with live sliders — this struct is where 80% of playtest
  tuning happens.
- **Keycard progression graph:** rooms as nodes, doors/lifts/switches as edges, key-locked edges
  tagged; critical path = one BFS at load. Player sees a faint route overlay — chokepoint warfare
  ("he MUST cross the nukage bridge") is the core strategic layer.
- **Haunted map (sandbox/campaign only):** per-sector decaying "fear" value where ambushes
  happened; he pre-fires known closets, advances slower in marked rooms. His caution is itself
  exploitable — paranoia in room A means no contest in room B. **[DECIDED]** disabled in seeded
  modes (puzzle/daily) to keep solutions shareable.

---

## 4. Monster Roster & Combos

Counter-matrix is inherited Doom folk knowledge, and **one C table is the single source of truth**
read by both his weapon-select AI and the player-facing tooltips — AI behavior and player
expectations stay perfectly aligned.

| Monster | Role | Hard counter |
|---|---|---|
| Zombieman / shotgun guy / chaingunner | Chip + area denial — but corpses feed him ammo | Any; the cost is yours |
| Imp | Cheap projectile pressure, corpse fodder | Shotgun |
| Pinky / spectre | **The Cork** — body-block corridors, edit level connectivity | Chainsaw/berserk (stunlock shred) |
| Lost soul | Budget flyer, dive-bomb displacement; bait rockets at close range | Splash discipline |
| Cacodemon | **Airspace herding** — cut routes over pits/courtyards, bend his path | Plasma |
| Pain elemental | **Soul Foundry** — unit generator (21-soul vanilla cap, self-jam quirk) | Kill-it-first priority |
| Revenant | Cover denial — homing rockets evict him from hold positions | Open ground + dodge skill |
| Mancubus | Angular flame lattice, zone denial in arenas | Strafe gaps |
| Arachnotron | Plasma suppression, pins lateral movement | Cover |
| Hell knight / baron | **Baron Tax** — no drops, soak enormous ammo before the fight you care about | He routes around unless choked |
| Arch-vile | **The Corpse Bank** (below) | Rockets gib your savings |
| Cyberdemon / mastermind | **Exit Toll** — once-per-match static anchors that rezone the map's routing | BFG + cells race |

**Named combo plays:**
- **Flush and Cut:** revenant pair evicts him from cover → pre-placed chaingunner picket taxes
  every meter of open ground he's flushed into.
- **The Corpse Bank:** corpses persist (cap ~40–64, gibbed corpses unraisable — real Doom rule).
  Seed a chokepoint with cheap waves, then drop a 220-soul arch-vile behind the pile and re-buy
  the whole wave free. His counterplay: rocketing corpse piles to gib them — make that a published,
  visible tell, or vile-banking becomes the dominant build.
- **Soul Foundry:** pain elemental corked behind pinkies — souls fly over the meat wall; over-cork
  a small room and his own souls jam his spit (authentic vanilla failure mode — greed punished by
  physics).
- **Iron Lattice:** two mancubi at offset angles weave crossing flame cones; an arachnotron pins
  his strafe inside the lattice. Hell's answer to big open rooms.
- **Gotcha!:** anchor cyberdemon AND mastermind with overlapping zones and trigger the most famous
  infight in Doom history — or waste both by overlapping carelessly.

**Infighting (full vanilla rules):** hit by another species → retarget attacker; same-species
projectiles exempt; baron/knight mutually immune. ~4 lines of C in the damage function. Mixed
comps have the best coverage but can dissolve into civil war when he kites fireballs through your
front line — spacing and same-species lanes are a core skill.

**Sector light as terrain:** dark sectors degrade his target acquisition; spectres in sub-128
light are nearly invisible to him; arch-vile flame and explosions illuminate momentarily.
You hunt the map's darkness topology like a stealth game in reverse.

---

## 5. Presentation & raylib Tech

- **Level data: WAD-native, Freedoom-first.** Parse VERTEXES/LINEDEFS/SIDEDEFS/SECTORS, ear-clip
  triangulate (sectors can be concave with holes), extrude wall quads between neighbor heights,
  `UploadMesh()`. Bundle **freedoom1.wad (BSD)** as default + "point me at your DOOM.WAD" for
  owners. **[DECIDED]** over hand-authored maps — legally clean and every PWAD ever made becomes
  a Helldrop arena. (id/Microsoft IP: original assets are not licensed for reuse; shareware WAD
  redistributes only whole and unmodified.)
- **v1 art direction: flat-shaded vertex colors** (skip wall textures entirely) + chunky sprites =
  deliberate diorama look that sidesteps texture-extraction complexity.
- **Doom's 8-rotation sprite system, billboarded:** quantize (monster facing − camera yaw) to 8
  rotations, parse frame names (POSSA1, POSSA2A8 mirrored) straight from sprite lumps;
  `DrawBillboardPro` locked to Y-up cylindrical so sprites stand upright. Camera rotation snaps
  every sprite to its new rotation frame — delightfully mechanical.
- **Depth sorting story:** Doom sprites are binary-alpha, so an alpha-test shader
  (`if (a < 0.5) discard`) lets billboards write depth like opaque geometry — zero sorting.
  One coarse back-to-front pass only for true translucents (spectre fuzz, teleport fog).
- **Sector light:** vertex-color tint (free with raylib's default shader); authentic option later:
  `floor(light*32)/32` COLORMAP-style banding + distance darkening in a small shader.
- **The mugshot IS the progress bar:** classic STF face (5 damage tiers × look directions, ouch,
  evil grin, dead) — he looks toward incoming damage, grins when he grabs a weapon (deflating!),
  and bloodies as you win. No abstract HP bar. The most iconic UI in games, inverted into the
  villain's scrying mirror. Logic documented in open-source `st_stuff.c`; Freedoom ships STF.
- **Doomcam:** 320×200 RenderTexture PiP of his first-person view — diegetic answer to "can he
  see my spectres?". On death it takes over fullscreen: camera drops to floor, red fade, dead face.
  Throttle to 30Hz.
- **Living geometry:** doors/lifts/crushers as small dynamic meshes (`UpdateMeshBuffer` per tic),
  vanilla movement constants (doors 2 map-units/tic). A rising door reads as a panel sliding out
  of the floor. Crusher kills via the player's door-possession verb = the funniest emergent stories.
- **Juice pack:** trauma-based screenshake (`noise(t)*trauma²` offsetting position+target together;
  cyberdemon footsteps add persistent rumble), XDeath gib animations + persistent floor decals,
  Doom palette flashes, DMX sound lumps (8-bit PCM ~11kHz) → `LoadSoundFromWave` trivially.
- **Low-res soul:** render to fixed 640×400 RenderTexture (2× Doom's 320×200), point-filter
  upscale, optional scanlines, **screen-melt wipe** on his death. Locks sprite pixel density to
  world scale — kills the mixed-pixel-size tell that ruins retro-3D mashups.
- **Determinism [DECIDED]:** fixed 35Hz tic, one `P_Random()`-style 256-entry table from day one
  (puzzle mode, dailies, and LMP-style input-timeline replays all depend on it; retrofitting onto
  float spaghetti is painful). Camera/rendering interpolates at 60fps on top.

---

## 6. Modes & Progression

- **Campaign — "Knee-Deep in His Blood":** E1M1→E1M9 from Hell's side. He pistol-starts each map;
  your bestiary unlocks in Doom's own monster-introduction order. Kill him inside a secret sector
  on M3 → unlock M9 (secret-exit tradition). 5–8 min/level.
- **Inverted skill scale:** "He's Too Young to Die" → "NIGHTMARE!" where HE is the nightmare —
  near-instant acquisition, speedrun routing, vile-sniping, and Nightmare's actual respawn rule
  flipped: health/ammo pickups respawn *for him*. Difficulty = behavior presets over the
  DoomGuySkill struct, never stat inflation.
- **Roguelike — "Icon of Sin Run":** 6–8 randomized levels, draft 1-of-3 cards between levels
  (monsters, quantity, Hell modifiers), banked souls carry forward, 3 exits = run over. The Icon
  of Sin literally spawns random monsters in Doom II — perfect run patron.
- **Puzzle mode:** "kill him with exactly these monsters", fixed seed, 3-star objectives
  (infighting-only, before the blue key, without him firing). Onboarding disguised as content.
- **Daily Nightmare:** global seed, one scored attempt, style multipliers, replays as tiny
  input timelines (LMP demo philosophy) — watch #1's drop sequence as a ghost.
- **Endless "Slaughterhouse"** (he respawns with escalating gear: pistol start → BFG start) +
  **sandbox** with AI sliders and one-button GIF export (`msf_gif.c`) — free marketing.
- **Meta-progression [DECIDED — horizontal only]:** unlock monsters via themed feats (arch-vile
  after 100 corpses accumulated; pain elemental after 50 lost-soul kills). NO stat-mutating
  upgrades and NO spawn-flash reduction — they break the legibility contract and the one-table
  source of truth. Veterancy/extraction is a sandbox-mode experiment at most.
- **Tone:** **Hell's Department of Mortal Termination** — between-level memos from a baron with
  an MBA, Performance Improvement Plan on retry, corner office over the blood falls. Dungeon
  Keeper's mentor-voice lesson: tone does enormous retention work, and it's skippable text —
  huge payoff per engineering hour. (Alternate skin: the corrupted gate AI, clinical VEGA-style
  announcer — a string-table swap. Pick ONE default early.)

---

## 7. Top Design Risks (from prior art + critique)

1. **The passive watch phase** (killed Despot's Game's engagement; dogged Loop Hero — "most of
   the gameplay is not playing it"). Mitigations: Hell's Hand verbs, hot-drop timing premium,
   director-style scoring, possession release valve.
2. **A dominant strategy collapses the bestiary** (cheap-spam, or bank-and-bomb). The lattice that
   holds: Feeding the Slayer (spam feeds him), depreciating cost curve (hoarding rots), infighting
   (lazy blobs eat themselves). Watch the Corpse Bank — his corpse-gibbing must fire visibly and
   often or vile-banking becomes the only build.
3. **Doom Guy feels cheaty or trivial — and unreadable either way** (Loop Hero's cardinal sin;
   also the schedule risk — the AI is 60% of the project). Mitigations: Rule-of-30 tells, route
   overlay, in-the-moment intent telegraphy, behavioral difficulty dials, debug-slider playtesting.

**Prior-art cheat sheet:** Loop Hero (closest analog — placement combinatorics good, opaque AI
bad), Anomaly: Warzone Earth (the genre needs active verbs), Dungeon Keeper (tone + drop verb +
possession; stockpiling economies break), Despot's Game (the watch-phase failure mode),
Carrion (never force the player to help the human), Doom Eternal Battlemode (one demon is never a
threat — orchestration is; single-player-vs-AI dodges its fatal PvP balance trap), Zombie Master /
L4D Director (score pacing and pressure, not just kills), Legion TD 2 (sends as economy decisions).

---

## 8. MVP (1–2 weeks of evenings)

**Milestone sentence:** one playtester loses, says "one more try," and articulates a map-specific
plan. That's the whole goal — it answers the only question that matters: *is predicting and
ambushing a readable AI marine fun for 5 minutes with four monsters?*

- **Level:** E1M1 from bundled freedoom1.wad. Flat-shaded extruded sectors, fixed ortho angle
  (no rotation), light as vertex tint only. Doors animate; skip lifts/crushers.
- **Monsters (4):** zombieman 8 (proves the ammo dilemma), imp 22 (projectile + corpse fodder),
  pinky 38 (corridor cork via nav-blocking), cacodemon 65 (flyer, proves routing pressure).
  No arch-vile, no spectres, no bosses — corpses persist visually only.
- **Doom Guy:** 3-state FSM, not the utility brain — ADVANCE (A* to exit, opens doors),
  FIGHT (nearest visible, circle-strafe at fixed radius, never stationary),
  RETREAT (hard rule: <30% HP → beeline to nearest known health). Pistol start; picks up the
  E1M1 shotgun + green armor — exactly one visible power-up beat. **Infighting retarget rule in**
  (4 lines; the demo's best moment). No dodge skill, no baiting, no secrets, no memory.
- **Economy/UI:** Pain Tithe (drip + 1 soul/damage), static cost table, drop anywhere out of LOS,
  hot drop +50% with green flash. One verb: **hold door shut**. HUD: souls, monster bar,
  **mugshot with health/armor** (the emotional core — keep it). Win/lose: death or exit switch;
  plain-text end stats.
- **Cut/fake:** modes, meta, narrative, doomcam, 8-rotation sprites (front-facing billboards),
  screenshake/gibs, skill levels, full determinism — but route ALL randomness through one
  `P_Random()`-shaped function from day one.
- **Schedule:** evenings 1–3 WAD parse + ortho render · 4–6 marine FSM + A* + weapons ·
  7–9 four monsters + damage + infighting · 10–12 economy + drop UI + mugshot + door verb ·
  13–14 tune until the playtest sentence happens.

---

## Appendix: build-order spine

1. WAD loader → extruded sectors → ortho Camera3D (the 2.5D Cheat)
2. Marine FSM with the <30% health tell → A* → pistol/shotgun
3. Four monsters + damage function (with infighting + soul credit hooks)
4. Pain Tithe + drop placement + mugshot HUD + one door verb
5. *(v2)* Utility-AI brain, arch-vile/corpse bank, Hell's Hand suite, skill dials, determinism
   hardening, modes
