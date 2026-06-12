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
    DG_FIGHT,       // engaging a visible monster
    DG_RETREAT,     // low health: routing to a known health pickup
    DG_EXITED,      // flipped the exit switch: the player has lost
    DG_DEAD,        // the player has won
    DG_NO_GOAL,     // no start, no exit, or no route
} DGState;

typedef enum DGObjective { OBJ_EXIT, OBJ_HEAL } DGObjective;
typedef enum DGWeapon { WPN_PISTOL, WPN_SHOTGUN } DGWeapon;

typedef enum DoorState { DOOR_CLOSED, DOOR_OPENING, DOOR_OPEN, DOOR_CLOSING } DoorState;

typedef struct Door {
    int sector;
    float ceil, prevCeil;   // moving ceiling = bottom of the door panel
    float openCeil;         // open target: lowest neighbor ceiling - 4
    float speed;            // units per tic: 2 vanilla, 8 blaze
    DoorState state;
    int timer;              // tics left while OPEN before auto-close
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
    MK_CACO,        // 3005: flyer, ball + bite
    MK_ARCHVILE,    // 64:   flame channel + raises corpses (the Corpse Bank)
    MK_COUNT
} MobKind;

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
    SND_POPAIN, SND_DMPAIN, SND_PLPAIN, SND_VIPAIN,
    SND_PODTH, SND_BGDTH, SND_SGTDTH, SND_CACDTH, SND_VILDTH, SND_PLDETH,
    SND_DOROPN, SND_DORCLS, SND_ITEMUP, SND_WPNUP, SND_TELEPT,
    SND_VILATK, SND_FLAMST, SND_SLOP,
    SND_COUNT
} SimSnd;

#define SND_EVENTS_MAX 64

typedef struct SndEvent {
    int id;
    Vector2 pos;
} SndEvent;

typedef struct Proj {
    bool alive;
    Vector2 pos, prevPos;
    Vector2 vel;            // per tic
    float z, prevZ, zVel;
    int damageMult;         // damage = (1d8) * damageMult, vanilla missiles
    int ownerMob;           // -1 = Doom Guy (unused for now: he is hitscan)
    MobKind ownerKind;
} Proj;

typedef enum PickupType {
    PK_STIM, PK_MEDIKIT, PK_ARMOR, PK_SHOTGUN, PK_CLIP, PK_CLIPBOX,
    PK_SHELLS, PK_SHELLBOX,
} PickupType;

typedef struct Pickup {
    bool alive;
    bool dropped;           // monster drops give half amounts, vanilla rule
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
    int bullets, shells;
    bool hasShotgun;
    DGWeapon weapon;
    int fireCooldown;
    long deathTic;          // when he died; drives the death animation
    int targetMob;          // current fight target, -1 none
    float strafeDir;        // +1/-1 orbit direction in FIGHT
    int noTargetTics;
    int threatScanTics;
    int healPickup;         // pickup index RETREAT is heading for, -1
    Vector2 *wp;            // route waypoints (crossing midpoints + goal)
    float *wpFloor;         // floor height of the sector each wp enters
    int *wpDoor;            // door index the waypoint passes through, -1
    int wpCount, wpCursor;
    int waitDoor;           // door index while DG_WAIT_DOOR, else -1
    int stuckTics;
    int replans;
} DoomGuy;

typedef struct NavEdge {
    int to;        // destination node (triangle or sector)
    int line;      // portal linedef, -1 intra-sector
    int door;      // door that must be open to pass, -1
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
    DoomGuy dg;
    Mob mobs[MOBS_MAX];
    int mobCount;           // compact prefix; corpses stay in the array
    Proj projs[PROJS_MAX];
    Pickup pickups[PICKUPS_MAX];
    int pickupCount;
    Tracer tracers[TRACERS_MAX];
    int tracerHead;
    SndEvent sndEvents[SND_EVENTS_MAX];
    long sndHead;
    PendingDrop pending[PENDING_MAX];
    // The Pain Tithe: souls drip slowly and pour from damage dealt to him.
    float souls;
    long soulsSpent;
    int dropsMade;
    float startGoalDist;    // spawn->exit distance, normalizes his progress
    long kills;             // monsters Doom Guy has killed
    int exitLine;           // -1 if the map has no exit special
    Vector2 goal;
    int goalSector;
    unsigned char rnd[256];
    int rndIndex;
} Sim;

bool SimInit(Sim *sim, const DoomMap *map, const LevelMesh *lm);
void SimFree(Sim *sim);
void SimTick(Sim *sim);
int  SimRandom(Sim *sim);                                  // 0..255
int  SimSectorAtPoint(const Sim *sim, float x, float y);   // -1 outside
float SimFloorAtPoint(const Sim *sim, float x, float y, bool *ok);
// Dev/free spawn (used by --autodrop); ignores the economy entirely.
bool SimDrop(Sim *sim, MobKind kind, float x, float y);
// Player verb: pay souls and drop a monster. Out of his sight: instant.
// In his sight: HOT_DROP_MULT price and a telegraphed delayed spawn.
DropResult SimTryDrop(Sim *sim, MobKind kind, float x, float y);
// Price of dropping `kind` at a point right now; *hot reports the
// line-of-sight premium. (Non-const: the sight check uses scratch stamps.)
int SimDropCost(Sim *sim, MobKind kind, float x, float y, bool *hot);
int SimDropBaseCost(MobKind kind);
float SimSoulRate(const Sim *sim);   // current drip per second
const char *DGStateName(DGState s);
const char *MobKindName(MobKind k);
const char *MobStateName(MobState s);
float MobRadius(MobKind k);
bool MobIsFlyer(MobKind k);
int MobMaxHealth(MobKind k);

#endif
