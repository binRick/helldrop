#ifndef HELLDROP_SIM_H
#define HELLDROP_SIM_H

#include <stdbool.h>
#include "raylib.h"
#include "map.h"
#include "levelmesh.h"

// Fixed-rate deterministic simulation core, Doom-style: the render loop owns
// wall time and feeds whole 35 Hz tics into SimTick; all randomness flows
// through SimRandom (a fixed 256-entry table, Doom's P_Random shape) so a
// given map + drop sequence plays out identically. Positions stay in Doom
// map coordinates (x east, y north); the renderer maps to world (x, h, -y).
#define SIM_TICRATE 35
#define SIM_TICTIME (1.0/35.0)

// Doom movement constants, map units
#define DG_RADIUS    16.0f
#define DG_HEIGHT    56.0f
#define DG_STEP      24.0f   // max climbable floor step
#define DG_RUNSPEED   8.0f   // per tic, ~Doom forward run
#define DG_USERANGE  64.0f   // switch/door reach

#define MOBS_MAX    256
#define PROJS_MAX   128
#define PICKUPS_MAX 192
#define TRACERS_MAX  64
#define PENDING_MAX  16

#define SOULS_START 30.0f
#define SOULS_CAP  999.0f
#define HOT_DROP_MULT 1.5f     // in-his-sight premium
#define HOT_DROP_TELEGRAPH 52  // tics of teleport flash before a hot spawn

typedef enum DGState {
    DG_ADVANCE,     // following the route toward the current objective
    DG_WAIT_DOOR,   // used a door, standing until it opens enough
    DG_WAIT_LIFT,   // called a lift, standing until it reaches him
    DG_FIGHT,       // engaging a visible monster
    DG_RETREAT,     // low health: routing to a known health pickup
    DG_EXITED,      // flipped the exit switch: the player has lost
    DG_DEAD,        // the player has won
    DG_NO_GOAL,     // no start, no exit, or no route
} DGState;

// OBJ_SWITCH: no route to the exit yet — go fire the nearest reachable
// unused switch/walkover that might open one (progression hunting).
typedef enum DGObjective { OBJ_EXIT, OBJ_HEAL, OBJ_WEAPON, OBJ_SWITCH } DGObjective;
typedef enum DGWeapon { WPN_PISTOL, WPN_SHOTGUN, WPN_CHAINGUN, WPN_ROCKET } DGWeapon;

typedef enum DoorState { DOOR_CLOSED, DOOR_OPENING, DOOR_OPEN, DOOR_CLOSING } DoorState;

// Vanilla plats: the floor lowers to the lowest neighbor, waits, raises.
typedef enum LiftState { LIFT_UP, LIFT_DOWN, LIFT_LOW, LIFT_RISING } LiftState;

typedef struct Lift {
    int sector;
    float floor, prevFloor;   // live floor height (prev for render lerp)
    float lowFloor;           // lowest neighbor floor
    float highFloor;          // the sector's original floor
    float speed;              // 4/tic vanilla, 8 blaze
    LiftState state;
    int timer;                // tics left waiting at the bottom
    bool stayLow;             // one-shot floor-lower barriers never rise
    bool riser;               // floor-raise targets: start low, rise once
    bool recalled;            // Hell's Hand: extra hold at the bottom
    int stairTag;             // stair-builder chain (0 = not a stair step)
} Lift;

// Exploding barrels (thing 2035): shootable, chain-reacting, and remotely
// detonable by the player.
typedef struct Barrel {
    bool alive;
    Vector2 pos;
    float z;
    int health;
} Barrel;

#define BARRELS_MAX 64

// Hell's Hand: environmental micro-spends. Costs in souls; each verb has
// its own cooldown so it's seasoning, not a stunlock engine (DESIGN.md).
#define VERB_DOOR_COST 15
#define VERB_LIFT_COST 20
#define VERB_BARREL_COST 30
#define VERB_RANGE 140.0f
#define VERB_COOLDOWN_TICS 420   // 12s
#define DOOR_HOLD_TICS 105       // 3s

typedef enum VerbResult {
    VERB_OK, VERB_NO_SOULS, VERB_NO_TARGET, VERB_COOLDOWN
} VerbResult;

typedef enum VerbKind { VERB_DOOR, VERB_LIFT, VERB_BARREL, VERB_KINDS } VerbKind;

typedef struct Door {
    int sector;
    float ceil, prevCeil;   // moving ceiling = bottom of the door panel
    float openCeil;         // open target: lowest neighbor ceiling - 4
    float speed;            // units per tic: 2 vanilla, 8 blaze
    DoorState state;
    int timer;              // tics left while OPEN before auto-close
    long holdUntil;         // Hell's Hand: his Use is denied until this tic
    bool stayOpen;          // D1-type doors never auto-close
    bool locked;            // key door; v1 sim opens it anyway, logged once
    bool lockLogged;
} Door;

// Monster kinds, vanilla-flavored stats live in sim.c's MOB_INFO table.
typedef enum MobKind {
    MK_ZOMBIE,      // 3004: hitscan chip, drops a clip
    MK_SHOTGUNGUY,  // 9:    3-pellet hitscan, drops a shotgun
    MK_IMP,         // 3001: fireball + claw
    MK_PINKY,       // 3002: fast melee wall
    MK_SPECTRE,     // 58:   pinky in the dark — he barely sees it
    MK_CACO,        // 3005: flyer, ball + bite
    MK_LOSTSOUL,    // 3006: flyer, dive-bomb charge
    MK_REVENANT,    // 66:   homing tracers — cover denial
    MK_BARON,       // 3003: the Baron Tax — 1000hp ammo sponge
    MK_ARCHVILE,    // 64:   flame channel + raises corpses (the Corpse Bank)
    // Doom 2 heavies (sprites/sounds need freedoom2.wad / DOOM2.WAD):
    MK_CHAINGUNNER, // 65: sustained hitscan bursts, drops his chaingun
    MK_HELLKNIGHT,  // 69: half a baron; nobles never infight each other
    MK_MANCUBUS,    // 67: three paired-fireball volleys — the flame lattice
    MK_PAINELEMENTAL, // 71: the Soul Foundry — spits lost souls, capped 21
    MK_COUNT
} MobKind;

#define MAXSKULLS 21    // vanilla live lost-soul cap (Soul Foundry governor)

typedef enum MobState {
    MOB_IDLE, MOB_CHASE, MOB_ATTACK, MOB_PAIN, MOB_RAISE, MOB_DEAD
} MobState;

// Raisable corpses are capped; beyond this the oldest decay (gib) and the
// arch-vile can no longer bank them.
#define CORPSES_MAX 64

typedef struct Mob {
    MobKind kind;
    Vector2 pos, prevPos;
    float z, prevZ;         // feet height (cacos float above the floor)
    float angleDeg;
    int health;
    MobState state;
    int stateTics;          // countdown inside ATTACK/PAIN
    int attackCooldown;
    int sector;             // cached containing sector, -1 unknown
    int targetMob;          // infight target, -1 = Doom Guy
    int dodgeTics;          // blocked-path wiggle, Doom A_Chase style
    float dodgeDir;
    long deathTic;          // when it died; drives the death animation
    int raiseTarget;        // arch-vile: corpse being resurrected
    int raiseCooldown;      // arch-vile: ~3s between resurrections
    bool gibbed;            // overkilled/decayed: beyond resurrection
    bool awake;
    bool playerDropped;
} Mob;

// Sound events the sim emits and the presentation layer drains; keeps the
// sim headless. `head` counts monotonically; readers keep their own cursor.
typedef enum SimSnd {
    SND_PISTOL, SND_SHOTGUN, SND_FIRSHT, SND_FIRXPL, SND_CLAW, SND_SGTATK,
    SND_POSIT, SND_BGSIT, SND_SGTSIT, SND_CACSIT, SND_VILSIT,
    SND_SKLATK, SND_SKESIT, SND_SKEATK, SND_SKEDTH,
    SND_BRSSIT, SND_BRSDTH,
    SND_MANSIT, SND_MNPAIN, SND_MANDTH,
    SND_PESIT, SND_PEPAIN, SND_PEDTH, SND_KNTSIT, SND_KNTDTH,
    SND_POPAIN, SND_DMPAIN, SND_PLPAIN, SND_VIPAIN,
    SND_PODTH, SND_BGDTH, SND_SGTDTH, SND_CACDTH, SND_VILDTH, SND_PLDETH,
    SND_DOROPN, SND_DORCLS, SND_ITEMUP, SND_WPNUP, SND_TELEPT,
    SND_VILATK, SND_FLAMST, SND_SLOP,
    SND_RLAUNC, SND_BAREXP,
    SND_PSTART, SND_PSTOP,
    SND_COUNT
} SimSnd;

#define SND_EVENTS_MAX 64

typedef struct SndEvent {
    int id;
    Vector2 pos;
} SndEvent;

typedef struct Proj {
    bool alive;
    bool isRocket;          // his rocket: direct (1d8)*20 + 128 radius blast
    bool homing;            // revenant tracer: curves toward Doom Guy
    Vector2 pos, prevPos;
    Vector2 vel;            // per tic
    float z, prevZ, zVel;
    int damageMult;         // damage = (1d8) * damageMult, vanilla missiles
    int ownerMob;           // -1 = Doom Guy
    MobKind ownerKind;
} Proj;

// Explosion flash for the renderer (deterministic sim data, cosmetic only).
typedef enum BoomKind {
    BOOM_ROCKET, BOOM_IMPBALL, BOOM_CACOBALL, BOOM_TRACER, BOOM_BARONBALL
} BoomKind;

typedef struct Boom {
    unsigned char kind;
    Vector2 pos;
    float z;
    long tic;
} Boom;

#define BOOMS_MAX 32

typedef enum PickupType {
    PK_STIM, PK_MEDIKIT, PK_ARMOR, PK_SHOTGUN, PK_CLIP, PK_CLIPBOX,
    PK_SHELLS, PK_SHELLBOX, PK_CHAINGUN, PK_RLAUNCH, PK_ROCKET, PK_ROCKETBOX,
    PK_COUNT
} PickupType;

typedef struct Pickup {
    bool alive;
    bool dropped;           // monster drops give half amounts, vanilla rule
    bool unreachable;       // routing to it failed once; stop trying
    PickupType type;
    Vector2 pos;
    float z;
} Pickup;

// Cosmetic hitscan tracer, render-only (ring buffer).
typedef struct Tracer {
    int ttl;
    Vector3 a, b;           // world coords
    bool fromDG;
} Tracer;

// A paid-for monster mid-teleport: hot drops (inside his line of sight)
// telegraph with a flash for HOT_DROP_TELEGRAPH tics before materializing.
typedef struct PendingDrop {
    bool active;
    long spawnTic;
    MobKind kind;
    Vector2 pos;
} PendingDrop;

typedef enum DropResult {
    DROP_OK,         // placed (or telegraphing)
    DROP_NO_SOULS,
    DROP_BAD_SPOT,   // off the walkable floors
    DROP_FULL,       // mob/pending capacity
} DropResult;

typedef struct DoomGuy {
    Vector2 pos, prevPos;   // prev values are last tic's, for render lerp
    float z, prevZ;         // ground height under feet
    float angleDeg;         // facing, CCW from east (Doom convention)
    DGState state;
    DGObjective objective;
    int sector;             // last known containing sector, -1 unknown
    int health, armor;
    int bullets, shells, rockets;
    bool hasShotgun, hasChaingun, hasRocketLauncher;
    DGWeapon weapon;
    int fireCooldown;
    long deathTic;          // when he died; drives the death animation
    int targetMob;          // current fight target, -1 none
    float strafeDir;        // +1/-1 orbit direction in FIGHT
    int noTargetTics;
    int threatScanTics;
    int healPickup;         // pickup index RETREAT is heading for, -1
    int weaponPickup;       // pickup index OBJ_WEAPON is heading for, -1
    int switchLine;         // linedef OBJ_SWITCH is heading for, -1
    int navSkips;           // watchdog skips past nav dead-ends (should stay 0)
    Vector2 progressGoal;   // objective point the watchdog measures against
    float progressBest;     // closest approach to it so far
    int noProgressTics;     // tics without a new closest approach
    long healRetryTic;      // low-health heal search backs off until this tic
    long nextPlanTic;       // failed-replan throttle
    Vector2 *wp;            // route waypoints (crossing midpoints + goal)
    float *wpFloor;         // floor height of the sector each wp enters
    int *wpDoor;            // door index the waypoint passes through, -1
    int *wpLift;            // lift index the waypoint rides, -1
    int *wpEdge;            // tri-nav edge per waypoint (-1 = none/goal)
    int wpCount, wpCursor;
    int stuckStrikes;       // consecutive stuck-replans; escalates recovery
    int waitDoor;           // door index while DG_WAIT_DOOR, else -1
    int waitLift;           // lift index while DG_WAIT_LIFT, else -1
    int stuckTics;
    int replans;
} DoomGuy;

typedef struct NavEdge {
    int to;        // destination node (triangle or sector)
    int line;      // portal linedef, -1 intra-sector
    int door;      // door that must be open to pass, -1
    int lift;      // lift that must be down/level to pass, -1
    Vector2 wp;    // crossing midpoint
} NavEdge;

typedef struct Sim {
    const DoomMap *map;
    const LevelMesh *lm;
    long tic;
    // Primary nav: directional adjacency over floor TRIANGLES (convex, so
    // straight lines between adjacent crossing points never hit walls —
    // sector-level routing fails inside concave sectors).
    NavEdge *tEdges;
    int *tEdgeStart;        // triCount+1 prefix offsets
    int tEdgeCount;
    int triCount;
    unsigned char *tEdgeBlocked;   // self-healing nav: edges he got stuck on
    // Fallback nav: directional sector adjacency via portal midpoints
    NavEdge *edges;
    int *edgeStart;         // sectorCount+1 prefix offsets
    int edgeCount;
    // uniform-grid line blockmap for movement clipping
    int *bmLines;
    int *bmStart;           // bmW*bmH+1 prefix offsets
    int bmW, bmH;
    float bmOrgX, bmOrgY;
    int *lineStamp;         // dedupe scratch, lineCount entries
    int stamp;
    Door *doors;
    int doorCount;
    int *sectorDoor;        // door index per sector, -1
    Lift *lifts;
    int liftCount;
    int *sectorLift;        // lift index per sector, -1
    unsigned char *lineUsed; // one-shot (W1/S1) trigger lines already fired
    unsigned char *lineTried; // switch hunt: lines he already went to fire
    DoomGuy dg;
    Mob mobs[MOBS_MAX];
    int mobCount;           // compact prefix; corpses stay in the array
    Barrel barrels[BARRELS_MAX];
    int barrelCount;
    long verbReadyTic[VERB_KINDS];   // Hell's Hand cooldowns
    Proj projs[PROJS_MAX];
    Pickup pickups[PICKUPS_MAX];
    int pickupCount;
    Tracer tracers[TRACERS_MAX];
    int tracerHead;
    Boom booms[BOOMS_MAX];
    int boomHead;
    SndEvent sndEvents[SND_EVENTS_MAX];
    long sndHead;
    PendingDrop pending[PENDING_MAX];
    // The Pain Tithe: souls drip slowly and pour from damage dealt to him.
    float souls;
    long soulsSpent;
    int dropsMade;
    float startGoalDist;    // spawn->exit distance, normalizes his progress
    long dgDamageTaken;     // total damage he has eaten (intermission stat)
    long kills;             // monsters Doom Guy has killed
    int exitLine;           // -1 if the map has no exit special
    Vector2 goal;
    int goalSector;
    unsigned char rnd[256];
    int rndIndex;
} Sim;

// Balance knobs for the map's native population (the "garrison").
typedef struct SimTuning {
    unsigned garrisonSkillBit;   // THINGS skill flag: 1 easy, 2 medium, 4 hard
    int garrisonDmgPct;          // native monsters' damage to him, percent
} SimTuning;
void SimSetTuning(SimTuning t);
SimTuning SimGetTuning(void);

bool SimInit(Sim *sim, const DoomMap *map, const LevelMesh *lm);
void SimFree(Sim *sim);
void SimTick(Sim *sim);
int  SimRandom(Sim *sim);                                  // 0..255
int  SimSectorAtPoint(const Sim *sim, float x, float y);   // -1 outside
float SimFloorAtPoint(const Sim *sim, float x, float y, bool *ok);
float SimSectorFloorNow(const Sim *sim, int sector);   // live (lift-aware)
// Dev/free spawn (used by --autodrop); ignores the economy entirely.
bool SimDrop(Sim *sim, MobKind kind, float x, float y);
// Player verb: pay souls and drop a monster. Out of his sight: instant.
// In his sight: HOT_DROP_MULT price and a telegraphed delayed spawn.
DropResult SimTryDrop(Sim *sim, MobKind kind, float x, float y);
// Hell's Hand: slam/hold the nearest door, or recall the nearest lift,
// within VERB_RANGE of the point. Picks whichever mover is closest.
VerbResult SimSlamMover(Sim *sim, float x, float y, int *costOut);
// Hell's Hand: detonate the nearest barrel within VERB_RANGE.
VerbResult SimDetonateBarrel(Sim *sim, float x, float y);
// Price of dropping `kind` at a point right now; *hot reports the
// line-of-sight premium. (Non-const: the sight check uses scratch stamps.)
int SimDropCost(Sim *sim, MobKind kind, float x, float y, bool *hot);
int SimDropBaseCost(MobKind kind);
float SimSoulRate(const Sim *sim);   // current drip per second
const char *DGStateName(DGState s);
const char *DGWeaponName(DGWeapon w);
const char *MobKindName(MobKind k);
const char *MobStateName(MobState s);
float MobRadius(MobKind k);
bool MobIsFlyer(MobKind k);
int MobMaxHealth(MobKind k);

#endif
