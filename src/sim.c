#include "sim.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "statelog.h"

#define BM_CELL 128.0f
#define DOOR_WAIT_TICS 150     // vanilla VDOORWAIT
#define DOOR_COST 300.0f       // A* distance-equivalent penalty per door
#define TURN_PER_TIC 20.0f     // degrees
#define WP_ARRIVE 24.0f
#define STUCK_TICS 70

#define SIGHT_RANGE 1200.0f    // monster wake / DG threat-scan distance
#define HITSCAN_RANGE 2048.0f
#define DG_EYE 40.0f
#define DG_LOW_HP 40
#define MOB_HEIGHT 56.0f

static bool PlanCurrentObjective(struct Sim *sim, const char *reason);
static void DgSetState(struct Sim *sim, DGState st, const char *reason);
static void DamageBarrel(struct Sim *sim, int idx, int dmg, int attackerMob);

static void Snd(struct Sim *sim, int id, Vector2 pos)
{
    sim->sndEvents[sim->sndHead % SND_EVENTS_MAX] = (SndEvent){ id, pos };
    sim->sndHead++;
}

// Per-kind voice lines: sight, pain, death, melee.
static const int MOB_SND_SIGHT[MK_COUNT] = {
    SND_POSIT, SND_POSIT, SND_BGSIT, SND_SGTSIT, SND_CACSIT,
    SND_SKLATK, SND_SKESIT, SND_BRSSIT, SND_VILSIT,
};
static const int MOB_SND_PAIN[MK_COUNT] = {
    SND_POPAIN, SND_POPAIN, SND_POPAIN, SND_DMPAIN, SND_DMPAIN,
    SND_DMPAIN, SND_POPAIN, SND_DMPAIN, SND_VIPAIN,
};
static const int MOB_SND_DEATH[MK_COUNT] = {
    SND_PODTH, SND_PODTH, SND_BGDTH, SND_SGTDTH, SND_CACDTH,
    SND_FIRXPL, SND_SKEDTH, SND_BRSDTH, SND_VILDTH,
};

// Vanilla-flavored bestiary stats. Damage dice follow Doom's idiom:
// melee/missile damage = (1dDie) * Mult; hitscan pellets each 5*(1d3).
typedef struct MobInfo {
    const char *name;
    int ednum;
    int health;
    float radius;
    float speed;           // units per tic while chasing
    int painChance;        // 0..255
    float meleeRange;      // 0 = no melee
    int meleeMult, meleeDie;
    bool hasMissile;
    float missileSpeed;    // units per tic
    int missileMult;       // damage = (1d8) * mult
    int pellets;           // hitscan pellet count, 0 = not a hitscanner
    bool flyer;
    int drop;              // PickupType, -1 = none
    int cost;              // souls, DESIGN.md bestiary curve
} MobInfo;

static const MobInfo MOB_INFO[MK_COUNT] = {
    [MK_ZOMBIE]     = { "zombieman",  3004,  20, 20.0f, 1.2f, 200,
                        0.0f, 0, 0,  false, 0.0f, 0,  1, false, PK_CLIP, 8 },
    [MK_SHOTGUNGUY] = { "shotgunguy",    9,  30, 20.0f, 1.2f, 170,
                        0.0f, 0, 0,  false, 0.0f, 0,  3, false, PK_SHOTGUN, 14 },
    [MK_IMP]        = { "imp",        3001,  60, 20.0f, 1.2f, 200,
                        70.0f, 3, 8,  true, 10.0f, 3,  0, false, -1, 22 },
    [MK_PINKY]      = { "pinky",      3002, 150, 30.0f, 2.4f, 180,
                        80.0f, 4, 10, false, 0.0f, 0,  0, false, -1, 38 },
    [MK_CACO]       = { "cacodemon",  3005, 400, 31.0f, 1.6f, 128,
                        90.0f, 10, 6,  true, 10.0f, 5,  0, true, -1, 65 },
    // The lost soul's charge is special-cased in MobTick (no normal attack).
    [MK_LOSTSOUL]   = { "lostsoul",   3006, 100, 16.0f, 1.7f, 256,
                        0.0f, 0, 0,  false, 0.0f, 0,  0, true, -1, 28 },
    [MK_REVENANT]   = { "revenant",     66, 300, 20.0f, 2.0f, 100,
                        70.0f, 6, 10,  true, 10.0f, 10, 0, false, -1, 110 },
    // The Baron Tax: drops nothing, walks slow, eats his ammo reserves.
    [MK_BARON]      = { "baron",      3003, 1000, 24.0f, 1.4f, 50,
                        80.0f, 10, 8,  true, 15.0f, 8,  0, false, -1, 170 },
    // The flame attack and resurrection are special-cased in MobTick.
    [MK_ARCHVILE]   = { "archvile",     64, 700, 20.0f, 3.0f,  10,
                        0.0f, 0, 0,  false, 0.0f, 0,  0, false, -1, 220 },
};

#define SKULL_CHARGE_SPEED 16.0f
#define SKULL_CHARGE_TICS 24

#define VILE_FLAME_TICS 24      // channel length; damage lands near the end
#define VILE_FLAME_RANGE 900.0f
#define VILE_FLAME_DMG 20       // vanilla direct hit
#define VILE_RAISE_RADIUS 120.0f
#define VILE_RAISE_TICS 12
#define VILE_RAISE_COOLDOWN 105 // DESIGN.md: ~one resurrection per 3s

int SimDropBaseCost(MobKind kind)
{
    return (kind >= 0 && kind < MK_COUNT) ? MOB_INFO[kind].cost : 0;
}

const char *MobKindName(MobKind k)
{
    return (k >= 0 && k < MK_COUNT) ? MOB_INFO[k].name : "?";
}

float MobRadius(MobKind k)
{
    return (k >= 0 && k < MK_COUNT) ? MOB_INFO[k].radius : 20.0f;
}

bool MobIsFlyer(MobKind k)
{
    return (k >= 0 && k < MK_COUNT) ? MOB_INFO[k].flyer : false;
}

int MobMaxHealth(MobKind k)
{
    return (k >= 0 && k < MK_COUNT) ? MOB_INFO[k].health : 1;
}

const char *MobStateName(MobState s)
{
    switch (s) {
    case MOB_IDLE:   return "idle";
    case MOB_CHASE:  return "chase";
    case MOB_ATTACK: return "attack";
    case MOB_PAIN:   return "pain";
    case MOB_RAISE:  return "raise";
    case MOB_DEAD:   return "dead";
    }
    return "?";
}

// ---------------------------------------------------------------- small math

static float Dist2D(Vector2 a, Vector2 b)
{
    float dx = a.x - b.x, dy = a.y - b.y;
    return sqrtf(dx*dx + dy*dy);
}

static bool PtInTri2D(float px, float pz,
                      float ax, float az, float bx, float bz, float cx, float cz)
{
    const float EPS = 0.5f;
    float d1 = (px - bx)*(az - bz) - (ax - bx)*(pz - bz);
    float d2 = (px - cx)*(bz - cz) - (bx - cx)*(pz - cz);
    float d3 = (px - ax)*(cz - az) - (cx - ax)*(pz - az);
    bool hasNeg = (d1 < -EPS) || (d2 < -EPS) || (d3 < -EPS);
    bool hasPos = (d1 >  EPS) || (d2 >  EPS) || (d3 >  EPS);
    return !(hasNeg && hasPos);
}

static Vector2 SegClosest(Vector2 a, Vector2 b, Vector2 p)
{
    float abx = b.x - a.x, aby = b.y - a.y;
    float len2 = abx*abx + aby*aby;
    if (len2 <= 0.0001f) return a;
    float t = ((p.x - a.x)*abx + (p.y - a.y)*aby) / len2;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return (Vector2){ a.x + abx*t, a.y + aby*t };
}

// ---------------------------------------------------------------- random

int SimRandom(Sim *sim)
{
    sim->rndIndex = (sim->rndIndex + 1) & 0xFF;
    return sim->rnd[sim->rndIndex];
}

static void RndInit(Sim *sim)
{
    // Fixed xorshift32 fill: P_Random's shape (bounded table, index state)
    // without copying id's GPL byte table. Determinism is the contract.
    unsigned x = 0x29A1D903u;
    for (int i = 0; i < 256; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        sim->rnd[i] = (unsigned char)(x & 0xFF);
    }
    sim->rndIndex = 0;
}

// ---------------------------------------------------------------- sectors

// Floor triangle containing (x, y); overlapping floors pick the highest.
static int TriAtPoint(const Sim *sim, float x, float y)
{
    const Mesh *m = &sim->lm->floors;
    const float *v = m->vertices;
    if (!v || !sim->lm->floorTriSector) return -1;
    float pz = -y;
    int best = -1;
    float bestH = -FLT_MAX;
    for (int t = 0; t < m->triangleCount; t++) {
        const float *a = &v[(size_t)t*9], *b = a + 3, *c = a + 6;
        if (!PtInTri2D(x, pz, a[0], a[2], b[0], b[2], c[0], c[2])) continue;
        if (a[1] > bestH) { bestH = a[1]; best = t; }
    }
    return best;
}

int SimSectorAtPoint(const Sim *sim, float x, float y)
{
    int t = TriAtPoint(sim, x, y);
    return (t >= 0) ? sim->lm->floorTriSector[t] : -1;
}

float SimSectorFloorNow(const Sim *sim, int sector)
{
    if (sector < 0) return 0.0f;
    int li = sim->sectorLift ? sim->sectorLift[sector] : -1;
    return (li >= 0) ? sim->lifts[li].floor : sim->map->sectors[sector].floorH;
}

float SimFloorAtPoint(const Sim *sim, float x, float y, bool *ok)
{
    int s = SimSectorAtPoint(sim, x, y);
    if (ok) *ok = (s >= 0);
    return (s >= 0) ? SimSectorFloorNow(sim, s) : 0.0f;
}

// Triangle corner in Doom coords (mesh stores world (x, h, -y)).
static Vector2 TriCorner(const Sim *sim, int tri, int corner)
{
    const float *p = &sim->lm->floors.vertices[(size_t)tri*9 + (size_t)corner*3];
    return (Vector2){ p[0], -p[2] };
}

// ---------------------------------------------------------------- doors

static const struct { int special; float speed; bool stay; bool locked; } DOOR_SPECIALS[] = {
    { 1,   2.0f, false, false },   // DR open wait close
    { 117, 8.0f, false, false },   // DR blaze
    { 31,  2.0f, true,  false },   // D1 open stay
    { 118, 8.0f, true,  false },   // D1 blaze
    { 26,  2.0f, false, true },    // DR blue key
    { 27,  2.0f, false, true },    // DR yellow key
    { 28,  2.0f, false, true },    // DR red key
    { 32,  2.0f, true,  true },    // D1 blue key
    { 33,  2.0f, true,  true },    // D1 yellow key
    { 34,  2.0f, true,  true },    // D1 red key
};

static int DoorSpecialIndex(int special)
{
    for (size_t i = 0; i < sizeof(DOOR_SPECIALS)/sizeof(DOOR_SPECIALS[0]); i++) {
        if (DOOR_SPECIALS[i].special == special) return (int)i;
    }
    return -1;
}

static void LogDoorEvent(const Sim *sim, int door, const char *action)
{
    if (!StateLogEnabled()) return;
    StateLogBegin("door");
    StateLogInt("tic", sim->tic);
    StateLogInt("door", door);
    StateLogInt("sector", sim->doors[door].sector);
    StateLogStr("action", action);
    StateLogFloat("ceil", sim->doors[door].ceil);
    StateLogEnd();
}

static Vector2 DoorCenter(const Sim *sim, int di)
{
    // Cheap stand-in for the door's position: any vertex of any line
    // bordering its sector would do; use Doom Guy-independent map data.
    int ds = sim->doors[di].sector;
    for (int i = 0; i < sim->map->lineCount; i++) {
        const Linedef *l = &sim->map->lines[i];
        if (l->sideBack >= 0 && sim->map->sides[l->sideBack].sector == ds) {
            Vector2 a = sim->map->verts[l->v1], b = sim->map->verts[l->v2];
            return (Vector2){ (a.x + b.x)*0.5f, (a.y + b.y)*0.5f };
        }
    }
    return sim->dg.pos;
}

static void DoorUse(Sim *sim, int di)
{
    Door *d = &sim->doors[di];
    if (sim->tic < d->holdUntil) return;   // Hell's Hand holds it shut
    switch (d->state) {
    case DOOR_CLOSED:
        d->state = DOOR_OPENING;
        Snd(sim, SND_DOROPN, DoorCenter(sim, di));
        if (d->locked && !d->lockLogged) {
            d->lockLogged = true;
            TraceLog(LOG_WARNING,
                     "SIM: door sector %d is key-locked; no key system yet, opening anyway",
                     d->sector);
            LogDoorEvent(sim, di, "locked_ignored");
        }
        LogDoorEvent(sim, di, "use_open");
        break;
    case DOOR_CLOSING:
        d->state = DOOR_OPENING;
        LogDoorEvent(sim, di, "use_reopen");
        break;
    default:
        break;
    }
}

static void DoorTick(Sim *sim, int di)
{
    Door *d = &sim->doors[di];
    float floorH = sim->map->sectors[d->sector].floorH;
    d->prevCeil = d->ceil;
    switch (d->state) {
    case DOOR_OPENING:
        d->ceil += d->speed;
        if (d->ceil >= d->openCeil) {
            d->ceil = d->openCeil;
            d->state = DOOR_OPEN;
            d->timer = d->stayOpen ? -1 : DOOR_WAIT_TICS;
            LogDoorEvent(sim, di, "open");
        }
        break;
    case DOOR_OPEN:
        if (!d->stayOpen && d->timer > 0 && --d->timer == 0) {
            if (sim->dg.sector == d->sector) {
                d->timer = SIM_TICRATE;   // someone in the doorway: hold
            } else {
                d->state = DOOR_CLOSING;
                Snd(sim, SND_DORCLS, DoorCenter(sim, di));
                LogDoorEvent(sim, di, "closing");
            }
        }
        break;
    case DOOR_CLOSING:
        d->ceil -= d->speed;
        if (sim->dg.sector == d->sector && d->ceil - floorH < DG_HEIGHT) {
            d->state = DOOR_OPENING;   // vanilla doors reverse on obstruction
            LogDoorEvent(sim, di, "reverse_obstructed");
        } else if (d->ceil <= floorH) {
            d->ceil = floorH;
            d->state = DOOR_CLOSED;
            LogDoorEvent(sim, di, "closed");
        }
        break;
    case DOOR_CLOSED:
        break;
    }
}

static bool BuildDoors(Sim *sim)
{
    const DoomMap *map = sim->map;
    sim->sectorDoor = (int *)malloc((size_t)map->sectorCount * sizeof(int));
    if (!sim->sectorDoor) return false;
    for (int s = 0; s < map->sectorCount; s++) sim->sectorDoor[s] = -1;

    sim->doors = (Door *)calloc((size_t)map->sectorCount, sizeof(Door));
    if (!sim->doors) return false;
    sim->doorCount = 0;

    for (int i = 0; i < map->lineCount; i++) {
        const Linedef *l = &map->lines[i];
        int spec = DoorSpecialIndex(l->special);
        if (spec < 0 || l->sideBack < 0) continue;
        int ds = map->sides[l->sideBack].sector;
        if (ds < 0 || sim->sectorDoor[ds] >= 0) continue;

        // Open target: lowest ceiling among neighbor sectors, minus 4.
        float minNeighborCeil = FLT_MAX;
        for (int j = 0; j < map->lineCount; j++) {
            const Linedef *n = &map->lines[j];
            if (n->sideFront < 0 || n->sideBack < 0) continue;
            int fs = map->sides[n->sideFront].sector;
            int bs = map->sides[n->sideBack].sector;
            int other = (fs == ds) ? bs : ((bs == ds) ? fs : -1);
            if (other < 0 || other == ds) continue;
            float c = map->sectors[other].ceilH;
            if (c < minNeighborCeil) minNeighborCeil = c;
        }
        if (minNeighborCeil == FLT_MAX) continue;   // sealed box, not a real door

        Door *d = &sim->doors[sim->doorCount];
        d->sector = ds;
        d->ceil = map->sectors[ds].ceilH;
        d->prevCeil = d->ceil;
        d->openCeil = minNeighborCeil - 4.0f;
        d->speed = DOOR_SPECIALS[spec].speed;
        d->stayOpen = DOOR_SPECIALS[spec].stay;
        d->locked = DOOR_SPECIALS[spec].locked;
        if (d->openCeil <= map->sectors[ds].floorH) continue;   // can never open
        if (d->ceil - map->sectors[ds].floorH >= DG_HEIGHT) {
            d->state = DOOR_OPEN;     // mapper left it open; treat as propped
            d->stayOpen = true;
            d->timer = -1;
        } else {
            d->state = DOOR_CLOSED;
        }
        sim->sectorDoor[ds] = sim->doorCount;
        sim->doorCount++;
    }
    return true;
}

// ---------------------------------------------------------------- nav graph

// Effective ceiling for passability: doors count at their OPEN height, since
// the route may open them. Live blocking uses the current ceiling instead.
static float PotentialCeil(const Sim *sim, int sector)
{
    int di = sim->sectorDoor[sector];
    return (di >= 0) ? sim->doors[di].openCeil : sim->map->sectors[sector].ceilH;
}

// Potential floors for routing: a lift counts at its LOW height when being
// stepped onto (it can come down for you) and at its HIGH height when being
// left (it carries you up).
static float PotentialFloorLow(const Sim *sim, int sector)
{
    int li = sim->sectorLift[sector];
    return (li >= 0) ? sim->lifts[li].lowFloor : sim->map->sectors[sector].floorH;
}

static float PotentialFloorHigh(const Sim *sim, int sector)
{
    int li = sim->sectorLift[sector];
    return (li >= 0) ? sim->lifts[li].highFloor : sim->map->sectors[sector].floorH;
}

static bool EdgePassable(const Sim *sim, int from, int to, const Linedef *l)
{
    if (PotentialFloorLow(sim, to) - PotentialFloorHigh(sim, from) > DG_STEP)
        return false;                                    // climb limit; drops ok
    float top = fminf(PotentialCeil(sim, from), PotentialCeil(sim, to));
    float bottom = fmaxf(PotentialFloorLow(sim, from), PotentialFloorLow(sim, to));
    if (top - bottom < DG_HEIGHT) return false;
    if (l->flags & ML_BLOCKING) return false;
    return true;
}

static bool BuildNav(Sim *sim)
{
    const DoomMap *map = sim->map;
    int S = map->sectorCount;
    sim->edgeStart = (int *)calloc((size_t)S + 1, sizeof(int));
    if (!sim->edgeStart) return false;

    for (int pass = 0; pass < 2; pass++) {
        int *fill = NULL;
        if (pass == 1) {
            for (int s = 0; s < S; s++) sim->edgeStart[s + 1] += sim->edgeStart[s];
            sim->edgeCount = sim->edgeStart[S];
            sim->edges = (NavEdge *)malloc((size_t)(sim->edgeCount > 0 ? sim->edgeCount : 1)
                                           * sizeof(NavEdge));
            fill = (int *)malloc((size_t)S * sizeof(int));
            if (!sim->edges || !fill) { free(fill); return false; }
            memcpy(fill, sim->edgeStart, (size_t)S * sizeof(int));
        }
        for (int i = 0; i < map->lineCount; i++) {
            const Linedef *l = &map->lines[i];
            if (l->sideFront < 0 || l->sideBack < 0) continue;
            int fs = map->sides[l->sideFront].sector;
            int bs = map->sides[l->sideBack].sector;
            if (fs < 0 || bs < 0 || fs == bs) continue;
            Vector2 v1 = map->verts[l->v1], v2 = map->verts[l->v2];
            Vector2 mid = { (v1.x + v2.x)*0.5f, (v1.y + v2.y)*0.5f };
            const int dirs[2][2] = { { fs, bs }, { bs, fs } };
            for (int k = 0; k < 2; k++) {
                int from = dirs[k][0], to = dirs[k][1];
                if (!EdgePassable(sim, from, to, l)) continue;
                if (pass == 0) {
                    sim->edgeStart[from + 1]++;
                } else {
                    int door = (sim->sectorDoor[to] >= 0) ? sim->sectorDoor[to]
                                                          : sim->sectorDoor[from];
                    int lift = (sim->sectorLift[to] >= 0) ? sim->sectorLift[to]
                                                          : sim->sectorLift[from];
                    sim->edges[fill[from]++] = (NavEdge){
                        .to = to, .line = i, .door = door, .lift = lift, .wp = mid };
                }
            }
        }
        if (pass == 1) free(fill);
    }
    return true;
}

// ---------------------------------------------------------------- lifts

#define LIFT_WAIT_TICS 105    // vanilla PLATWAIT (3s)

static const struct { int special; float speed; } LIFT_SPECIALS[] = {
    { 10, 4.0f }, { 21, 4.0f }, { 62, 4.0f }, { 88, 4.0f },   // vanilla plats
    { 120, 8.0f }, { 121, 8.0f }, { 122, 8.0f }, { 123, 8.0f }, // blaze
};

static int LiftSpecialIndex(int special)
{
    for (size_t i = 0; i < sizeof(LIFT_SPECIALS)/sizeof(LIFT_SPECIALS[0]); i++) {
        if (LIFT_SPECIALS[i].special == special) return (int)i;
    }
    return -1;
}

static Vector2 SectorAnyPoint(const Sim *sim, int sector)
{
    for (int i = 0; i < sim->map->lineCount; i++) {
        const Linedef *l = &sim->map->lines[i];
        int fs = (l->sideFront >= 0) ? sim->map->sides[l->sideFront].sector : -1;
        int bs = (l->sideBack >= 0) ? sim->map->sides[l->sideBack].sector : -1;
        if (fs == sector || bs == sector) {
            Vector2 a = sim->map->verts[l->v1], b = sim->map->verts[l->v2];
            return (Vector2){ (a.x + b.x)*0.5f, (a.y + b.y)*0.5f };
        }
    }
    return sim->dg.pos;
}

// Lift sectors are the TAGGED sectors of plat linedef specials (unlike
// doors, which act on the line's back sector).
static bool BuildLifts(Sim *sim)
{
    const DoomMap *map = sim->map;
    sim->sectorLift = (int *)malloc((size_t)map->sectorCount * sizeof(int));
    if (!sim->sectorLift) return false;
    for (int s = 0; s < map->sectorCount; s++) sim->sectorLift[s] = -1;
    sim->lifts = (Lift *)calloc((size_t)map->sectorCount, sizeof(Lift));
    if (!sim->lifts) return false;
    sim->liftCount = 0;

    for (int i = 0; i < map->lineCount; i++) {
        const Linedef *l = &map->lines[i];
        int spec = LiftSpecialIndex(l->special);
        if (spec < 0 || l->tag <= 0) continue;
        for (int s = 0; s < map->sectorCount; s++) {
            if (map->sectors[s].tag != l->tag || sim->sectorLift[s] >= 0) continue;
            float lowest = FLT_MAX;
            for (int j = 0; j < map->lineCount; j++) {
                const Linedef *n = &map->lines[j];
                if (n->sideFront < 0 || n->sideBack < 0) continue;
                int fs = map->sides[n->sideFront].sector;
                int bs = map->sides[n->sideBack].sector;
                int other = (fs == s) ? bs : ((bs == s) ? fs : -1);
                if (other < 0 || other == s) continue;
                float f = map->sectors[other].floorH;
                if (f < lowest) lowest = f;
            }
            if (lowest == FLT_MAX || lowest >= map->sectors[s].floorH) continue;
            Lift *lf = &sim->lifts[sim->liftCount];
            lf->sector = s;
            lf->floor = lf->prevFloor = lf->highFloor = map->sectors[s].floorH;
            lf->lowFloor = lowest;
            lf->speed = LIFT_SPECIALS[spec].speed;
            lf->state = LIFT_UP;
            sim->sectorLift[s] = sim->liftCount;
            sim->liftCount++;
        }
    }
    return true;
}

static void LiftActivate(Sim *sim, int li)
{
    Lift *l = &sim->lifts[li];
    if (l->riser) {
        if (l->state != LIFT_LOW) return;
        l->state = LIFT_RISING;
        Snd(sim, SND_PSTART, SectorAnyPoint(sim, l->sector));
        if (StateLogEnabled()) {
            StateLogBegin("lift");
            StateLogInt("tic", sim->tic);
            StateLogInt("sector", l->sector);
            StateLogStr("action", "raise");
            StateLogEnd();
        }
        return;
    }
    if (l->state != LIFT_UP) return;
    l->state = LIFT_DOWN;
    Snd(sim, SND_PSTART, SectorAnyPoint(sim, l->sector));
    if (StateLogEnabled()) {
        StateLogBegin("lift");
        StateLogInt("tic", sim->tic);
        StateLogInt("sector", l->sector);
        StateLogStr("action", "down");
        StateLogEnd();
    }
}

static void LiftTick(Sim *sim, int li)
{
    Lift *l = &sim->lifts[li];
    l->prevFloor = l->floor;
    switch (l->state) {
    case LIFT_DOWN:
        l->floor -= l->speed;
        if (l->floor <= l->lowFloor) {
            l->floor = l->lowFloor;
            l->state = LIFT_LOW;
            // A recalled lift sulks at the bottom for the verb's hold time.
            l->timer = LIFT_WAIT_TICS + (l->recalled ? DOOR_HOLD_TICS : 0);
            l->recalled = false;
            Snd(sim, SND_PSTOP, SectorAnyPoint(sim, l->sector));
        }
        break;
    case LIFT_LOW:
        if (l->stayLow) break;   // one-shot floor-lower barriers rest here
        if (--l->timer <= 0) {
            l->state = LIFT_RISING;
            Snd(sim, SND_PSTART, SectorAnyPoint(sim, l->sector));
        }
        break;
    case LIFT_RISING:
        l->floor += l->speed;
        if (l->floor >= l->highFloor) {
            l->floor = l->highFloor;
            l->state = LIFT_UP;
            Snd(sim, SND_PSTOP, SectorAnyPoint(sim, l->sector));
        }
        break;
    case LIFT_UP:
        break;
    }
}

// Any plat trigger line for this lift within use range of the point?
static bool LiftTriggerInRange(const Sim *sim, int li, Vector2 from)
{
    int tag = sim->map->sectors[sim->lifts[li].sector].tag;
    for (int i = 0; i < sim->map->lineCount; i++) {
        const Linedef *l = &sim->map->lines[i];
        if (l->tag != tag || LiftSpecialIndex(l->special) < 0) continue;
        Vector2 q = SegClosest(sim->map->verts[l->v1], sim->map->verts[l->v2], from);
        if (Dist2D(q, from) <= DG_USERANGE) return true;
    }
    return false;
}

// ------------------------------------------------------------ line actions
//
// The vanilla trigger system, reduced to what progression needs: tagged
// doors, plats, one-shot floor-lower barriers, teleports, and exits, fired
// by walking over (W) or using (S) the line. Doom Guy plays it like a
// speedrunner: he triggers every walkover he crosses and presses every
// switch he passes.

typedef enum ActKind {
    ACT_NONE, ACT_DOOR_OPEN, ACT_DOOR_OWC, ACT_DOOR_CLOSE,
    ACT_PLAT, ACT_FLOOR_LOWER, ACT_FLOOR_RAISE, ACT_TELEPORT, ACT_EXIT
} ActKind;

typedef struct LineAction {
    ActKind kind;
    bool use;       // S-type: pressed
    bool walk;      // W-type: crossed
    bool repeat;    // SR/WR
} LineAction;

static LineAction ActionForSpecial(int sp)
{
    switch (sp) {
    // tagged doors
    case 2:   return (LineAction){ ACT_DOOR_OPEN,  false, true,  false };
    case 3:   return (LineAction){ ACT_DOOR_CLOSE, false, true,  false };
    case 4:   return (LineAction){ ACT_DOOR_OWC,   false, true,  false };
    case 29:  return (LineAction){ ACT_DOOR_OWC,   true,  false, false };
    case 42:  return (LineAction){ ACT_DOOR_CLOSE, true,  false, true };
    case 50:  return (LineAction){ ACT_DOOR_CLOSE, true,  false, false };
    case 61:  return (LineAction){ ACT_DOOR_OPEN,  true,  false, true };
    case 63:  return (LineAction){ ACT_DOOR_OWC,   true,  false, true };
    case 86:  return (LineAction){ ACT_DOOR_OPEN,  false, true,  true };
    case 90:  return (LineAction){ ACT_DOOR_OWC,   false, true,  true };
    case 103: return (LineAction){ ACT_DOOR_OPEN,  true,  false, false };
    case 105: return (LineAction){ ACT_DOOR_OWC,   false, true,  true };
    case 106: return (LineAction){ ACT_DOOR_OPEN,  false, true,  true };
    case 108: return (LineAction){ ACT_DOOR_OWC,   false, true,  false };
    case 109: return (LineAction){ ACT_DOOR_OPEN,  false, true,  false };
    case 111: return (LineAction){ ACT_DOOR_OWC,   true,  false, false };
    case 112: return (LineAction){ ACT_DOOR_OWC,   true,  false, false };
    case 113: return (LineAction){ ACT_DOOR_OPEN,  true,  false, false };
    case 114: return (LineAction){ ACT_DOOR_OWC,   true,  false, true };
    case 115: return (LineAction){ ACT_DOOR_OPEN,  true,  false, true };
    // plats (already lift sectors; these fire them remotely)
    case 10: case 121: return (LineAction){ ACT_PLAT, false, true,  false };
    case 88: case 120: return (LineAction){ ACT_PLAT, false, true,  true };
    case 21: case 122: return (LineAction){ ACT_PLAT, true,  false, false };
    case 62: case 123: return (LineAction){ ACT_PLAT, true,  false, true };
    // one-shot floor lowers (barriers)
    case 23: case 36: return (LineAction){ ACT_FLOOR_LOWER, true,  false, false };
    case 38: case 19: return (LineAction){ ACT_FLOOR_LOWER, false, true,  false };
    case 60: case 70: return (LineAction){ ACT_FLOOR_LOWER, true,  false, true };
    case 82: case 83: case 98: return (LineAction){ ACT_FLOOR_LOWER, false, true, true };
    // floor raises (steps/bridges that come up to meet you)
    case 18: case 20: case 64: case 101:
        return (LineAction){ ACT_FLOOR_RAISE, true,  false, false };
    case 69: case 91: return (LineAction){ ACT_FLOOR_RAISE, true,  false, true };
    case 5: case 119: return (LineAction){ ACT_FLOOR_RAISE, false, true,  false };
    case 128:         return (LineAction){ ACT_FLOOR_RAISE, false, true,  true };
    // teleports
    case 39: return (LineAction){ ACT_TELEPORT, false, true, false };
    case 97: return (LineAction){ ACT_TELEPORT, false, true, true };
    // exits
    case 11: case 51:  return (LineAction){ ACT_EXIT, true,  false, false };
    case 52: case 124: return (LineAction){ ACT_EXIT, false, true,  false };
    default: return (LineAction){ ACT_NONE, false, false, false };
    }
}

static void DgTriggerExit(Sim *sim);
static bool SegsCross(Vector2 a, Vector2 b, Vector2 c, Vector2 d);

// Sectors targeted by tagged door/floor actions get Door/Lift state
// machines too, so nav potentials and live blocking treat them like their
// manually-operated cousins.
static void BuildTaggedActions(Sim *sim)
{
    const DoomMap *map = sim->map;
    for (int i = 0; i < map->lineCount; i++) {
        const Linedef *l = &map->lines[i];
        if (l->tag <= 0) continue;
        LineAction act = ActionForSpecial(l->special);
        bool isDoor = (act.kind == ACT_DOOR_OPEN || act.kind == ACT_DOOR_OWC ||
                       act.kind == ACT_DOOR_CLOSE);
        bool isFloor = (act.kind == ACT_FLOOR_LOWER);
        bool isRaise = (act.kind == ACT_FLOOR_RAISE);
        if (!isDoor && !isFloor && !isRaise) continue;
        for (int s = 0; s < map->sectorCount; s++) {
            if (map->sectors[s].tag != l->tag) continue;
            if (isDoor && sim->sectorDoor[s] < 0) {
                float minC = FLT_MAX;
                for (int j = 0; j < map->lineCount; j++) {
                    const Linedef *n = &map->lines[j];
                    if (n->sideFront < 0 || n->sideBack < 0) continue;
                    int fs = map->sides[n->sideFront].sector;
                    int bs = map->sides[n->sideBack].sector;
                    int other = (fs == s) ? bs : ((bs == s) ? fs : -1);
                    if (other < 0 || other == s) continue;
                    if (map->sectors[other].ceilH < minC) minC = map->sectors[other].ceilH;
                }
                if (minC == FLT_MAX) continue;
                Door *d = &sim->doors[sim->doorCount];
                memset(d, 0, sizeof(*d));
                d->sector = s;
                d->ceil = d->prevCeil = map->sectors[s].ceilH;
                d->openCeil = minC - 4.0f;
                d->speed = 2.0f;
                if (d->openCeil <= map->sectors[s].floorH) continue;
                if (d->ceil - map->sectors[s].floorH >= DG_HEIGHT) {
                    d->state = DOOR_OPEN;
                    d->stayOpen = true;
                    d->timer = -1;
                } else {
                    d->state = DOOR_CLOSED;
                }
                sim->sectorDoor[s] = sim->doorCount++;
            } else if (isFloor && sim->sectorLift[s] < 0) {
                float lowest = FLT_MAX;
                for (int j = 0; j < map->lineCount; j++) {
                    const Linedef *n = &map->lines[j];
                    if (n->sideFront < 0 || n->sideBack < 0) continue;
                    int fs = map->sides[n->sideFront].sector;
                    int bs = map->sides[n->sideBack].sector;
                    int other = (fs == s) ? bs : ((bs == s) ? fs : -1);
                    if (other < 0 || other == s) continue;
                    if (map->sectors[other].floorH < lowest) lowest = map->sectors[other].floorH;
                }
                if (lowest == FLT_MAX || lowest >= map->sectors[s].floorH) continue;
                Lift *lf = &sim->lifts[sim->liftCount];
                memset(lf, 0, sizeof(*lf));
                lf->sector = s;
                lf->floor = lf->prevFloor = lf->highFloor = map->sectors[s].floorH;
                lf->lowFloor = lowest;
                lf->speed = 2.0f;
                lf->state = LIFT_UP;
                lf->stayLow = true;
                sim->sectorLift[s] = sim->liftCount++;
            } else if (isRaise && sim->sectorLift[s] < 0) {
                // Destination: the next-higher neighbor floor.
                float next = FLT_MAX;
                for (int j = 0; j < map->lineCount; j++) {
                    const Linedef *n = &map->lines[j];
                    if (n->sideFront < 0 || n->sideBack < 0) continue;
                    int fs = map->sides[n->sideFront].sector;
                    int bs = map->sides[n->sideBack].sector;
                    int other = (fs == s) ? bs : ((bs == s) ? fs : -1);
                    if (other < 0 || other == s) continue;
                    float f = map->sectors[other].floorH;
                    if (f > map->sectors[s].floorH && f < next) next = f;
                }
                if (next == FLT_MAX) continue;
                Lift *lf = &sim->lifts[sim->liftCount];
                memset(lf, 0, sizeof(*lf));
                lf->sector = s;
                lf->floor = lf->prevFloor = lf->lowFloor = map->sectors[s].floorH;
                lf->highFloor = next;
                lf->speed = 1.0f;          // vanilla floor-move speed
                lf->state = LIFT_LOW;
                lf->stayLow = true;        // holds until activated
                lf->riser = true;
                sim->sectorLift[s] = sim->liftCount++;
            }
        }
    }
}

static void RunLineAction(Sim *sim, int lineIdx)
{
    const Linedef *l = &sim->map->lines[lineIdx];
    LineAction act = ActionForSpecial(l->special);
    if (act.kind == ACT_NONE) return;
    if (sim->lineUsed[lineIdx] && !act.repeat) return;
    sim->lineUsed[lineIdx] = 1;

    if (act.kind == ACT_EXIT) {
        DgTriggerExit(sim);
        return;
    }
    if (act.kind == ACT_TELEPORT) {
        // Send him to the teleport-destination thing in the tagged sector.
        for (int t = 0; t < sim->map->thingCount; t++) {
            const Thing *th = &sim->map->things[t];
            if (th->type != 14) continue;
            int ds = SimSectorAtPoint(sim, th->x, th->y);
            if (ds < 0 || sim->map->sectors[ds].tag != l->tag) continue;
            sim->dg.pos = sim->dg.prevPos = (Vector2){ th->x, th->y };
            sim->dg.sector = ds;
            sim->dg.z = sim->dg.prevZ = SimSectorFloorNow(sim, ds);
            Snd(sim, SND_TELEPT, sim->dg.pos);
            if (StateLogEnabled()) {
                StateLogBegin("teleport");
                StateLogInt("tic", sim->tic);
                StateLogFloat("x", th->x);
                StateLogFloat("y", th->y);
                StateLogEnd();
            }
            PlanCurrentObjective(sim, "teleported");
            return;
        }
        return;
    }
    for (int s = 0; s < sim->map->sectorCount; s++) {
        if (sim->map->sectors[s].tag != l->tag || l->tag <= 0) continue;
        if (act.kind == ACT_PLAT || act.kind == ACT_FLOOR_LOWER ||
            act.kind == ACT_FLOOR_RAISE) {
            int li = sim->sectorLift[s];
            if (li >= 0) LiftActivate(sim, li);
        } else {
            int di = sim->sectorDoor[s];
            if (di < 0) continue;
            Door *d = &sim->doors[di];
            if (act.kind == ACT_DOOR_CLOSE) {
                if (d->state == DOOR_OPEN || d->state == DOOR_OPENING) {
                    d->state = DOOR_CLOSING;
                    LogDoorEvent(sim, di, "remote_close");
                }
            } else {
                d->stayOpen = (act.kind == ACT_DOOR_OPEN);
                DoorUse(sim, di);
            }
        }
    }
}

// Speedrunner trigger discipline: fire walkover lines crossed this move,
// and press usable switches within reach every few tics.
static void DgRunTriggers(Sim *sim, Vector2 from, Vector2 to)
{
    sim->stamp++;
    float pad = DG_RADIUS + 4.0f;
    int x0 = (int)((fminf(from.x, to.x) - pad - sim->bmOrgX) / BM_CELL);
    int x1 = (int)((fmaxf(from.x, to.x) + pad - sim->bmOrgX) / BM_CELL);
    int y0 = (int)((fminf(from.y, to.y) - pad - sim->bmOrgY) / BM_CELL);
    int y1 = (int)((fmaxf(from.y, to.y) + pad - sim->bmOrgY) / BM_CELL);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= sim->bmW) x1 = sim->bmW - 1;
    if (y1 >= sim->bmH) y1 = sim->bmH - 1;
    bool pressTick = (sim->tic % 8) == 0;
    for (int cy = y0; cy <= y1; cy++) {
        for (int cx = x0; cx <= x1; cx++) {
            int c = cy*sim->bmW + cx;
            for (int k = sim->bmStart[c]; k < sim->bmStart[c + 1]; k++) {
                int li = sim->bmLines[k];
                if (sim->lineStamp[li] == sim->stamp) continue;
                sim->lineStamp[li] = sim->stamp;
                const Linedef *l = &sim->map->lines[li];
                LineAction act = ActionForSpecial(l->special);
                if (act.kind == ACT_NONE || act.kind == ACT_EXIT) continue;
                if (sim->lineUsed[li] && !act.repeat) continue;
                Vector2 a = sim->map->verts[l->v1], b = sim->map->verts[l->v2];
                if (act.walk && SegsCross(from, to, a, b)) {
                    RunLineAction(sim, li);
                } else if (act.use && pressTick &&
                           Dist2D(SegClosest(a, b, to), to) <= DG_USERANGE) {
                    RunLineAction(sim, li);
                }
            }
        }
    }
}

// ------------------------------------------------------- triangle nav graph

// Triangle edges are matched by quantized endpoint pairs: per-sector
// tessellations and linedefs both emit exact map-vertex coordinates, so
// shared edges (within a sector AND across portals) collide on the same key.
typedef struct TriEdgeKey {
    unsigned long long a, b;   // packed endpoints, a <= b
    Vector2 p0, p1;
    int tri;
} TriEdgeKey;

typedef struct LineKey {
    unsigned long long a, b;
    int line;
} LineKey;

static unsigned long long PackPt(Vector2 p)
{
    long qx = lroundf(p.x*4.0f), qy = lroundf(p.y*4.0f);
    return ((unsigned long long)(unsigned int)(int)qx << 32)
         | (unsigned long long)(unsigned int)(int)qy;
}

static int TriEdgeKeyCmp(const void *pa, const void *pb)
{
    const TriEdgeKey *x = (const TriEdgeKey *)pa, *y = (const TriEdgeKey *)pb;
    if (x->a != y->a) return (x->a < y->a) ? -1 : 1;
    if (x->b != y->b) return (x->b < y->b) ? -1 : 1;
    return (x->tri < y->tri) ? -1 : ((x->tri > y->tri) ? 1 : 0);
}

static int LineKeyCmp(const void *pa, const void *pb)
{
    const LineKey *x = (const LineKey *)pa, *y = (const LineKey *)pb;
    if (x->a != y->a) return (x->a < y->a) ? -1 : 1;
    if (x->b != y->b) return (x->b < y->b) ? -1 : 1;
    return 0;
}

static int FindLineByKey(const LineKey *lk, int n, unsigned long long a, unsigned long long b)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi)/2;
        if (lk[mid].a == a && lk[mid].b == b) return lk[mid].line;
        if (lk[mid].a < a || (lk[mid].a == a && lk[mid].b < b)) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

// Directional triangle-to-triangle passability; cross-sector crossings must
// ride a real linedef and obey the same rules as sector portals.
static bool TriPairEdge(Sim *sim, const LineKey *lk, int lkN,
                        const TriEdgeKey *from, const TriEdgeKey *to,
                        NavEdge *out)
{
    const DoomMap *map = sim->map;
    int sa = sim->lm->floorTriSector[from->tri];
    int sb = sim->lm->floorTriSector[to->tri];
    int line = -1, door = -1, lift = -1;
    if (sa != sb) {
        line = FindLineByKey(lk, lkN, from->a, from->b);
        if (line < 0) return false;
        const Linedef *l = &map->lines[line];
        if (l->flags & ML_BLOCKING) return false;
        // No per-segment width check: wide openings are often subdivided
        // into many short collinear segments (stair edges); body clearance
        // is enforced by movement collision instead.
        if (PotentialFloorLow(sim, sb) - PotentialFloorHigh(sim, sa) > DG_STEP)
            return false;
        float top = fminf(PotentialCeil(sim, sa), PotentialCeil(sim, sb));
        float bottom = fmaxf(PotentialFloorLow(sim, sa), PotentialFloorLow(sim, sb));
        if (top - bottom < DG_HEIGHT) return false;
        door = (sim->sectorDoor[sb] >= 0) ? sim->sectorDoor[sb] : sim->sectorDoor[sa];
        lift = (sim->sectorLift[sb] >= 0) ? sim->sectorLift[sb] : sim->sectorLift[sa];
    }
    out->to = to->tri;
    out->line = line;
    out->door = door;
    out->lift = lift;
    out->wp = (Vector2){ (from->p0.x + from->p1.x)*0.5f, (from->p0.y + from->p1.y)*0.5f };
    return true;
}

static bool BuildTriNav(Sim *sim)
{
    const DoomMap *map = sim->map;
    int T = sim->lm->floors.triangleCount;
    sim->triCount = (sim->lm->floorTriSector) ? T : 0;
    sim->tEdgeStart = (int *)calloc((size_t)sim->triCount + 1, sizeof(int));
    if (!sim->tEdgeStart) return false;
    if (sim->triCount == 0) return true;   // sector-level fallback still works

    TriEdgeKey *ek = (TriEdgeKey *)malloc((size_t)T*3*sizeof(TriEdgeKey));
    LineKey *lk = (LineKey *)malloc((size_t)(map->lineCount > 0 ? map->lineCount : 1)
                                    * sizeof(LineKey));
    if (!ek || !lk) { free(ek); free(lk); return false; }

    for (int t = 0; t < T; t++) {
        for (int c = 0; c < 3; c++) {
            Vector2 p0 = TriCorner(sim, t, c);
            Vector2 p1 = TriCorner(sim, t, (c + 1)%3);
            unsigned long long ka = PackPt(p0), kb = PackPt(p1);
            if (ka > kb) { unsigned long long tmp = ka; ka = kb; kb = tmp; }
            ek[t*3 + c] = (TriEdgeKey){ ka, kb, p0, p1, t };
        }
    }
    qsort(ek, (size_t)T*3, sizeof(TriEdgeKey), TriEdgeKeyCmp);

    for (int i = 0; i < map->lineCount; i++) {
        unsigned long long ka = PackPt(map->verts[map->lines[i].v1]);
        unsigned long long kb = PackPt(map->verts[map->lines[i].v2]);
        if (ka > kb) { unsigned long long tmp = ka; ka = kb; kb = tmp; }
        lk[i] = (LineKey){ ka, kb, i };
    }
    qsort(lk, (size_t)map->lineCount, sizeof(LineKey), LineKeyCmp);

    for (int pass = 0; pass < 2; pass++) {
        int *fill = NULL;
        if (pass == 1) {
            for (int t = 0; t < sim->triCount; t++) sim->tEdgeStart[t + 1] += sim->tEdgeStart[t];
            sim->tEdgeCount = sim->tEdgeStart[sim->triCount];
            sim->tEdges = (NavEdge *)malloc((size_t)(sim->tEdgeCount > 0 ? sim->tEdgeCount : 1)
                                            * sizeof(NavEdge));
            sim->tEdgeBlocked = (unsigned char *)calloc(
                (size_t)(sim->tEdgeCount > 0 ? sim->tEdgeCount : 1), 1);
            fill = (int *)malloc((size_t)sim->triCount * sizeof(int));
            if (!sim->tEdges || !fill) { free(fill); free(ek); free(lk); return false; }
            memcpy(fill, sim->tEdgeStart, (size_t)sim->triCount * sizeof(int));
        }
        int g0 = 0;
        while (g0 < T*3) {
            int g1 = g0 + 1;
            while (g1 < T*3 && ek[g1].a == ek[g0].a && ek[g1].b == ek[g0].b) g1++;
            for (int i = g0; i < g1; i++) {
                for (int j = g0; j < g1; j++) {
                    if (i == j || ek[i].tri == ek[j].tri) continue;
                    NavEdge e;
                    if (!TriPairEdge(sim, lk, map->lineCount, &ek[i], &ek[j], &e)) continue;
                    if (pass == 0) sim->tEdgeStart[ek[i].tri + 1]++;
                    else sim->tEdges[fill[ek[i].tri]++] = e;
                }
            }
            g0 = g1;
        }
        if (pass == 1) free(fill);
    }
    free(ek);
    free(lk);
    return true;
}

// ---------------------------------------------------------------- blockmap

static bool BuildBlockmap(Sim *sim)
{
    const DoomMap *map = sim->map;
    sim->bmOrgX = map->minX - 8.0f;
    sim->bmOrgY = map->minY - 8.0f;
    sim->bmW = (int)((map->maxX - sim->bmOrgX) / BM_CELL) + 1;
    sim->bmH = (int)((map->maxY - sim->bmOrgY) / BM_CELL) + 1;
    int cells = sim->bmW * sim->bmH;
    sim->bmStart = (int *)calloc((size_t)cells + 1, sizeof(int));
    sim->lineStamp = (int *)calloc((size_t)map->lineCount, sizeof(int));
    if (!sim->bmStart || !sim->lineStamp) return false;

    for (int pass = 0; pass < 2; pass++) {
        int *fill = NULL;
        if (pass == 1) {
            for (int c = 0; c < cells; c++) sim->bmStart[c + 1] += sim->bmStart[c];
            int total = sim->bmStart[cells];
            sim->bmLines = (int *)malloc((size_t)(total > 0 ? total : 1) * sizeof(int));
            fill = (int *)malloc((size_t)cells * sizeof(int));
            if (!sim->bmLines || !fill) { free(fill); return false; }
            memcpy(fill, sim->bmStart, (size_t)cells * sizeof(int));
        }
        for (int i = 0; i < map->lineCount; i++) {
            const Linedef *l = &map->lines[i];
            Vector2 a = map->verts[l->v1], b = map->verts[l->v2];
            int x0 = (int)((fminf(a.x, b.x) - sim->bmOrgX) / BM_CELL);
            int x1 = (int)((fmaxf(a.x, b.x) - sim->bmOrgX) / BM_CELL);
            int y0 = (int)((fminf(a.y, b.y) - sim->bmOrgY) / BM_CELL);
            int y1 = (int)((fmaxf(a.y, b.y) - sim->bmOrgY) / BM_CELL);
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 >= sim->bmW) x1 = sim->bmW - 1;
            if (y1 >= sim->bmH) y1 = sim->bmH - 1;
            for (int cy = y0; cy <= y1; cy++) {
                for (int cx = x0; cx <= x1; cx++) {
                    int c = cy*sim->bmW + cx;
                    if (pass == 0) sim->bmStart[c + 1]++;
                    else sim->bmLines[fill[c]++] = i;
                }
            }
        }
        if (pass == 1) free(fill);
    }
    return true;
}

// Live blocking test for movement (uses CURRENT door ceilings). Flyers
// ignore floor steps — they rise over ledges; closed doors still stop them.
static bool LineBlocksEnt(const Sim *sim, const Linedef *l, float z, bool flyer)
{
    if (l->sideFront < 0 || l->sideBack < 0) return true;
    if (l->flags & ML_BLOCKING) return true;
    int fs = sim->map->sides[l->sideFront].sector;
    int bs = sim->map->sides[l->sideBack].sector;
    if (fs < 0 || bs < 0) return true;
    const Sector *a = &sim->map->sectors[fs];
    const Sector *b = &sim->map->sectors[bs];
    int dfa = sim->sectorDoor[fs], dfb = sim->sectorDoor[bs];
    float ceilA = (dfa >= 0) ? sim->doors[dfa].ceil : a->ceilH;
    float ceilB = (dfb >= 0) ? sim->doors[dfb].ceil : b->ceilH;
    float top = fminf(ceilA, ceilB);
    float bottom = fmaxf(SimSectorFloorNow(sim, fs), SimSectorFloorNow(sim, bs));
    if (top - bottom < MOB_HEIGHT) return true;
    if (!flyer && bottom - z > DG_STEP) return true;
    return false;
}

// Shared circle mover for Doom Guy and monsters: wall push-out (which
// preserves the tangential component, so entities slide along walls) plus
// body blocking against other live entities. Returns distance moved.
static float MoveEntity(Sim *sim, Vector2 *pos, float z, float radius,
                        Vector2 vel, bool flyer, int selfMob)
{
    Vector2 start = *pos;
    Vector2 p = { pos->x + vel.x, pos->y + vel.y };
    for (int it = 0; it < 2; it++) {
        sim->stamp++;
        int x0 = (int)((p.x - radius - 1.0f - sim->bmOrgX) / BM_CELL);
        int x1 = (int)((p.x + radius + 1.0f - sim->bmOrgX) / BM_CELL);
        int y0 = (int)((p.y - radius - 1.0f - sim->bmOrgY) / BM_CELL);
        int y1 = (int)((p.y + radius + 1.0f - sim->bmOrgY) / BM_CELL);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 >= sim->bmW) x1 = sim->bmW - 1;
        if (y1 >= sim->bmH) y1 = sim->bmH - 1;
        for (int cy = y0; cy <= y1; cy++) {
            for (int cx = x0; cx <= x1; cx++) {
                int c = cy*sim->bmW + cx;
                for (int k = sim->bmStart[c]; k < sim->bmStart[c + 1]; k++) {
                    int li = sim->bmLines[k];
                    if (sim->lineStamp[li] == sim->stamp) continue;
                    sim->lineStamp[li] = sim->stamp;
                    const Linedef *l = &sim->map->lines[li];
                    if (!LineBlocksEnt(sim, l, z, flyer)) continue;
                    Vector2 q = SegClosest(sim->map->verts[l->v1],
                                           sim->map->verts[l->v2], p);
                    float dx = p.x - q.x, dy = p.y - q.y;
                    float d2 = dx*dx + dy*dy;
                    if (d2 >= radius*radius || d2 <= 0.000001f) continue;
                    float d = sqrtf(d2);
                    float push = (radius - d) / d;
                    p.x += dx*push;
                    p.y += dy*push;
                }
            }
        }
        for (int m = 0; m < sim->mobCount; m++) {
            if (m == selfMob || sim->mobs[m].state == MOB_DEAD) continue;
            float r = radius + MOB_INFO[sim->mobs[m].kind].radius;
            float dx = p.x - sim->mobs[m].pos.x, dy = p.y - sim->mobs[m].pos.y;
            float d2 = dx*dx + dy*dy;
            if (d2 >= r*r || d2 <= 0.000001f) continue;
            float d = sqrtf(d2);
            p.x += dx*((r - d)/d);
            p.y += dy*((r - d)/d);
        }
        if (selfMob >= 0 && sim->dg.state != DG_DEAD && sim->dg.state != DG_EXITED) {
            float r = radius + DG_RADIUS;
            float dx = p.x - sim->dg.pos.x, dy = p.y - sim->dg.pos.y;
            float d2 = dx*dx + dy*dy;
            if (d2 < r*r && d2 > 0.000001f) {
                float d = sqrtf(d2);
                p.x += dx*((r - d)/d);
                p.y += dy*((r - d)/d);
            }
        }
    }
    *pos = p;
    return Dist2D(start, p);
}

// ---------------------------------------------------------------- doom guy

const char *DGWeaponName(DGWeapon w)
{
    switch (w) {
    case WPN_PISTOL:   return "pistol";
    case WPN_SHOTGUN:  return "shotgun";
    case WPN_CHAINGUN: return "chaingun";
    case WPN_ROCKET:   return "rockets";
    }
    return "?";
}

const char *DGStateName(DGState s)
{
    switch (s) {
    case DG_ADVANCE:   return "advance";
    case DG_WAIT_DOOR: return "wait_door";
    case DG_WAIT_LIFT: return "wait_lift";
    case DG_FIGHT:     return "fight";
    case DG_RETREAT:   return "retreat";
    case DG_EXITED:    return "exited";
    case DG_DEAD:      return "dead";
    case DG_NO_GOAL:   return "no_goal";
    }
    return "?";
}

static void DgSetState(Sim *sim, DGState st, const char *reason)
{
    if (sim->dg.state == st) return;
    if (StateLogEnabled()) {
        StateLogBegin("doomguy_state");
        StateLogInt("tic", sim->tic);
        StateLogStr("from", DGStateName(sim->dg.state));
        StateLogStr("to", DGStateName(st));
        StateLogStr("reason", reason);
        StateLogEnd();
    }
    sim->dg.state = st;
}

// Conservative "can he walk straight from a to b" test for route pruning.
// Door-adjacent lines always block so door waypoints keep their triggers.
static bool PruneLineBlocks(const Sim *sim, const Linedef *l)
{
    if (l->sideFront < 0 || l->sideBack < 0) return true;
    if (l->flags & ML_BLOCKING) return true;
    int fs = sim->map->sides[l->sideFront].sector;
    int bs = sim->map->sides[l->sideBack].sector;
    if (fs < 0 || bs < 0) return true;
    if (sim->sectorDoor[fs] >= 0 || sim->sectorDoor[bs] >= 0) return true;
    if (sim->sectorLift[fs] >= 0 || sim->sectorLift[bs] >= 0) return true;
    const Sector *a = &sim->map->sectors[fs];
    const Sector *b = &sim->map->sectors[bs];
    if (fabsf(a->floorH - b->floorH) > DG_STEP) return true;
    float top = fminf(a->ceilH, b->ceilH);
    float bottom = fmaxf(a->floorH, b->floorH);
    if (top - bottom < DG_HEIGHT) return true;
    return false;
}

static float Cross2(Vector2 o, Vector2 a, Vector2 b)
{
    return (a.x - o.x)*(b.y - o.y) - (a.y - o.y)*(b.x - o.x);
}

// Proper crossings only; endpoint touches don't count, so a segment may END
// exactly on a portal midpoint without being "blocked" by that portal.
static bool SegsCross(Vector2 a, Vector2 b, Vector2 c, Vector2 d)
{
    float d1 = Cross2(c, d, a), d2 = Cross2(c, d, b);
    float d3 = Cross2(a, b, c), d4 = Cross2(a, b, d);
    return ((d1 > 0.0f && d2 < 0.0f) || (d1 < 0.0f && d2 > 0.0f)) &&
           ((d3 > 0.0f && d4 < 0.0f) || (d3 < 0.0f && d4 > 0.0f));
}

static bool SegClear(Sim *sim, Vector2 a, Vector2 b)
{
    sim->stamp++;
    float pad = DG_RADIUS + 1.0f;
    int x0 = (int)((fminf(a.x, b.x) - pad - sim->bmOrgX) / BM_CELL);
    int x1 = (int)((fmaxf(a.x, b.x) + pad - sim->bmOrgX) / BM_CELL);
    int y0 = (int)((fminf(a.y, b.y) - pad - sim->bmOrgY) / BM_CELL);
    int y1 = (int)((fmaxf(a.y, b.y) + pad - sim->bmOrgY) / BM_CELL);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= sim->bmW) x1 = sim->bmW - 1;
    if (y1 >= sim->bmH) y1 = sim->bmH - 1;
    for (int cy = y0; cy <= y1; cy++) {
        for (int cx = x0; cx <= x1; cx++) {
            int c = cy*sim->bmW + cx;
            for (int k = sim->bmStart[c]; k < sim->bmStart[c + 1]; k++) {
                int li = sim->bmLines[k];
                if (sim->lineStamp[li] == sim->stamp) continue;
                sim->lineStamp[li] = sim->stamp;
                const Linedef *l = &sim->map->lines[li];
                if (!PruneLineBlocks(sim, l)) continue;
                if (SegsCross(a, b, sim->map->verts[l->v1], sim->map->verts[l->v2]))
                    return false;
            }
        }
    }
    return true;
}

// Greedy line-of-sight shortcutting over the waypoint list; never skips
// past a door waypoint (its position is the door-use trigger).
static void PruneRoute(Sim *sim)
{
    DoomGuy *dg = &sim->dg;
    if (dg->wpCount <= 1) return;
    Vector2 cur = dg->pos;
    int out = 0, i = 0;
    while (i < dg->wpCount) {
        int far = i;
        int limit = i + 24;
        for (int j = i; j < dg->wpCount && j <= limit; j++) {
            if (SegClear(sim, cur, dg->wp[j])) far = j;
            if (dg->wpDoor[j] >= 0 || dg->wpLift[j] >= 0) break;
        }
        dg->wp[out] = dg->wp[far];
        dg->wpFloor[out] = dg->wpFloor[far];
        dg->wpDoor[out] = dg->wpDoor[far];
        dg->wpLift[out] = dg->wpLift[far];
        dg->wpEdge[out] = dg->wpEdge[far];
        cur = dg->wp[out];
        out++;
        i = far + 1;
    }
    dg->wpCount = out;
    if (dg->wpCursor > dg->wpCount) dg->wpCursor = dg->wpCount;
}

// ---------------------------------------------------------------- sight

// Vertical opening a sight line must thread at this linedef (door panels
// use their CURRENT ceiling). False = one-sided/solid: always blocks.
static bool LineSightOpening(const Sim *sim, const Linedef *l,
                             float *bottom, float *top)
{
    if (l->sideFront < 0 || l->sideBack < 0) return false;
    int fs = sim->map->sides[l->sideFront].sector;
    int bs = sim->map->sides[l->sideBack].sector;
    if (fs < 0 || bs < 0) return false;
    int dfa = sim->sectorDoor[fs], dfb = sim->sectorDoor[bs];
    float ceilA = (dfa >= 0) ? sim->doors[dfa].ceil : sim->map->sectors[fs].ceilH;
    float ceilB = (dfb >= 0) ? sim->doors[dfb].ceil : sim->map->sectors[bs].ceilH;
    *top = fminf(ceilA, ceilB);
    *bottom = fmaxf(SimSectorFloorNow(sim, fs), SimSectorFloorNow(sim, bs));
    return (*top - *bottom) >= 8.0f;
}

static float SegSegT(Vector2 a, Vector2 b, Vector2 c, Vector2 d)
{
    float rx = b.x - a.x, ry = b.y - a.y;
    float sx = d.x - c.x, sy = d.y - c.y;
    float denom = rx*sy - ry*sx;
    if (fabsf(denom) < 0.000001f) return 2.0f;
    float t = ((c.x - a.x)*sy - (c.y - a.y)*sx) / denom;
    float u = ((c.x - a.x)*ry - (c.y - a.y)*rx) / denom;
    if (t < 0.0f || t > 1.0f || u < 0.0f || u > 1.0f) return 2.0f;
    return t;
}

// Earliest wall hit along the 3D ray (a, za) -> (b, zb) as a fraction of
// the segment; 1.0 means clear. At every crossed two-sided line the ray's
// interpolated height must thread the vertical opening — ledge steps and
// half-closed doors block exactly like real walls. Used for both vision
// and hitscan wall clipping.
static float SightT(Sim *sim, Vector2 a, float za, Vector2 b, float zb)
{
    float best = 1.0f;
    sim->stamp++;
    int x0 = (int)((fminf(a.x, b.x) - 1.0f - sim->bmOrgX) / BM_CELL);
    int x1 = (int)((fmaxf(a.x, b.x) + 1.0f - sim->bmOrgX) / BM_CELL);
    int y0 = (int)((fminf(a.y, b.y) - 1.0f - sim->bmOrgY) / BM_CELL);
    int y1 = (int)((fmaxf(a.y, b.y) + 1.0f - sim->bmOrgY) / BM_CELL);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= sim->bmW) x1 = sim->bmW - 1;
    if (y1 >= sim->bmH) y1 = sim->bmH - 1;
    for (int cy = y0; cy <= y1; cy++) {
        for (int cx = x0; cx <= x1; cx++) {
            int c = cy*sim->bmW + cx;
            for (int k = sim->bmStart[c]; k < sim->bmStart[c + 1]; k++) {
                int li = sim->bmLines[k];
                if (sim->lineStamp[li] == sim->stamp) continue;
                sim->lineStamp[li] = sim->stamp;
                const Linedef *l = &sim->map->lines[li];
                float t = SegSegT(a, b, sim->map->verts[l->v1], sim->map->verts[l->v2]);
                if (t >= best) continue;
                float bottom, top;
                if (LineSightOpening(sim, l, &bottom, &top)) {
                    float z = za + (zb - za)*t;
                    if (z > bottom + 1.0f && z < top - 1.0f) continue;
                }
                best = t;
            }
        }
    }
    return best;
}

static bool SightClearZ(Sim *sim, Vector2 a, float za, Vector2 b, float zb)
{
    return SightT(sim, a, za, b, zb) >= 1.0f;
}

// ---------------------------------------------------------------- combat

static void PushTracer(Sim *sim, Vector2 a, float za, Vector2 b, float zb, bool fromDG)
{
    Tracer *t = &sim->tracers[sim->tracerHead];
    sim->tracerHead = (sim->tracerHead + 1) % TRACERS_MAX;
    t->ttl = 6;
    t->a = (Vector3){ a.x, za, -a.y };
    t->b = (Vector3){ b.x, zb, -b.y };
    t->fromDG = fromDG;
}

// Smallest t in [0,1] where segment a->b enters the circle at c, or 2.
static float SegCircleT(Vector2 a, Vector2 b, Vector2 c, float r)
{
    float dx = b.x - a.x, dy = b.y - a.y;
    float fx = a.x - c.x, fy = a.y - c.y;
    float A = dx*dx + dy*dy;
    if (A < 0.000001f) return 2.0f;
    float B = 2.0f*(fx*dx + fy*dy);
    float C = fx*fx + fy*fy - r*r;
    float disc = B*B - 4.0f*A*C;
    if (disc < 0.0f) return 2.0f;
    float s = sqrtf(disc);
    float t = (-B - s) / (2.0f*A);
    if (t < 0.0f) t = (-B + s) / (2.0f*A);
    return (t >= 0.0f && t <= 1.0f) ? t : 2.0f;
}

static void SpawnPickup(Sim *sim, PickupType type, Vector2 pos, float z, bool dropped)
{
    for (int i = 0; i < PICKUPS_MAX; i++) {
        Pickup *p = &sim->pickups[i];
        if (p->alive) continue;
        *p = (Pickup){ .alive = true, .dropped = dropped, .type = type,
                       .pos = pos, .z = z };
        if (i >= sim->pickupCount) sim->pickupCount = i + 1;
        return;
    }
}

static bool SightClearZ(Sim *sim, Vector2 a, float za, Vector2 b, float zb);

// Gunfire alerts monsters that can see the muzzle, or are close enough to
// hear it through a wall. Without the sight gate, one shot rallies the whole
// map — and pre-placed ambushes behind walls would never stay asleep.
static void WakeNearby(Sim *sim, Vector2 pos, float z, float radius)
{
    for (int i = 0; i < sim->mobCount; i++) {
        Mob *m = &sim->mobs[i];
        if (m->state == MOB_DEAD || m->awake) continue;
        float d = Dist2D(m->pos, pos);
        if (d > radius) continue;
        if (d > 250.0f && !SightClearZ(sim, m->pos, m->z + 32.0f, pos, z)) continue;
        m->awake = true;
        if (m->state == MOB_IDLE) m->state = MOB_CHASE;
    }
}

static void LogMobDeath(const Sim *sim, const Mob *m, int attackerMob)
{
    if (!StateLogEnabled()) return;
    StateLogBegin("mob_death");
    StateLogInt("tic", sim->tic);
    StateLogStr("kind", MobKindName(m->kind));
    StateLogStr("by", (attackerMob < 0) ? "doomguy"
                                        : MobKindName(sim->mobs[attackerMob].kind));
    StateLogBool("infight", attackerMob >= 0);
    StateLogBool("player_dropped", m->playerDropped);
    StateLogBool("gibbed", m->gibbed);
    StateLogFloat("x", m->pos.x);
    StateLogFloat("y", m->pos.y);
    StateLogEnd();
}

// Keep the bank bounded: beyond CORPSES_MAX raisable bodies, the oldest
// decay into gibs the arch-vile can't use.
static void DecayCorpses(Sim *sim)
{
    for (;;) {
        int raisable = 0, oldest = -1;
        long oldestTic = 0;
        for (int i = 0; i < sim->mobCount; i++) {
            Mob *m = &sim->mobs[i];
            if (m->state != MOB_DEAD || m->gibbed) continue;
            raisable++;
            if (oldest < 0 || m->deathTic < oldestTic) {
                oldest = i;
                oldestTic = m->deathTic;
            }
        }
        if (raisable <= CORPSES_MAX || oldest < 0) return;
        sim->mobs[oldest].gibbed = true;
    }
}

static void DamageMob(Sim *sim, int idx, int dmg, int attackerMob)
{
    Mob *m = &sim->mobs[idx];
    if (m->state == MOB_DEAD) return;
    m->health -= dmg;
    m->awake = true;
    if (m->state == MOB_IDLE) m->state = MOB_CHASE;

    // Doom's infighting rule: hurt by another monster, turn on it.
    // Same-species hits stay loyal (the projectile exemption, simplified).
    // attackerMob: >=0 a monster, -1 Doom Guy, -2 the environment/player.
    if (attackerMob >= 0 && attackerMob != idx &&
        sim->mobs[attackerMob].kind != m->kind &&
        sim->mobs[attackerMob].state != MOB_DEAD) {
        if (m->targetMob != attackerMob) {
            m->targetMob = attackerMob;
            if (StateLogEnabled()) {
                StateLogBegin("infight");
                StateLogInt("tic", sim->tic);
                StateLogStr("victim", MobKindName(m->kind));
                StateLogStr("attacker", MobKindName(sim->mobs[attackerMob].kind));
                StateLogEnd();
            }
        }
    } else if (attackerMob == -1) {
        m->targetMob = -1;   // retaliate at Doom Guy
    }

    if (m->health <= 0) {
        m->state = MOB_DEAD;
        m->stateTics = 0;
        m->deathTic = sim->tic;
        // Vanilla gib rule: overkill past -spawnhealth destroys the corpse —
        // a gibbed body is withdrawn from the arch-vile's bank for good.
        m->gibbed = (m->health <= -MOB_INFO[m->kind].health);
        Snd(sim, MOB_SND_DEATH[m->kind], m->pos);
        if (attackerMob == -1) sim->kills++;   // his kills, not the player's
        LogMobDeath(sim, m, attackerMob);
        DecayCorpses(sim);
        int drop = MOB_INFO[m->kind].drop;
        if (drop >= 0) SpawnPickup(sim, (PickupType)drop, m->pos, m->z, true);
        return;
    }
    if (SimRandom(sim) < MOB_INFO[m->kind].painChance) {
        m->state = MOB_PAIN;
        m->stateTics = 8;
        Snd(sim, MOB_SND_PAIN[m->kind], m->pos);
    }
}

static void DamageDG(Sim *sim, int dmg, int attackerMob)
{
    DoomGuy *dg = &sim->dg;
    if (dg->state == DG_DEAD || dg->state == DG_EXITED) return;
    // The Pain Tithe: every point of damage dealt to him is a soul earned.
    // Healing he picks up is never refunded — a medikit is a pure loss.
    sim->souls += (float)dmg;
    if (sim->souls > SOULS_CAP) sim->souls = SOULS_CAP;
    int saved = 0;
    if (dg->armor > 0) {
        saved = dmg / 3;
        if (saved > dg->armor) saved = dg->armor;
        dg->armor -= saved;
    }
    dg->health -= dmg - saved;
    sim->dgDamageTaken += dmg - saved;
    if (dmg - saved >= 10 && dg->health > 0) Snd(sim, SND_PLPAIN, dg->pos);
    if (StateLogEnabled()) {
        StateLogBegin("dg_damage");
        StateLogInt("tic", sim->tic);
        StateLogInt("dmg", dmg - saved);
        StateLogStr("by", (attackerMob >= 0) ? MobKindName(sim->mobs[attackerMob].kind)
                                             : "world");
        StateLogInt("health", dg->health);
        StateLogInt("armor", dg->armor);
        StateLogEnd();
    }
    if (dg->health <= 0) {
        dg->health = 0;
        dg->deathTic = sim->tic;
        Snd(sim, SND_PLDETH, dg->pos);
        DgSetState(sim, DG_DEAD, "killed");
        if (StateLogEnabled()) {
            StateLogBegin("dg_death");
            StateLogInt("tic", sim->tic);
            StateLogFloat("seconds", (double)sim->tic / SIM_TICRATE);
            StateLogInt("kills", (int)sim->kills);
            StateLogEnd();
        }
        TraceLog(LOG_INFO, "SIM: Doom Guy died at tic %ld (%.1fs). You win.",
                 sim->tic, (double)sim->tic / SIM_TICRATE);
    }
}

// One hitscan pellet from a shooter (shooterMob -1 = Doom Guy). Walls clip
// the ray in 3D (the shot rises/falls toward the aim height); the first
// body on the line eats it — including other monsters, which is exactly
// how infights start.
static void FireHitscan(Sim *sim, int shooterMob, Vector2 from, float zFrom,
                        Vector2 at, float zAt, float spreadDeg, int dmg)
{
    float aim = atan2f(at.y - from.y, at.x - from.x);
    float spread = ((float)(SimRandom(sim) - SimRandom(sim)) / 255.0f) * spreadDeg * DEG2RAD;
    float a = aim + spread;
    Vector2 end = { from.x + cosf(a)*HITSCAN_RANGE, from.y + sinf(a)*HITSCAN_RANGE };
    float targetDist = Dist2D(from, at);
    float zEnd = (targetDist > 1.0f)
                     ? zFrom + (zAt - zFrom)*(HITSCAN_RANGE/targetDist)
                     : zFrom;
    float tWall = SightT(sim, from, zFrom, end, zEnd);

    int hitMob = -1;
    bool hitDG = false;
    float tBest = tWall;
    for (int i = 0; i < sim->mobCount; i++) {
        Mob *m = &sim->mobs[i];
        if (i == shooterMob || m->state == MOB_DEAD) continue;
        float t = SegCircleT(from, end, m->pos, MOB_INFO[m->kind].radius);
        if (t > 0.001f && t < tBest) { tBest = t; hitMob = i; hitDG = false; }
    }
    if (shooterMob >= 0 && sim->dg.state != DG_DEAD && sim->dg.state != DG_EXITED) {
        float t = SegCircleT(from, end, sim->dg.pos, DG_RADIUS);
        if (t > 0.001f && t < tBest) { tBest = t; hitMob = -1; hitDG = true; }
    }
    int hitBarrel = -1;
    for (int i = 0; i < sim->barrelCount; i++) {
        if (!sim->barrels[i].alive) continue;
        float t = SegCircleT(from, end, sim->barrels[i].pos, 12.0f);
        if (t > 0.001f && t < tBest) { tBest = t; hitBarrel = i; hitMob = -1; hitDG = false; }
    }

    Vector2 hit = { from.x + (end.x - from.x)*tBest, from.y + (end.y - from.y)*tBest };
    float zTo = zFrom;
    if (hitDG) zTo = sim->dg.z + DG_EYE*0.7f;
    else if (hitMob >= 0) zTo = sim->mobs[hitMob].z + 24.0f;
    PushTracer(sim, from, zFrom, hit, zTo, shooterMob < 0);

    if (hitDG) DamageDG(sim, dmg, shooterMob);
    else if (hitMob >= 0) DamageMob(sim, hitMob, dmg, shooterMob);
    else if (hitBarrel >= 0) DamageBarrel(sim, hitBarrel, dmg, shooterMob);
}

static void SpawnProjectile(Sim *sim, int ownerMob, Vector2 from, float zFrom,
                            Vector2 at, float zAt, float speed, int damageMult,
                            bool isRocket, bool homing)
{
    for (int i = 0; i < PROJS_MAX; i++) {
        Proj *p = &sim->projs[i];
        if (p->alive) continue;
        float dist = Dist2D(from, at);
        if (dist < 1.0f) dist = 1.0f;
        Vector2 dir = { (at.x - from.x)/dist, (at.y - from.y)/dist };
        p->alive = true;
        p->isRocket = isRocket;
        p->homing = homing;
        p->pos = p->prevPos = (Vector2){ from.x + dir.x*20.0f, from.y + dir.y*20.0f };
        p->vel = (Vector2){ dir.x*speed, dir.y*speed };
        p->z = p->prevZ = zFrom;
        p->zVel = (zAt - zFrom) / (dist/speed);
        p->damageMult = damageMult;
        p->ownerMob = ownerMob;
        p->ownerKind = (ownerMob >= 0) ? sim->mobs[ownerMob].kind : MK_COUNT;
        return;
    }
}

static void PushBoom(Sim *sim, BoomKind kind, Vector2 pos, float z)
{
    sim->booms[sim->boomHead % BOOMS_MAX] =
        (Boom){ (unsigned char)kind, pos, z, sim->tic };
    sim->boomHead++;
}

// Vanilla P_RadiusAttack flavor: linear falloff from the blast center,
// hurts everything including Doom Guy himself — and gibs corpses, which
// is THE counterplay against arch-vile corpse banking.
static void RadiusBlast(Sim *sim, Vector2 pos, int maxDmg, int attackerMob)
{
    for (int i = 0; i < sim->mobCount; i++) {
        Mob *m = &sim->mobs[i];
        float d = Dist2D(pos, m->pos) - MOB_INFO[m->kind].radius;
        if (d < 0.0f) d = 0.0f;
        if (m->state == MOB_DEAD) {
            if (!m->gibbed && d < 100.0f) {
                m->gibbed = true;
                Snd(sim, SND_SLOP, m->pos);
                if (StateLogEnabled()) {
                    StateLogBegin("corpse_gibbed");
                    StateLogInt("tic", sim->tic);
                    StateLogStr("kind", MobKindName(m->kind));
                    StateLogFloat("x", m->pos.x);
                    StateLogFloat("y", m->pos.y);
                    StateLogEnd();
                }
            }
            continue;
        }
        if (d >= (float)maxDmg) continue;
        DamageMob(sim, i, maxDmg - (int)d, attackerMob);
    }
    if (sim->dg.state != DG_DEAD && sim->dg.state != DG_EXITED) {
        float d = Dist2D(pos, sim->dg.pos) - DG_RADIUS;
        if (d < 0.0f) d = 0.0f;
        if (d < (float)maxDmg) DamageDG(sim, maxDmg - (int)d, attackerMob);
    }
    for (int i = 0; i < sim->barrelCount; i++) {
        if (!sim->barrels[i].alive) continue;
        float d = Dist2D(pos, sim->barrels[i].pos) - 12.0f;
        if (d < 0.0f) d = 0.0f;
        if (d < (float)maxDmg) DamageBarrel(sim, i, maxDmg - (int)d, attackerMob);
    }
}

// Barrels chain: the blast keeps the original attacker's attribution.
static void DamageBarrel(Sim *sim, int idx, int dmg, int attackerMob)
{
    Barrel *b = &sim->barrels[idx];
    if (!b->alive) return;
    b->health -= dmg;
    if (b->health > 0) return;
    b->alive = false;
    PushBoom(sim, BOOM_ROCKET, b->pos, b->z + 20.0f);
    Snd(sim, SND_BAREXP, b->pos);
    if (StateLogEnabled()) {
        StateLogBegin("barrel_explode");
        StateLogInt("tic", sim->tic);
        StateLogFloat("x", b->pos.x);
        StateLogFloat("y", b->pos.y);
        StateLogEnd();
    }
    RadiusBlast(sim, b->pos, 128, attackerMob);   // vanilla barrel blast
}

static void ProjTick(Sim *sim, Proj *p)
{
    p->prevPos = p->pos;
    p->prevZ = p->z;
    // Homing tracers bend toward him: clamped turn per tic, vanilla-feel.
    if (p->homing && sim->dg.state != DG_DEAD && sim->dg.state != DG_EXITED) {
        float speed = sqrtf(p->vel.x*p->vel.x + p->vel.y*p->vel.y);
        float cur = atan2f(p->vel.y, p->vel.x);
        float want = atan2f(sim->dg.pos.y - p->pos.y, sim->dg.pos.x - p->pos.x);
        float err = want - cur;
        while (err > PI) err -= 2.0f*PI;
        while (err < -PI) err += 2.0f*PI;
        float maxTurn = 4.0f*DEG2RAD;
        if (err > maxTurn) err = maxTurn;
        if (err < -maxTurn) err = -maxTurn;
        cur += err;
        p->vel = (Vector2){ cosf(cur)*speed, sinf(cur)*speed };
        float zWant = (sim->dg.z + 24.0f) - p->z;
        p->zVel = fmaxf(-2.0f, fminf(2.0f, zWant*0.1f));
    }
    Vector2 next = { p->pos.x + p->vel.x, p->pos.y + p->vel.y };
    float nextZ = p->z + p->zVel;

    float tWall = SightT(sim, p->pos, p->z, next, nextZ);
    float tBest = tWall;
    int hitMob = -1;
    bool hitDG = false;
    for (int i = 0; i < sim->mobCount; i++) {
        Mob *m = &sim->mobs[i];
        if (i == p->ownerMob || m->state == MOB_DEAD) continue;
        float t = SegCircleT(p->pos, next, m->pos, MOB_INFO[m->kind].radius + 6.0f);
        if (t < tBest) { tBest = t; hitMob = i; }
    }
    // His own rockets fly through him (vanilla); only the blast hurts him.
    if (!(p->isRocket && p->ownerMob < 0) &&
        sim->dg.state != DG_DEAD && sim->dg.state != DG_EXITED) {
        float t = SegCircleT(p->pos, next, sim->dg.pos, DG_RADIUS + 6.0f);
        if (t < tBest) { tBest = t; hitMob = -1; hitDG = true; }
    }

    int hitBarrel = -1;
    for (int i = 0; i < sim->barrelCount; i++) {
        if (!sim->barrels[i].alive) continue;
        float t = SegCircleT(p->pos, next, sim->barrels[i].pos, 16.0f);
        if (t < tBest) { tBest = t; hitBarrel = i; hitMob = -1; hitDG = false; }
    }

    bool exploded = (tBest < 1.0f);
    Vector2 hitPos = { p->pos.x + (next.x - p->pos.x)*tBest,
                       p->pos.y + (next.y - p->pos.y)*tBest };
    if (!exploded) {
        p->pos = next;
        p->z = nextZ;
        bool onMap = false;
        float floorH = SimFloorAtPoint(sim, p->pos.x, p->pos.y, &onMap);
        if (!onMap || p->z < floorH + 2.0f) {   // ground/void hit
            exploded = true;
            hitPos = p->pos;
        }
    }
    if (!exploded) return;

    int dmg = (SimRandom(sim)%8 + 1) * p->damageMult;
    if (hitDG) DamageDG(sim, dmg, p->ownerMob);
    else if (hitMob >= 0) DamageMob(sim, hitMob, dmg, p->ownerMob);
    else if (hitBarrel >= 0) DamageBarrel(sim, hitBarrel, dmg, p->ownerMob);
    if (p->isRocket) {
        PushBoom(sim, BOOM_ROCKET, hitPos, p->z);
        Snd(sim, SND_BAREXP, hitPos);
        RadiusBlast(sim, hitPos, 128, p->ownerMob);
    } else {
        BoomKind bk = (p->ownerKind == MK_CACO) ? BOOM_CACOBALL
                    : (p->ownerKind == MK_REVENANT) ? BOOM_TRACER
                    : (p->ownerKind == MK_BARON) ? BOOM_BARONBALL : BOOM_IMPBALL;
        PushBoom(sim, bk, hitPos, p->z);
        Snd(sim, SND_FIRXPL, hitPos);
    }
    p->alive = false;
}

// ---------------------------------------------------------------- pickups

static int NearestHealPickup(const Sim *sim, Vector2 from)
{
    int best = -1;
    float bestD = FLT_MAX;
    for (int i = 0; i < sim->pickupCount; i++) {
        const Pickup *p = &sim->pickups[i];
        if (!p->alive || (p->type != PK_STIM && p->type != PK_MEDIKIT)) continue;
        float d = Dist2D(from, p->pos);
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

static void TouchPickups(Sim *sim)
{
    DoomGuy *dg = &sim->dg;
    for (int i = 0; i < sim->pickupCount; i++) {
        Pickup *p = &sim->pickups[i];
        if (!p->alive) continue;
        if (Dist2D(dg->pos, p->pos) > 32.0f || fabsf(p->z - dg->z) > DG_HEIGHT) continue;
        bool took = true;
        const char *what = "?";
        switch (p->type) {
        case PK_STIM:
            if (dg->health >= 100) took = false;
            else { dg->health += 10; what = "stimpack"; }
            break;
        case PK_MEDIKIT:
            if (dg->health >= 100) took = false;
            else { dg->health += 25; what = "medikit"; }
            break;
        case PK_ARMOR:
            if (dg->armor >= 100) took = false;
            else { dg->armor = 100; what = "green_armor"; }
            break;
        case PK_SHOTGUN:
            dg->hasShotgun = true;
            dg->shells += p->dropped ? 4 : 8;
            what = "shotgun";
            break;
        case PK_CLIP:
            if (dg->bullets >= 200) took = false;
            else { dg->bullets += p->dropped ? 5 : 10; what = "clip"; }
            break;
        case PK_CLIPBOX:
            if (dg->bullets >= 200) took = false;
            else { dg->bullets += 50; what = "bullet_box"; }
            break;
        case PK_SHELLS:
            if (dg->shells >= 50) took = false;
            else { dg->shells += 4; what = "shells"; }
            break;
        case PK_SHELLBOX:
            if (dg->shells >= 50) took = false;
            else { dg->shells += 20; what = "shell_box"; }
            break;
        case PK_CHAINGUN:
            dg->hasChaingun = true;
            dg->bullets += p->dropped ? 10 : 20;
            what = "chaingun";
            break;
        case PK_RLAUNCH:
            dg->hasRocketLauncher = true;
            dg->rockets += 2;
            what = "rocket_launcher";
            break;
        case PK_ROCKET:
            if (dg->rockets >= 50) took = false;
            else { dg->rockets += 1; what = "rocket"; }
            break;
        case PK_ROCKETBOX:
            if (dg->rockets >= 50) took = false;
            else { dg->rockets += 5; what = "rocket_box"; }
            break;
        default:
            took = false;
            break;
        }
        if (!took) continue;
        if (dg->health > 100) dg->health = 100;
        if (dg->bullets > 200) dg->bullets = 200;
        if (dg->shells > 50) dg->shells = 50;
        if (dg->rockets > 50) dg->rockets = 50;
        p->alive = false;
        Snd(sim, (p->type == PK_SHOTGUN || p->type == PK_CHAINGUN ||
                  p->type == PK_RLAUNCH) ? SND_WPNUP : SND_ITEMUP, dg->pos);
        if (StateLogEnabled()) {
            StateLogBegin("pickup");
            StateLogInt("tic", sim->tic);
            StateLogStr("what", what);
            StateLogInt("health", dg->health);
            StateLogInt("armor", dg->armor);
            StateLogInt("bullets", dg->bullets);
            StateLogInt("shells", dg->shells);
            StateLogEnd();
        }
        if (i == dg->healPickup) {
            dg->healPickup = -1;
            if (dg->state == DG_RETREAT) {
                dg->objective = OBJ_EXIT;
                DgSetState(sim, DG_ADVANCE, "healed");
                PlanCurrentObjective(sim, "healed");
            }
        }
        if (i == dg->weaponPickup) {
            dg->weaponPickup = -1;
            if (dg->state == DG_ADVANCE) {
                PlanCurrentObjective(sim, "got_weapon");   // next rung or exit
            }
        }
    }
}

// ---------------------------------------------------------------- mob ai

static void MobFace(Mob *m, Vector2 to, float maxTurn)
{
    float want = atan2f(to.y - m->pos.y, to.x - m->pos.x) * RAD2DEG;
    float err = want - m->angleDeg;
    while (err > 180.0f) err -= 360.0f;
    while (err < -180.0f) err += 360.0f;
    if (err > maxTurn) err = maxTurn;
    if (err < -maxTurn) err = -maxTurn;
    m->angleDeg += err;
}

// Nearest raisable corpse within reach of the arch-vile, or -1. Viles
// cannot raise other viles (vanilla: no raise state).
static int FindCorpse(const Sim *sim, const Mob *vile)
{
    int best = -1;
    float bestD = VILE_RAISE_RADIUS;
    for (int i = 0; i < sim->mobCount; i++) {
        const Mob *c = &sim->mobs[i];
        if (c->state != MOB_DEAD || c->gibbed || c->kind == MK_ARCHVILE) continue;
        float d = Dist2D(vile->pos, c->pos);
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

static void Resurrect(Sim *sim, int vileIdx, int corpseIdx)
{
    Mob *c = &sim->mobs[corpseIdx];
    c->state = MOB_CHASE;
    c->health = MOB_INFO[c->kind].health/2;   // DESIGN.md: raised at half HP
    c->awake = true;
    c->targetMob = -1;
    c->deathTic = 0;
    c->prevPos = c->pos;
    c->prevZ = c->z;
    Snd(sim, SND_SLOP, c->pos);
    if (StateLogEnabled()) {
        StateLogBegin("resurrect");
        StateLogInt("tic", sim->tic);
        StateLogStr("kind", MobKindName(c->kind));
        StateLogInt("hp", c->health);
        StateLogFloat("x", c->pos.x);
        StateLogFloat("y", c->pos.y);
        StateLogInt("vile", vileIdx);
        StateLogEnd();
    }
}

static void MobAttack(Sim *sim, int idx, Vector2 tpos, float tz, bool targetIsDG, int targetMob)
{
    Mob *m = &sim->mobs[idx];
    const MobInfo *inf = &MOB_INFO[m->kind];
    float dist = Dist2D(m->pos, tpos);
    if (m->kind == MK_ARCHVILE) {
        // The flame lands only if he still has line of sight at the end of
        // the channel — breaking sight behind a wall dodges it, vanilla-style.
        if (SightClearZ(sim, m->pos, m->z + 32.0f, tpos, tz + 24.0f)) {
            if (targetIsDG) DamageDG(sim, VILE_FLAME_DMG, idx);
            else DamageMob(sim, targetMob, VILE_FLAME_DMG, idx);
            Snd(sim, SND_FIRXPL, tpos);
        }
        return;
    }
    if (inf->meleeRange > 0.0f && dist < inf->meleeRange + inf->radius) {
        Snd(sim, (m->kind == MK_PINKY) ? SND_SGTATK : SND_CLAW, m->pos);
        int dmg = (SimRandom(sim)%inf->meleeDie + 1) * inf->meleeMult;
        if (targetIsDG) DamageDG(sim, dmg, idx);
        else DamageMob(sim, targetMob, dmg, idx);
    } else if (inf->pellets > 0) {
        // Monster aim degrades with range, vanilla-feel: point blank ~25%
        // whiff, across a courtyard closer to half.
        int whiff = 55 + (int)(dist*0.07f);
        if (whiff > 130) whiff = 130;
        Snd(sim, (inf->pellets > 1) ? SND_SHOTGUN : SND_PISTOL, m->pos);
        for (int p = 0; p < inf->pellets; p++) {
            if (SimRandom(sim) < whiff) continue;
            int dmg = (SimRandom(sim)%5 + 1) * 3;   // vanilla zombie bullet
            FireHitscan(sim, idx, m->pos, m->z + 32.0f, tpos, tz + 24.0f, 13.0f, dmg);
        }
        WakeNearby(sim, m->pos, m->z + 32.0f, 500.0f);
    } else if (inf->hasMissile) {
        Snd(sim, (m->kind == MK_REVENANT) ? SND_SKEATK : SND_FIRSHT, m->pos);
        // Revenant tracers home in half the time, vanilla's coin flip.
        bool homing = (m->kind == MK_REVENANT) && (SimRandom(sim) & 1);
        SpawnProjectile(sim, idx, m->pos, m->z + 32.0f, tpos, tz + 24.0f,
                        inf->missileSpeed, inf->missileMult, false, homing);
    }
}

static void MobTick(Sim *sim, int idx)
{
    Mob *m = &sim->mobs[idx];
    m->prevPos = m->pos;
    m->prevZ = m->z;
    if (m->state == MOB_DEAD) return;

    const MobInfo *inf = &MOB_INFO[m->kind];
    bool dgGone = (sim->dg.state == DG_DEAD || sim->dg.state == DG_EXITED);

    // Resolve current target: another mob while infighting, else Doom Guy.
    if (m->targetMob >= 0 && sim->mobs[m->targetMob].state == MOB_DEAD)
        m->targetMob = -1;
    bool targetIsDG = (m->targetMob < 0);
    if (targetIsDG && dgGone) {
        m->state = MOB_IDLE;
        m->awake = false;
        return;
    }
    Vector2 tpos = targetIsDG ? sim->dg.pos : sim->mobs[m->targetMob].pos;
    float tz = targetIsDG ? sim->dg.z : sim->mobs[m->targetMob].z;

    switch (m->state) {
    case MOB_IDLE:
        if (((sim->tic + idx) & 7) == 0 && !dgGone &&
            Dist2D(m->pos, sim->dg.pos) < SIGHT_RANGE &&
            SightClearZ(sim, m->pos, m->z + 32.0f,
                        sim->dg.pos, sim->dg.z + 32.0f)) {
            m->awake = true;
            m->state = MOB_CHASE;
            Snd(sim, MOB_SND_SIGHT[m->kind], m->pos);
        }
        break;
    case MOB_PAIN:
        if (--m->stateTics <= 0) m->state = MOB_CHASE;
        break;
    case MOB_RAISE:
        m->stateTics--;
        if (m->stateTics == 4 && m->raiseTarget >= 0 &&
            sim->mobs[m->raiseTarget].state == MOB_DEAD &&
            !sim->mobs[m->raiseTarget].gibbed) {
            Resurrect(sim, idx, m->raiseTarget);
        }
        if (m->stateTics <= 0) {
            m->state = MOB_CHASE;
            m->raiseTarget = -1;
            m->raiseCooldown = VILE_RAISE_COOLDOWN;
        }
        break;
    case MOB_ATTACK:
        if (m->kind == MK_LOSTSOUL) {
            // The dive-bomb: locked heading, full speed, hit on contact.
            Vector2 cv = { cosf(m->dodgeDir)*SKULL_CHARGE_SPEED,
                           sinf(m->dodgeDir)*SKULL_CHARGE_SPEED };
            MoveEntity(sim, &m->pos, m->z, inf->radius, cv, true, idx);
            float contact = inf->radius +
                            (targetIsDG ? DG_RADIUS : MOB_INFO[sim->mobs[m->targetMob].kind].radius);
            if (Dist2D(m->pos, tpos) < contact + 6.0f) {
                int dmg = (SimRandom(sim)%8 + 1) * 3;   // vanilla skull ram
                if (targetIsDG) DamageDG(sim, dmg, idx);
                else DamageMob(sim, m->targetMob, dmg, idx);
                m->stateTics = 0;
            }
            m->stateTics--;
            if (m->stateTics <= 0) {
                m->state = MOB_CHASE;
                m->attackCooldown = 35 + SimRandom(sim)%50;
            }
            break;
        }
        MobFace(m, tpos, 45.0f);
        m->stateTics--;
        if (m->stateTics == 4) MobAttack(sim, idx, tpos, tz, targetIsDG, m->targetMob);
        if (m->stateTics <= 0) {
            m->state = MOB_CHASE;
            m->attackCooldown = (m->kind == MK_ARCHVILE ? 60 : 30) + SimRandom(sim)%50;
        }
        break;
    case MOB_CHASE: {
        if (m->attackCooldown > 0) m->attackCooldown--;
        if (m->raiseCooldown > 0) m->raiseCooldown--;
        // The Corpse Bank: a vile near a raisable body always banks it first.
        if (m->kind == MK_ARCHVILE && m->raiseCooldown <= 0) {
            int corpse = FindCorpse(sim, m);
            if (corpse >= 0) {
                m->state = MOB_RAISE;
                m->stateTics = VILE_RAISE_TICS;
                m->raiseTarget = corpse;
                break;
            }
        }
        float dist = Dist2D(m->pos, tpos);
        bool melee = (inf->meleeRange > 0.0f && dist < inf->meleeRange + inf->radius);
        bool ranged = (inf->pellets > 0 || inf->hasMissile ||
                       m->kind == MK_ARCHVILE || m->kind == MK_LOSTSOUL);
        float reach = (m->kind == MK_ARCHVILE) ? VILE_FLAME_RANGE
                    : (m->kind == MK_LOSTSOUL) ? 600.0f : 1100.0f;
        if (m->attackCooldown <= 0 && (melee ||
            (ranged && dist < reach &&
             SightClearZ(sim, m->pos, m->z + 32.0f, tpos, tz + 24.0f)))) {
            // Doom-ish gate: the closer the target, the likelier the attack.
            int gate = melee ? 256 : 70 + (int)fmaxf(0.0f, 110.0f - dist*0.09f);
            if (SimRandom(sim) < gate) {
                m->state = MOB_ATTACK;
                m->stateTics = (m->kind == MK_ARCHVILE) ? VILE_FLAME_TICS
                             : (m->kind == MK_LOSTSOUL) ? SKULL_CHARGE_TICS : 10;
                if (m->kind == MK_ARCHVILE) Snd(sim, SND_VILATK, m->pos);
                if (m->kind == MK_LOSTSOUL) {
                    m->dodgeDir = atan2f(tpos.y - m->pos.y, tpos.x - m->pos.x);
                    Snd(sim, SND_SKLATK, m->pos);
                }
                break;
            }
            m->attackCooldown = 6 + SimRandom(sim)%10;
        }
        Vector2 dir = { tpos.x - m->pos.x, tpos.y - m->pos.y };
        float len = sqrtf(dir.x*dir.x + dir.y*dir.y);
        if (len > 1.0f) { dir.x /= len; dir.y /= len; }
        if (m->dodgeTics > 0) {
            m->dodgeTics--;
            float c = cosf(m->dodgeDir), s = sinf(m->dodgeDir);
            dir = (Vector2){ dir.x*c - dir.y*s, dir.x*s + dir.y*c };
        }
        float moved = MoveEntity(sim, &m->pos, m->z, inf->radius,
                                 (Vector2){ dir.x*inf->speed, dir.y*inf->speed },
                                 inf->flyer, idx);
        if (moved < inf->speed*0.25f && m->dodgeTics <= 0) {
            // Blocked: wiggle off at an angle, Doom A_Chase style.
            m->dodgeTics = 12 + SimRandom(sim)%20;
            m->dodgeDir = (SimRandom(sim) & 1) ? 1.2f : -1.2f;
        }
        MobFace(m, (Vector2){ m->pos.x + dir.x*64.0f, m->pos.y + dir.y*64.0f }, 25.0f);
        break;
    }
    case MOB_DEAD:
        break;
    }

    // Ground entities snap to the floor; cacos float toward target height.
    bool onMap = false;
    float floorH = SimFloorAtPoint(sim, m->pos.x, m->pos.y, &onMap);
    if (inf->flyer) {
        float want = (m->state == MOB_CHASE || m->state == MOB_ATTACK)
                         ? tz + 24.0f : m->z;
        if (onMap && want < floorH + 24.0f) want = floorH + 24.0f;
        float dz = want - m->z;
        if (dz > 2.0f) dz = 2.0f;
        if (dz < -2.0f) dz = -2.0f;
        m->z += dz;
    } else if (onMap) {
        m->z = floorH;
    }
}

typedef struct ANode {
    float g;
    Vector2 entry;
    int parentNode, parentEdge;
    unsigned char state;   // 0 unseen, 1 open, 2 closed
} ANode;

// A* with entry-point relaxation over either nav graph (nodes are floor
// triangles or sectors). On success fills the doom-guy waypoint arrays
// (crossing midpoints plus the goal point) and returns true.
static bool RouteOnGraph(Sim *sim, const NavEdge *edges, const int *estart,
                         int nodeCount, int startNode, int goalNode,
                         Vector2 goalPos, int goalSector, bool triGraph)
{
    DoomGuy *dg = &sim->dg;
    const DoomMap *map = sim->map;

    if (startNode == goalNode) {
        dg->wp = (Vector2 *)malloc(sizeof(Vector2));
        dg->wpFloor = (float *)malloc(sizeof(float));
        dg->wpDoor = (int *)malloc(sizeof(int));
        dg->wpLift = (int *)malloc(sizeof(int));
        dg->wpEdge = (int *)malloc(sizeof(int));
        if (!dg->wp || !dg->wpFloor || !dg->wpDoor || !dg->wpLift || !dg->wpEdge)
            return false;
        dg->wp[0] = goalPos;
        dg->wpFloor[0] = map->sectors[goalSector].floorH;
        dg->wpDoor[0] = -1;
        dg->wpLift[0] = -1;
        dg->wpEdge[0] = -1;
        dg->wpCount = 1;
        return true;
    }

    ANode *n = (ANode *)calloc((size_t)nodeCount, sizeof(ANode));
    if (!n) return false;
    n[startNode] = (ANode){ 0.0f, dg->pos, -1, -1, 1 };
    bool found = false;
    for (;;) {
        int cur = -1;
        float bestF = FLT_MAX;
        for (int s = 0; s < nodeCount; s++) {
            if (n[s].state != 1) continue;
            float f = n[s].g + Dist2D(n[s].entry, goalPos);
            if (f < bestF) { bestF = f; cur = s; }
        }
        if (cur < 0) break;
        if (cur == goalNode) { found = true; break; }
        n[cur].state = 2;
        for (int e = estart[cur]; e < estart[cur + 1]; e++) {
            const NavEdge *E = &edges[e];
            if (triGraph && sim->tEdgeBlocked && sim->tEdgeBlocked[e]) continue;
            float ng = n[cur].g + Dist2D(n[cur].entry, E->wp)
                     + ((E->door >= 0) ? DOOR_COST : 0.0f);
            if (n[E->to].state == 0 || ng < n[E->to].g - 0.01f) {
                n[E->to].g = ng;
                n[E->to].entry = E->wp;
                n[E->to].parentNode = cur;
                n[E->to].parentEdge = e;
                n[E->to].state = 1;
            }
        }
    }
    if (!found) { free(n); return false; }

    int len = 0;
    for (int s = goalNode; s != startNode; s = n[s].parentNode) len++;
    dg->wp = (Vector2 *)malloc((size_t)(len + 1) * sizeof(Vector2));
    dg->wpFloor = (float *)malloc((size_t)(len + 1) * sizeof(float));
    dg->wpDoor = (int *)malloc((size_t)(len + 1) * sizeof(int));
    dg->wpLift = (int *)malloc((size_t)(len + 1) * sizeof(int));
    dg->wpEdge = (int *)malloc((size_t)(len + 1) * sizeof(int));
    if (!dg->wp || !dg->wpFloor || !dg->wpDoor || !dg->wpLift || !dg->wpEdge) {
        free(n);
        return false;
    }
    int k = len - 1;
    for (int s = goalNode; s != startNode; s = n[s].parentNode, k--) {
        const NavEdge *E = &edges[n[s].parentEdge];
        int destSector = triGraph ? sim->lm->floorTriSector[E->to] : E->to;
        dg->wp[k] = E->wp;
        dg->wpFloor[k] = map->sectors[destSector].floorH;
        dg->wpDoor[k] = E->door;
        dg->wpLift[k] = E->lift;
        dg->wpEdge[k] = triGraph ? n[s].parentEdge : -1;
    }
    dg->wp[len] = goalPos;
    dg->wpFloor[len] = map->sectors[goalSector].floorH;
    dg->wpDoor[len] = -1;
    dg->wpLift[len] = -1;
    dg->wpEdge[len] = -1;
    dg->wpCount = len + 1;
    free(n);
    return true;
}

// Existence-only A* over the triangle graph: is `to` reachable from `from`?
// Used to refuse pickup detours into one-way drops (pits whose way back
// out is a lift we don't simulate yet).
static bool RouteExists(Sim *sim, Vector2 from, Vector2 to)
{
    int startNode = TriAtPoint(sim, from.x, from.y);
    int goalNode = TriAtPoint(sim, to.x, to.y);
    if (startNode < 0 || goalNode < 0 || sim->triCount <= 0) return false;
    if (startNode == goalNode) return true;
    ANode *n = (ANode *)calloc((size_t)sim->triCount, sizeof(ANode));
    if (!n) return false;
    n[startNode] = (ANode){ 0.0f, from, -1, -1, 1 };
    bool found = false;
    for (;;) {
        int cur = -1;
        float bestF = FLT_MAX;
        for (int s = 0; s < sim->triCount; s++) {
            if (n[s].state != 1) continue;
            float f = n[s].g + Dist2D(n[s].entry, to);
            if (f < bestF) { bestF = f; cur = s; }
        }
        if (cur < 0) break;
        if (cur == goalNode) { found = true; break; }
        n[cur].state = 2;
        for (int e = sim->tEdgeStart[cur]; e < sim->tEdgeStart[cur + 1]; e++) {
            const NavEdge *E = &sim->tEdges[e];
            float ng = n[cur].g + Dist2D(n[cur].entry, E->wp);
            if (n[E->to].state == 0 || ng < n[E->to].g - 0.01f) {
                n[E->to].g = ng;
                n[E->to].entry = E->wp;
                n[E->to].parentNode = cur;
                n[E->to].parentEdge = e;
                n[E->to].state = 1;
            }
        }
    }
    free(n);
    return found;
}

static const char *ObjectiveName(DGObjective o)
{
    switch (o) {
    case OBJ_EXIT:   return "exit";
    case OBJ_HEAL:   return "heal";
    case OBJ_WEAPON: return "weapon";
    }
    return "?";
}

static bool PlanPathTo(Sim *sim, Vector2 goalPos, const char *reason)
{
    DoomGuy *dg = &sim->dg;
    free(dg->wp);
    free(dg->wpFloor);
    free(dg->wpDoor);
    free(dg->wpLift);
    free(dg->wpEdge);
    dg->wp = NULL;
    dg->wpFloor = NULL;
    dg->wpDoor = NULL;
    dg->wpLift = NULL;
    dg->wpEdge = NULL;
    dg->wpCount = 0;
    dg->wpCursor = 0;
    dg->waitDoor = -1;
    dg->waitLift = -1;
    dg->replans++;

    bool ok = false;
    const char *method = "none";
    int goalSector = SimSectorAtPoint(sim, goalPos.x, goalPos.y);
    if (goalSector >= 0) {
        int startTri = TriAtPoint(sim, dg->pos.x, dg->pos.y);
        int goalTri = TriAtPoint(sim, goalPos.x, goalPos.y);
        if (sim->triCount > 0 && startTri >= 0 && goalTri >= 0) {
            ok = RouteOnGraph(sim, sim->tEdges, sim->tEdgeStart, sim->triCount,
                              startTri, goalTri, goalPos, goalSector, true);
            if (ok) method = "tri";
        }
        if (!ok && dg->sector >= 0) {
            ok = RouteOnGraph(sim, sim->edges, sim->edgeStart, sim->map->sectorCount,
                              dg->sector, goalSector, goalPos, goalSector, false);
            if (ok) method = "sector";
        }
    }
    if (ok) PruneRoute(sim);

    if (!ok) {
        TraceLog(LOG_WARNING,
                 "SIM: plan failed: from (%.0f,%.0f) tri %d sector %d -> "
                 "goal (%.0f,%.0f) tri %d sector %d",
                 dg->pos.x, dg->pos.y, TriAtPoint(sim, dg->pos.x, dg->pos.y),
                 dg->sector, goalPos.x, goalPos.y,
                 TriAtPoint(sim, goalPos.x, goalPos.y), goalSector);
    }
    if (StateLogEnabled()) {
        StateLogBegin("doomguy_replan");
        StateLogInt("tic", sim->tic);
        StateLogStr("reason", reason);
        StateLogStr("objective", ObjectiveName(sim->dg.objective));
        StateLogBool("ok", ok);
        StateLogStr("method", method);
        StateLogInt("waypoints", dg->wpCount);
        StateLogEnd();
    }
    return ok;
}

// Nearest weapon pickup he doesn't own yet — the next rung of the ladder.
static int NearestWantedWeapon(const Sim *sim)
{
    const DoomGuy *dg = &sim->dg;
    int best = -1;
    float bestD = FLT_MAX;
    for (int i = 0; i < sim->pickupCount; i++) {
        const Pickup *p = &sim->pickups[i];
        if (!p->alive || p->unreachable) continue;
        bool wanted = (p->type == PK_SHOTGUN && !dg->hasShotgun) ||
                      (p->type == PK_CHAINGUN && !dg->hasChaingun) ||
                      (p->type == PK_RLAUNCH && !dg->hasRocketLauncher);
        if (!wanted) continue;
        float d = Dist2D(dg->pos, p->pos);
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

// Route to the current objective. Priority: an active heal run, then the
// arsenal ladder (he routes for un-owned weapons like a speedrunner — his
// power-ups ARE the difficulty curve), then the exit. Only a failed exit
// route is fatal (NO_GOAL).
static bool PlanCurrentObjective(Sim *sim, const char *reason)
{
    DoomGuy *dg = &sim->dg;
    if (dg->objective == OBJ_HEAL) {
        if (dg->healPickup >= 0 && sim->pickups[dg->healPickup].alive &&
            PlanPathTo(sim, sim->pickups[dg->healPickup].pos, reason)) {
            return true;
        }
        dg->objective = OBJ_EXIT;
        dg->healPickup = -1;
    }
    for (;;) {
        int w = NearestWantedWeapon(sim);
        if (w < 0) break;
        dg->objective = OBJ_WEAPON;
        // No detours into one-way drops: he must be able to leave with it.
        bool returnable = (sim->goalSector < 0) ||
                          RouteExists(sim, sim->pickups[w].pos, sim->goal);
        if (returnable && PlanPathTo(sim, sim->pickups[w].pos, reason)) {
            dg->weaponPickup = w;
            return true;
        }
        sim->pickups[w].unreachable = true;
    }
    dg->objective = OBJ_EXIT;
    dg->weaponPickup = -1;
    if (sim->goalSector >= 0 && PlanPathTo(sim, sim->goal, reason)) return true;
    DgSetState(sim, DG_NO_GOAL, reason);
    return false;
}

static void DgTriggerExit(Sim *sim)
{
    DgSetState(sim, DG_EXITED, "exit_switch");
    if (StateLogEnabled()) {
        StateLogBegin("doomguy_exit");
        StateLogInt("tic", sim->tic);
        StateLogFloat("seconds", (double)sim->tic / SIM_TICRATE);
        StateLogInt("line", sim->exitLine);
        StateLogInt("replans", sim->dg.replans);
        StateLogEnd();
    }
    TraceLog(LOG_INFO, "SIM: Doom Guy reached the exit at tic %ld (%.1fs). You lose.",
             sim->tic, (double)sim->tic / SIM_TICRATE);
}

static void DgFace(DoomGuy *dg, Vector2 to, float maxTurn)
{
    float want = atan2f(to.y - dg->pos.y, to.x - dg->pos.x) * RAD2DEG;
    float err = want - dg->angleDeg;
    while (err > 180.0f) err -= 360.0f;
    while (err < -180.0f) err += 360.0f;
    if (err > maxTurn) err = maxTurn;
    if (err < -maxTurn) err = -maxTurn;
    dg->angleDeg += err;
}

static float DgFacingErr(const DoomGuy *dg, Vector2 to)
{
    float want = atan2f(to.y - dg->pos.y, to.x - dg->pos.x) * RAD2DEG;
    float err = want - dg->angleDeg;
    while (err > 180.0f) err -= 360.0f;
    while (err < -180.0f) err += 360.0f;
    return err;
}

// The threat ledger (DESIGN.md): targeting reads compositions like a
// veteran — kill the vile first, ignore the distant pinky, respect the
// charging skull, and let infighting pairs grind each other down.
static float DgThreatScore(const Sim *sim, int i, float dist, const bool *hasProj)
{
    static const float CLASS_W[MK_COUNT] = {
        [MK_ZOMBIE] = 0.8f, [MK_SHOTGUNGUY] = 1.5f, [MK_IMP] = 1.0f,
        [MK_PINKY] = 1.0f, [MK_CACO] = 0.8f, [MK_LOSTSOUL] = 1.1f,
        [MK_REVENANT] = 1.7f, [MK_BARON] = 0.9f, [MK_ARCHVILE] = 4.0f,
    };
    const Mob *m = &sim->mobs[i];
    const MobInfo *inf = &MOB_INFO[m->kind];
    float w = CLASS_W[m->kind];
    // Melee-only monsters threaten by time-to-melee: a far pinky is a
    // doorstop, an adjacent one is a chainsaw accident.
    bool meleeOnly = (inf->pellets == 0 && !inf->hasMissile &&
                      m->kind != MK_ARCHVILE && m->kind != MK_LOSTSOUL);
    if (meleeOnly) {
        float f = 1.0f - (dist - 120.0f)/480.0f;
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        w *= f;
    }
    if (m->kind == MK_LOSTSOUL && m->state == MOB_ATTACK) w *= 3.0f;  // incoming!
    if (hasProj[i]) w *= 1.5f;              // its shot is already in the air
    if (m->targetMob >= 0) w *= 0.5f;       // infighting: let them work
    if (i == sim->dg.targetMob) w *= 1.3f;  // hysteresis: no target flicker
    return w / (1.0f + dist/700.0f);
}

// Anything scoring under this isn't worth stopping for — far stragglers
// get ignored while he keeps pathing.
#define ENGAGE_THRESHOLD 0.35f

static int DgPickTarget(Sim *sim)
{
    DoomGuy *dg = &sim->dg;
    bool hasProj[MOBS_MAX] = { false };
    for (int i = 0; i < PROJS_MAX; i++) {
        if (sim->projs[i].alive && sim->projs[i].ownerMob >= 0) {
            hasProj[sim->projs[i].ownerMob] = true;
        }
    }
    int best = -1;
    float bestScore = ENGAGE_THRESHOLD;
    for (int i = 0; i < sim->mobCount; i++) {
        Mob *m = &sim->mobs[i];
        if (m->state == MOB_DEAD) continue;
        float dist = Dist2D(dg->pos, m->pos);
        if (dist > SIGHT_RANGE) continue;
        float score = DgThreatScore(sim, i, dist, hasProj);
        if (score <= bestScore) continue;   // cheap reject before the raycast
        if (!SightClearZ(sim, dg->pos, dg->z + DG_EYE, m->pos, m->z + 24.0f)) continue;
        bestScore = score;
        best = i;
    }
    if (best >= 0 && best != dg->targetMob && StateLogEnabled()) {
        StateLogBegin("target_switch");
        StateLogInt("tic", sim->tic);
        StateLogStr("kind", MobKindName(sim->mobs[best].kind));
        StateLogFloat("score", bestScore);
        StateLogEnd();
    }
    return best;
}

// The arsenal ladder is the difficulty curve: each pickup changes which of
// your monsters still matter. Rocket discipline is deliberate counterplay —
// anything hugging him (pinkies!) switches his best weapon off.
static DGWeapon DgChooseWeapon(const DoomGuy *dg, const Mob *target, float dist)
{
    bool bigTarget = target && (MOB_INFO[target->kind].health >= 60);
    if (dg->hasRocketLauncher && dg->rockets > 0 && dist > 180.0f &&
        (bigTarget || dist > 500.0f)) {
        return WPN_ROCKET;
    }
    if (dg->hasShotgun && dg->shells > 0 && dist < 260.0f) return WPN_SHOTGUN;
    if (dg->hasChaingun && dg->bullets > 0) return WPN_CHAINGUN;
    if (dg->hasShotgun && dg->shells > 0) return WPN_SHOTGUN;
    return WPN_PISTOL;
}

static void DgFire(Sim *sim)
{
    DoomGuy *dg = &sim->dg;
    Mob *t = &sim->mobs[dg->targetMob];
    switch (dg->weapon) {
    case WPN_SHOTGUN:
        dg->shells--;
        Snd(sim, SND_SHOTGUN, dg->pos);
        for (int p = 0; p < 7; p++) {
            int dmg = (SimRandom(sim)%3 + 1) * 5;   // vanilla 5*(1d3) per pellet
            FireHitscan(sim, -1, dg->pos, dg->z + DG_EYE, t->pos, t->z + 24.0f,
                        6.0f, dmg);
        }
        dg->fireCooldown = 37;                       // vanilla shotgun cadence
        break;
    case WPN_CHAINGUN: {
        dg->bullets--;
        Snd(sim, SND_PISTOL, dg->pos);               // vanilla chaingun voice
        int dmg = (SimRandom(sim)%3 + 1) * 5;
        FireHitscan(sim, -1, dg->pos, dg->z + DG_EYE, t->pos, t->z + 24.0f,
                    4.0f, dmg);
        dg->fireCooldown = 4;                        // sustained fire
        break;
    }
    case WPN_ROCKET:
        dg->rockets--;
        Snd(sim, SND_RLAUNC, dg->pos);
        SpawnProjectile(sim, -1, dg->pos, dg->z + DG_EYE, t->pos, t->z + 24.0f,
                        20.0f, 20, true, false);     // vanilla speed and dice
        dg->fireCooldown = 30;
        break;
    default: {
        dg->bullets--;
        Snd(sim, SND_PISTOL, dg->pos);
        int dmg = (SimRandom(sim)%3 + 1) * 5;
        FireHitscan(sim, -1, dg->pos, dg->z + DG_EYE, t->pos, t->z + 24.0f,
                    2.5f, dmg);
        dg->fireCooldown = 10;                       // vanilla pistol cadence
        break;
    }
    }
    WakeNearby(sim, dg->pos, dg->z + DG_EYE, 800.0f);   // gunfire carries
}

static void DgUpdateGround(Sim *sim)
{
    DoomGuy *dg = &sim->dg;
    int sec = SimSectorAtPoint(sim, dg->pos.x, dg->pos.y);
    if (sec >= 0) {
        dg->sector = sec;
        dg->z = SimSectorFloorNow(sim, sec);
    }
}

// Shared ADVANCE/RETREAT locomotion along the planned waypoints. Handles
// door waits, arrival (objective-dependent), and stuck replanning.
static void DgFollowRoute(Sim *sim)
{
    DoomGuy *dg = &sim->dg;
    if (dg->wpCursor >= dg->wpCount) {
        if (!PlanCurrentObjective(sim, "route_exhausted")) return;
    }
    Vector2 target = dg->wp[dg->wpCursor];

    // Standing on a moving lift: ride it, don't grind against the shaft.
    int onLift = (dg->sector >= 0) ? sim->sectorLift[dg->sector] : -1;
    if (onLift >= 0 && (sim->lifts[onLift].state == LIFT_RISING ||
                        sim->lifts[onLift].state == LIFT_DOWN)) {
        dg->z = sim->lifts[onLift].floor;
        dg->stuckTics = 0;
        return;
    }

    int li = dg->wpLift[dg->wpCursor];
    if (li >= 0) {
        Lift *l = &sim->lifts[li];
        if (l->floor - dg->z > DG_STEP &&
            Dist2D(dg->pos, target) <= DG_USERANGE + 64.0f) {
            // The platform is above him: call it down, or wait while it moves.
            if (l->state == LIFT_UP) {
                if (LiftTriggerInRange(sim, li, dg->pos)) {
                    LiftActivate(sim, li);
                    dg->waitLift = li;
                    DgSetState(sim, DG_WAIT_LIFT, "lift_up");
                    return;
                }
                // else: keep walking; the trigger line sits closer in
            } else {
                dg->waitLift = li;
                DgSetState(sim, DG_WAIT_LIFT, "lift_moving");
                return;
            }
        }
    }

    int di = dg->wpDoor[dg->wpCursor];
    if (di >= 0) {
        Door *d = &sim->doors[di];
        float opening = d->ceil - sim->map->sectors[d->sector].floorH;
        if (opening < DG_HEIGHT && Dist2D(dg->pos, target) <= DG_USERANGE) {
            DoorUse(sim, di);
            dg->waitDoor = di;
            DgSetState(sim, DG_WAIT_DOOR, "door_in_way");
            return;
        }
    }

    DgFace(dg, target, TURN_PER_TIC);
    float dx = target.x - dg->pos.x, dy = target.y - dg->pos.y;
    float dist = sqrtf(dx*dx + dy*dy);
    Vector2 before = dg->pos;
    if (dist > 0.001f) {
        float step = (DG_RUNSPEED < dist) ? DG_RUNSPEED : dist;
        MoveEntity(sim, &dg->pos, dg->z, DG_RADIUS,
                   (Vector2){ dx/dist*step, dy/dist*step }, false, -1);
    }
    DgRunTriggers(sim, before, dg->pos);
    if (dg->state != DG_ADVANCE && dg->state != DG_RETREAT) return; // teleported/exited
    DgUpdateGround(sim);

    bool last = (dg->wpCursor == dg->wpCount - 1);
    // Pickup runs must end ON the item (touch radius 32), not merely near
    // it, or arrival flips the objective while it's still on the floor.
    float arrive = last ? ((dg->objective != OBJ_EXIT) ? 12.0f : DG_USERANGE*0.75f)
                        : WP_ARRIVE;
    if (Dist2D(dg->pos, target) < arrive) {
        dg->stuckStrikes = 0;
        if (last) {
            if (dg->objective == OBJ_EXIT) {
                if (sim->exitLine >= 0) DgTriggerExit(sim);
                else DgSetState(sim, DG_NO_GOAL, "no_exit_line");
            } else if (dg->objective == OBJ_WEAPON) {
                // Arrived but never touched it (a ledge perch, say): write
                // it off so the ladder can't loop on it forever.
                if (dg->weaponPickup >= 0 && sim->pickups[dg->weaponPickup].alive) {
                    sim->pickups[dg->weaponPickup].unreachable = true;
                }
                dg->weaponPickup = -1;
                PlanCurrentObjective(sim, "weapon_gone");
            } else {
                // Heal spot reached; TouchPickups normally flips us back
                // earlier, so the pickup must be gone. Move on.
                dg->objective = OBJ_EXIT;
                dg->healPickup = -1;
                DgSetState(sim, DG_ADVANCE, "heal_gone");
                PlanCurrentObjective(sim, "heal_gone");
            }
            return;
        }
        dg->wpCursor++;
    }

    if (Dist2D(dg->pos, dg->prevPos) < 0.5f) {
        if (++dg->stuckTics >= STUCK_TICS) {
            dg->stuckTics = 0;
            dg->stuckStrikes++;
            // Self-healing nav: the edge we're grinding against lied about
            // being passable (squeeze, ledge lip, ...). Ban it for this run.
            int e = (dg->wpEdge && dg->wpCursor < dg->wpCount)
                        ? dg->wpEdge[dg->wpCursor] : -1;
            if (e >= 0 && sim->tEdgeBlocked) {
                sim->tEdgeBlocked[e] = 1;
                if (StateLogEnabled()) {
                    StateLogBegin("nav_edge_banned");
                    StateLogInt("tic", sim->tic);
                    StateLogInt("edge", e);
                    StateLogFloat("x", dg->pos.x);
                    StateLogFloat("y", dg->pos.y);
                    StateLogEnd();
                }
            }
            if (dg->stuckStrikes >= 4 && dg->objective == OBJ_WEAPON &&
                dg->weaponPickup >= 0) {
                sim->pickups[dg->weaponPickup].unreachable = true;
                dg->weaponPickup = -1;
                dg->objective = OBJ_EXIT;
                dg->stuckStrikes = 0;
            }
            PlanCurrentObjective(sim, "stuck");
        }
    } else {
        dg->stuckTics = 0;
    }
}

static void DgFightTick(Sim *sim)
{
    DoomGuy *dg = &sim->dg;
    Mob *t = &sim->mobs[dg->targetMob];
    Vector2 tpos = t->pos;
    float dist = Dist2D(dg->pos, tpos);

    DgFace(dg, tpos, 35.0f);   // combat reflexes turn faster than travel

    DGWeapon want = DgChooseWeapon(dg, t, dist);
    if (want != dg->weapon) {
        dg->weapon = want;
        dg->fireCooldown = (dg->fireCooldown < 10) ? 10 : dg->fireCooldown;
        if (StateLogEnabled()) {
            StateLogBegin("weapon_switch");
            StateLogInt("tic", sim->tic);
            StateLogStr("weapon", DGWeaponName(want));
            StateLogEnd();
        }
    }

    if (dg->fireCooldown > 0) dg->fireCooldown--;
    bool hasAmmo = true;
    switch (dg->weapon) {
    case WPN_SHOTGUN:  hasAmmo = dg->shells > 0; break;
    case WPN_ROCKET:   hasAmmo = dg->rockets > 0; break;
    default:           hasAmmo = dg->bullets > 0; break;
    }
    if (dg->fireCooldown <= 0 && hasAmmo && fabsf(DgFacingErr(dg, tpos)) < 8.0f &&
        SightClearZ(sim, dg->pos, dg->z + DG_EYE, tpos, t->z + 24.0f)) {
        DgFire(sim);
    }

    // Never stand still: orbit the target at preferred range, back off from
    // melee chasers, flip orbit direction when a wall interrupts it.
    float wantRange = (dg->weapon == WPN_SHOTGUN) ? 220.0f
                    : (dg->weapon == WPN_ROCKET)  ? 420.0f : 300.0f;
    bool meleeThreat = (MOB_INFO[t->kind].meleeRange > 0.0f &&
                        !MOB_INFO[t->kind].hasMissile && MOB_INFO[t->kind].pellets == 0);
    if (meleeThreat) wantRange = 240.0f;
    float ux = (tpos.x - dg->pos.x)/fmaxf(dist, 1.0f);
    float uy = (tpos.y - dg->pos.y)/fmaxf(dist, 1.0f);
    float radial = (dist - wantRange) * 0.04f;
    if (radial > 1.0f) radial = 1.0f;
    if (radial < -1.5f) radial = -1.5f;
    Vector2 mv = {
        (-uy*dg->strafeDir + ux*radial),
        ( ux*dg->strafeDir + uy*radial),
    };
    float ml = sqrtf(mv.x*mv.x + mv.y*mv.y);
    if (ml > 0.01f) { mv.x /= ml; mv.y /= ml; }
    float speed = DG_RUNSPEED;   // circle-strafe at full run, like he means it
    Vector2 before = dg->pos;
    float moved = MoveEntity(sim, &dg->pos, dg->z, DG_RADIUS,
                             (Vector2){ mv.x*speed, mv.y*speed }, false, -1);
    if (moved < speed*0.3f || SimRandom(sim) < 3) dg->strafeDir = -dg->strafeDir;
    DgRunTriggers(sim, before, dg->pos);
    DgUpdateGround(sim);
}

static void DgTick(Sim *sim)
{
    DoomGuy *dg = &sim->dg;
    dg->prevPos = dg->pos;
    dg->prevZ = dg->z;

    if (dg->state == DG_EXITED || dg->state == DG_NO_GOAL || dg->state == DG_DEAD)
        return;

    TouchPickups(sim);
    if (dg->state == DG_DEAD) return;   // pickup path can't kill, but be safe

    // Threat scan: drives ADVANCE/WAIT_DOOR -> FIGHT and FIGHT retargeting.
    // RETREAT keeps running — he flees, he doesn't trade.
    if (--dg->threatScanTics <= 0) {
        dg->threatScanTics = 4;
        if (dg->targetMob >= 0 && sim->mobs[dg->targetMob].state == MOB_DEAD)
            dg->targetMob = -1;
        int seen = DgPickTarget(sim);
        if (dg->state == DG_FIGHT) {
            if (seen >= 0) {
                dg->targetMob = seen;
                dg->noTargetTics = 0;
            }
        } else if (seen >= 0 && dg->state != DG_RETREAT) {
            dg->targetMob = seen;
            dg->strafeDir = (SimRandom(sim) & 1) ? 1.0f : -1.0f;
            dg->waitDoor = -1;
            DgSetState(sim, DG_FIGHT, "enemy_sighted");
        }
    }

    // NO_GOAL heartbeat: the world changes (doors, drops); retry every ~5s
    // instead of standing down forever.
    if (dg->state == DG_NO_GOAL) {
        if (sim->tic % 175 == 0 && sim->goalSector >= 0) {
            dg->objective = OBJ_EXIT;
            if (PlanCurrentObjective(sim, "no_goal_retry")) {
                DgSetState(sim, DG_ADVANCE, "route_found");
            }
        }
        return;
    }

    // Low health: break off for the nearest health kit, if any exists.
    if (dg->health < DG_LOW_HP && dg->objective != OBJ_HEAL &&
        dg->state != DG_RETREAT) {
        int heal = NearestHealPickup(sim, dg->pos);
        if (heal >= 0 &&
            RouteExists(sim, sim->pickups[heal].pos, sim->goal)) {
            dg->objective = OBJ_HEAL;
            dg->healPickup = heal;
            if (PlanPathTo(sim, sim->pickups[heal].pos, "low_health")) {
                DgSetState(sim, DG_RETREAT, "low_health");
            } else {
                dg->objective = OBJ_EXIT;
                dg->healPickup = -1;
            }
        }
    }

    switch (dg->state) {
    case DG_WAIT_DOOR: {
        Door *d = &sim->doors[dg->waitDoor];
        float opening = d->ceil - sim->map->sectors[d->sector].floorH;
        if (opening >= DG_HEIGHT) {
            dg->waitDoor = -1;
            DgSetState(sim, DG_ADVANCE, "door_open");
        } else if (d->state == DOOR_CLOSED || d->state == DOOR_CLOSING) {
            DoorUse(sim, dg->waitDoor);   // re-use if it raced shut
        }
        break;
    }
    case DG_WAIT_LIFT: {
        DgRunTriggers(sim, dg->pos, dg->pos);   // press the call switch
        Lift *l = &sim->lifts[dg->waitLift];
        if (l->floor - dg->z <= DG_STEP) {
            dg->waitLift = -1;
            DgSetState(sim, DG_ADVANCE, "lift_arrived");
        } else if (l->state == LIFT_UP) {
            // It went back up without him; call it again or step closer.
            if (LiftTriggerInRange(sim, dg->waitLift, dg->pos)) {
                LiftActivate(sim, dg->waitLift);
            } else {
                dg->waitLift = -1;
                DgSetState(sim, DG_ADVANCE, "lift_left");
            }
        }
        break;
    }
    case DG_FIGHT:
        if (dg->targetMob < 0 || sim->mobs[dg->targetMob].state == MOB_DEAD ||
            !SightClearZ(sim, dg->pos, dg->z + DG_EYE,
                         sim->mobs[dg->targetMob].pos,
                         sim->mobs[dg->targetMob].z + 24.0f)) {
            if (++dg->noTargetTics >= 30) {
                dg->noTargetTics = 0;
                dg->targetMob = -1;
                DgSetState(sim, DG_ADVANCE, "area_clear");
                PlanCurrentObjective(sim, "after_fight");
            }
            break;
        }
        dg->noTargetTics = 0;
        DgFightTick(sim);
        break;
    case DG_ADVANCE:
    case DG_RETREAT:
        DgFollowRoute(sim);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------- goal/spawn

static void FindGoal(Sim *sim)
{
    const DoomMap *map = sim->map;
    sim->exitLine = -1;
    // S1 exit switches first (11/51), walkover exits as fallback (52/124).
    const int prefer[] = { 11, 51 };
    const int fallback[] = { 52, 124 };
    for (int pass = 0; pass < 2 && sim->exitLine < 0; pass++) {
        const int *specs = (pass == 0) ? prefer : fallback;
        for (int i = 0; i < map->lineCount && sim->exitLine < 0; i++) {
            for (int k = 0; k < 2; k++) {
                if (map->lines[i].special == specs[k]) { sim->exitLine = i; break; }
            }
        }
    }
    if (sim->exitLine < 0) {
        sim->goalSector = -1;
        return;
    }
    const Linedef *l = &map->lines[sim->exitLine];
    Vector2 v1 = map->verts[l->v1], v2 = map->verts[l->v2];
    Vector2 mid = { (v1.x + v2.x)*0.5f, (v1.y + v2.y)*0.5f };
    int frontSec = (l->sideFront >= 0) ? map->sides[l->sideFront].sector : -1;

    // Stand-off point on the usable (front) side; probe both normals and
    // keep whichever lands in the front sector rather than trusting the
    // winding convention.
    float dx = v2.x - v1.x, dy = v2.y - v1.y;
    float len = sqrtf(dx*dx + dy*dy);
    sim->goal = mid;
    sim->goalSector = frontSec;
    if (len > 0.001f && frontSec >= 0) {
        Vector2 nrm = { dy/len, -dx/len };
        Vector2 a = { mid.x + nrm.x*32.0f, mid.y + nrm.y*32.0f };
        Vector2 b = { mid.x - nrm.x*32.0f, mid.y - nrm.y*32.0f };
        if (SimSectorAtPoint(sim, a.x, a.y) == frontSec) sim->goal = a;
        else if (SimSectorAtPoint(sim, b.x, b.y) == frontSec) sim->goal = b;
    }
    if (sim->goalSector < 0) {
        sim->goalSector = SimSectorAtPoint(sim, sim->goal.x, sim->goal.y);
    }
}

static bool SpawnDoomGuy(Sim *sim)
{
    const DoomMap *map = sim->map;
    for (int i = 0; i < map->thingCount; i++) {
        if (map->things[i].type != 1) continue;
        DoomGuy *dg = &sim->dg;
        dg->pos = (Vector2){ map->things[i].x, map->things[i].y };
        dg->prevPos = dg->pos;
        dg->angleDeg = map->things[i].angleDeg;
        dg->health = 100;
        dg->armor = 0;
        dg->bullets = 50;                    // vanilla pistol start
        dg->shells = 0;
        dg->rockets = 0;
        dg->hasShotgun = false;
        dg->hasChaingun = false;
        dg->hasRocketLauncher = false;
        dg->weapon = WPN_PISTOL;
        dg->targetMob = -1;
        dg->healPickup = -1;
        dg->weaponPickup = -1;
        dg->strafeDir = 1.0f;
        dg->objective = OBJ_EXIT;
        dg->sector = SimSectorAtPoint(sim, dg->pos.x, dg->pos.y);
        dg->z = (dg->sector >= 0) ? map->sectors[dg->sector].floorH : 0.0f;
        dg->prevZ = dg->z;
        dg->waitDoor = -1;
        dg->waitLift = -1;
        return true;
    }
    return false;
}

static int SpawnMob(Sim *sim, MobKind kind, float x, float y, float angleDeg,
                    bool awake, bool playerDropped)
{
    if (sim->mobCount >= MOBS_MAX) return -1;
    bool onMap = false;
    float floorH = SimFloorAtPoint(sim, x, y, &onMap);
    if (!onMap) return -1;
    if (playerDropped) Snd(sim, SND_TELEPT, (Vector2){ x, y });
    int idx;
    if (sim->mobCount < MOBS_MAX) {
        idx = sim->mobCount++;
    } else {
        // Array full: reclaim the oldest gibbed corpse slot.
        idx = -1;
        long oldest = 0;
        for (int i = 0; i < sim->mobCount; i++) {
            if (sim->mobs[i].state != MOB_DEAD || !sim->mobs[i].gibbed) continue;
            if (idx < 0 || sim->mobs[i].deathTic < oldest) {
                idx = i;
                oldest = sim->mobs[i].deathTic;
            }
        }
        if (idx < 0) return -1;
    }
    Mob *m = &sim->mobs[idx];
    *m = (Mob){ 0 };
    m->kind = kind;
    m->pos = m->prevPos = (Vector2){ x, y };
    m->z = m->prevZ = floorH + (MOB_INFO[kind].flyer ? 40.0f : 0.0f);
    m->angleDeg = angleDeg;
    m->health = MOB_INFO[kind].health;
    m->state = awake ? MOB_CHASE : MOB_IDLE;
    m->awake = awake;
    m->targetMob = -1;
    m->raiseTarget = -1;
    m->playerDropped = playerDropped;
    return idx;
}

bool SimDrop(Sim *sim, MobKind kind, float x, float y)
{
    if (kind < 0 || kind >= MK_COUNT) return false;
    int idx = SpawnMob(sim, kind, x, y, 0.0f, true, true);
    if (idx < 0) return false;
    if (StateLogEnabled()) {
        StateLogBegin("drop");
        StateLogInt("tic", sim->tic);
        StateLogStr("kind", MobKindName(kind));
        StateLogFloat("x", x);
        StateLogFloat("y", y);
        StateLogBool("free", true);
        StateLogEnd();
    }
    return true;
}

// Souls drip faster as he advances — "Hell's grip tightens". Keyed to his
// straight-line progress toward the exit, not wall-clock, so a cautious
// marine doesn't bankroll the player for free.
float SimSoulRate(const Sim *sim)
{
    float progress = 0.0f;
    if (sim->goalSector >= 0 && sim->startGoalDist > 1.0f &&
        sim->dg.state != DG_NO_GOAL) {
        progress = 1.0f - Dist2D(sim->dg.pos, sim->goal)/sim->startGoalDist;
        if (progress < 0.0f) progress = 0.0f;
        if (progress > 1.0f) progress = 1.0f;
    }
    return 4.0f + 6.0f*progress;
}

int SimDropCost(Sim *sim, MobKind kind, float x, float y, bool *hot)
{
    bool inSight = false;
    if (sim->dg.state != DG_DEAD && sim->dg.state != DG_EXITED &&
        sim->dg.state != DG_NO_GOAL) {
        Vector2 p = { x, y };
        bool onMap = false;
        float pz = SimFloorAtPoint(sim, x, y, &onMap);
        inSight = onMap && Dist2D(sim->dg.pos, p) < SIGHT_RANGE &&
                  SightClearZ(sim, sim->dg.pos, sim->dg.z + DG_EYE, p, pz + 32.0f);
    }
    if (hot) *hot = inSight;
    float c = (float)SimDropBaseCost(kind);
    return (int)ceilf(inSight ? c*HOT_DROP_MULT : c);
}

DropResult SimTryDrop(Sim *sim, MobKind kind, float x, float y)
{
    if (kind < 0 || kind >= MK_COUNT) return DROP_BAD_SPOT;
    bool onMap = false;
    SimFloorAtPoint(sim, x, y, &onMap);
    if (!onMap) return DROP_BAD_SPOT;
    if (sim->mobCount >= MOBS_MAX) return DROP_FULL;

    bool hot = false;
    int cost = SimDropCost(sim, kind, x, y, &hot);
    if (sim->souls < (float)cost) return DROP_NO_SOULS;

    if (hot) {
        int slot = -1;
        for (int i = 0; i < PENDING_MAX; i++) {
            if (!sim->pending[i].active) { slot = i; break; }
        }
        if (slot < 0) return DROP_FULL;
        sim->pending[slot] = (PendingDrop){
            .active = true,
            .spawnTic = sim->tic + HOT_DROP_TELEGRAPH,
            .kind = kind,
            .pos = { x, y },
        };
    } else {
        if (SpawnMob(sim, kind, x, y, 0.0f, true, true) < 0) return DROP_FULL;
    }
    sim->souls -= (float)cost;
    sim->soulsSpent += cost;
    sim->dropsMade++;
    if (StateLogEnabled()) {
        StateLogBegin("drop");
        StateLogInt("tic", sim->tic);
        StateLogStr("kind", MobKindName(kind));
        StateLogFloat("x", x);
        StateLogFloat("y", y);
        StateLogInt("cost", cost);
        StateLogBool("hot", hot);
        StateLogFloat("souls", sim->souls);
        StateLogEnd();
    }
    return DROP_OK;
}

static void PendingTick(Sim *sim)
{
    for (int i = 0; i < PENDING_MAX; i++) {
        PendingDrop *p = &sim->pending[i];
        if (!p->active || sim->tic < p->spawnTic) continue;
        p->active = false;
        SpawnMob(sim, p->kind, p->pos.x, p->pos.y, 0.0f, true, true);
    }
}

// Map THINGS -> live monsters and pickup entities. Multiplayer-only things
// (flag 0x10) are skipped, and the vanilla skill filter applies at
// Hurt-Me-Plenty (bit 0x02): spawning every skill's monsters made the
// unaided baseline unwinnable. Inverted-difficulty selection comes later.
static void SpawnMapThings(Sim *sim)
{
    static const struct { int ednum; PickupType type; } PICKUP_THINGS[] = {
        { 2011, PK_STIM }, { 2012, PK_MEDIKIT }, { 2018, PK_ARMOR },
        { 2001, PK_SHOTGUN }, { 2007, PK_CLIP }, { 2048, PK_CLIPBOX },
        { 2008, PK_SHELLS }, { 2049, PK_SHELLBOX },
        { 2002, PK_CHAINGUN }, { 2003, PK_RLAUNCH },
        { 2010, PK_ROCKET }, { 2046, PK_ROCKETBOX },
    };
    const DoomMap *map = sim->map;
    int mobs = 0, pickups = 0;
    for (int i = 0; i < map->thingCount; i++) {
        const Thing *t = &map->things[i];
        if (t->flags & 0x10) continue;
        if (!(t->flags & 0x02)) continue;   // not present at Hurt-Me-Plenty
        if (t->type == 2035 && sim->barrelCount < BARRELS_MAX) {
            bool onMap = false;
            float fh = SimFloorAtPoint(sim, t->x, t->y, &onMap);
            if (onMap) {
                sim->barrels[sim->barrelCount++] = (Barrel){
                    .alive = true, .pos = { t->x, t->y }, .z = fh, .health = 20 };
            }
            continue;
        }
        bool placed = false;
        for (int k = 0; k < MK_COUNT && !placed; k++) {
            if (MOB_INFO[k].ednum != t->type) continue;
            if (SpawnMob(sim, (MobKind)k, t->x, t->y, t->angleDeg, false, false) >= 0)
                mobs++;
            placed = true;
        }
        for (size_t k = 0; k < sizeof(PICKUP_THINGS)/sizeof(PICKUP_THINGS[0]) && !placed; k++) {
            if (PICKUP_THINGS[k].ednum != t->type) continue;
            bool onMap = false;
            float floorH = SimFloorAtPoint(sim, t->x, t->y, &onMap);
            if (onMap) {
                SpawnPickup(sim, PICKUP_THINGS[k].type,
                            (Vector2){ t->x, t->y }, floorH, false);
                pickups++;
            }
            placed = true;
        }
    }
    TraceLog(LOG_INFO, "SIM: spawned %d monsters, %d pickups, %d barrels "
             "from map things", mobs, pickups, sim->barrelCount);
}

// --------------------------------------------------------- hell's hand

static bool VerbPay(Sim *sim, VerbKind vk, int cost, const char *what,
                    VerbResult *out)
{
    if (sim->tic < sim->verbReadyTic[vk]) { *out = VERB_COOLDOWN; return false; }
    if (sim->souls < (float)cost) { *out = VERB_NO_SOULS; return false; }
    sim->souls -= (float)cost;
    sim->soulsSpent += cost;
    sim->verbReadyTic[vk] = sim->tic + VERB_COOLDOWN_TICS;
    if (StateLogEnabled()) {
        StateLogBegin("verb");
        StateLogInt("tic", sim->tic);
        StateLogStr("what", what);
        StateLogInt("cost", cost);
        StateLogFloat("souls", sim->souls);
        StateLogEnd();
    }
    *out = VERB_OK;
    return true;
}

VerbResult SimSlamMover(Sim *sim, float x, float y, int *costOut)
{
    Vector2 p = { x, y };
    int door = -1, lift = -1;
    float bestDoor = VERB_RANGE, bestLift = VERB_RANGE;
    for (int i = 0; i < sim->doorCount; i++) {
        float d = Dist2D(DoorCenter(sim, i), p);
        if (d < bestDoor) { bestDoor = d; door = i; }
    }
    for (int i = 0; i < sim->liftCount; i++) {
        if (sim->lifts[i].riser || sim->lifts[i].stayLow) continue;
        float d = Dist2D(SectorAnyPoint(sim, sim->lifts[i].sector), p);
        if (d < bestLift) { bestLift = d; lift = i; }
    }
    VerbResult res;
    if (lift >= 0 && (door < 0 || bestLift < bestDoor)) {
        if (costOut) *costOut = VERB_LIFT_COST;
        if (!VerbPay(sim, VERB_LIFT, VERB_LIFT_COST, "lift_recall", &res)) return res;
        Lift *l = &sim->lifts[lift];
        l->recalled = true;
        if (l->state == LIFT_UP || l->state == LIFT_RISING) {
            l->state = LIFT_DOWN;
            Snd(sim, SND_PSTART, SectorAnyPoint(sim, l->sector));
        } else if (l->state == LIFT_LOW) {
            l->timer += DOOR_HOLD_TICS;
            l->recalled = false;
        }
        return VERB_OK;
    }
    if (door >= 0) {
        if (costOut) *costOut = VERB_DOOR_COST;
        if (!VerbPay(sim, VERB_DOOR, VERB_DOOR_COST, "door_slam", &res)) return res;
        Door *d = &sim->doors[door];
        d->holdUntil = sim->tic + DOOR_HOLD_TICS;
        if (d->state == DOOR_OPEN || d->state == DOOR_OPENING) {
            d->state = DOOR_CLOSING;
            d->stayOpen = false;
            Snd(sim, SND_DORCLS, DoorCenter(sim, door));
            LogDoorEvent(sim, door, "slammed");
        }
        return VERB_OK;
    }
    return VERB_NO_TARGET;
}

VerbResult SimDetonateBarrel(Sim *sim, float x, float y)
{
    Vector2 p = { x, y };
    int best = -1;
    float bestD = VERB_RANGE;
    for (int i = 0; i < sim->barrelCount; i++) {
        if (!sim->barrels[i].alive) continue;
        float d = Dist2D(sim->barrels[i].pos, p);
        if (d < bestD) { bestD = d; best = i; }
    }
    if (best < 0) return VERB_NO_TARGET;
    VerbResult res;
    if (!VerbPay(sim, VERB_BARREL, VERB_BARREL_COST, "barrel_detonate", &res))
        return res;
    DamageBarrel(sim, best, 999, -2);   // -2: the player's hand, not his
    return VERB_OK;
}

// ---------------------------------------------------------------- lifecycle

void SimFree(Sim *sim)
{
    if (!sim) return;
    free(sim->tEdges);
    free(sim->tEdgeStart);
    free(sim->edges);
    free(sim->edgeStart);
    free(sim->bmLines);
    free(sim->bmStart);
    free(sim->lineStamp);
    free(sim->doors);
    free(sim->sectorDoor);
    free(sim->lifts);
    free(sim->sectorLift);
    free(sim->lineUsed);
    free(sim->dg.wp);
    free(sim->dg.wpFloor);
    free(sim->dg.wpDoor);
    free(sim->dg.wpLift);
    memset(sim, 0, sizeof(*sim));
}

bool SimInit(Sim *sim, const DoomMap *map, const LevelMesh *lm)
{
    memset(sim, 0, sizeof(*sim));
    sim->map = map;
    sim->lm = lm;
    RndInit(sim);

    sim->lineUsed = (unsigned char *)calloc((size_t)map->lineCount, 1);
    if (!sim->lineUsed || !BuildDoors(sim) || !BuildLifts(sim)) {
        TraceLog(LOG_ERROR, "SIM: out of memory building sim");
        SimFree(sim);
        return false;
    }
    BuildTaggedActions(sim);
    if (!BuildNav(sim) || !BuildTriNav(sim) || !BuildBlockmap(sim)) {
        TraceLog(LOG_ERROR, "SIM: out of memory building sim");
        SimFree(sim);
        return false;
    }
    FindGoal(sim);

    bool spawned = SpawnDoomGuy(sim);
    SpawnMapThings(sim);
    sim->souls = SOULS_START;
    sim->startGoalDist = (spawned && sim->goalSector >= 0)
                             ? Dist2D(sim->dg.pos, sim->goal) : 0.0f;
    if (!spawned) {
        TraceLog(LOG_WARNING, "SIM: no player 1 start on this map");
        sim->dg.state = DG_NO_GOAL;
    } else if (sim->goalSector < 0) {
        TraceLog(LOG_WARNING, "SIM: no exit line on this map");
        sim->dg.state = DG_NO_GOAL;
    } else {
        sim->dg.state = DG_ADVANCE;
        PlanCurrentObjective(sim, "init");
    }

    int lockedDoors = 0;
    for (int i = 0; i < sim->doorCount; i++) lockedDoors += sim->doors[i].locked ? 1 : 0;
    TraceLog(LOG_INFO,
             "SIM: %d tri edges, %d sector edges, %d doors (%d locked), "
             "%d lifts, exit line %d, route %s (%d wps)",
             sim->tEdgeCount, sim->edgeCount, sim->doorCount, lockedDoors,
             sim->liftCount, sim->exitLine,
             (sim->dg.wpCount > 0) ? "ok" : "NONE", sim->dg.wpCount);

    if (StateLogEnabled()) {
        StateLogBegin("sim_init");
        StateLogInt("nav_tri_edges", sim->tEdgeCount);
        StateLogInt("nav_edges", sim->edgeCount);
        StateLogInt("doors", sim->doorCount);
        StateLogInt("lifts", sim->liftCount);
        StateLogInt("locked_doors", lockedDoors);
        StateLogInt("exit_line", sim->exitLine);
        StateLogObjBegin("start");
        StateLogFloat("x", sim->dg.pos.x);
        StateLogFloat("y", sim->dg.pos.y);
        StateLogInt("sector", sim->dg.sector);
        StateLogObjEnd();
        StateLogObjBegin("goal");
        StateLogFloat("x", sim->goal.x);
        StateLogFloat("y", sim->goal.y);
        StateLogInt("sector", sim->goalSector);
        StateLogObjEnd();
        StateLogStr("state", DGStateName(sim->dg.state));
        StateLogInt("waypoints", sim->dg.wpCount);
        StateLogInt("mobs", sim->mobCount);
        StateLogInt("pickups", sim->pickupCount);
        StateLogInt("barrels", sim->barrelCount);
        StateLogEnd();
    }
    return true;
}

void SimTick(Sim *sim)
{
    sim->tic++;
    if (sim->dg.state != DG_DEAD && sim->dg.state != DG_EXITED) {
        sim->souls += SimSoulRate(sim)/(float)SIM_TICRATE;
        if (sim->souls > SOULS_CAP) sim->souls = SOULS_CAP;
    }
    PendingTick(sim);
    for (int i = 0; i < sim->doorCount; i++) DoorTick(sim, i);
    for (int i = 0; i < sim->liftCount; i++) LiftTick(sim, i);
    DgTick(sim);
    for (int i = 0; i < sim->mobCount; i++) MobTick(sim, i);
    for (int i = 0; i < PROJS_MAX; i++) {
        if (sim->projs[i].alive) ProjTick(sim, &sim->projs[i]);
    }
    for (int i = 0; i < TRACERS_MAX; i++) {
        if (sim->tracers[i].ttl > 0) sim->tracers[i].ttl--;
    }
}
