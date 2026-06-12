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
};

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

float SimFloorAtPoint(const Sim *sim, float x, float y, bool *ok)
{
    int s = SimSectorAtPoint(sim, x, y);
    if (ok) *ok = (s >= 0);
    return (s >= 0) ? sim->map->sectors[s].floorH : 0.0f;
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

static void DoorUse(Sim *sim, int di)
{
    Door *d = &sim->doors[di];
    switch (d->state) {
    case DOOR_CLOSED:
        d->state = DOOR_OPENING;
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

static bool EdgePassable(const Sim *sim, int from, int to, const Linedef *l)
{
    const Sector *a = &sim->map->sectors[from];
    const Sector *b = &sim->map->sectors[to];
    if (b->floorH - a->floorH > DG_STEP) return false;   // climb limit; drops ok
    float top = fminf(PotentialCeil(sim, from), PotentialCeil(sim, to));
    float bottom = fmaxf(a->floorH, b->floorH);
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
            if (Dist2D(v1, v2) < 2.0f*DG_RADIUS) continue;   // too narrow
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
                    sim->edges[fill[from]++] =
                        (NavEdge){ .to = to, .line = i, .door = door, .wp = mid };
                }
            }
        }
        if (pass == 1) free(fill);
    }
    return true;
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
    int line = -1, door = -1;
    if (sa != sb) {
        line = FindLineByKey(lk, lkN, from->a, from->b);
        if (line < 0) return false;
        const Linedef *l = &map->lines[line];
        if (l->flags & ML_BLOCKING) return false;
        if (Dist2D(map->verts[l->v1], map->verts[l->v2]) < 2.0f*DG_RADIUS) return false;
        const Sector *A = &map->sectors[sa], *B = &map->sectors[sb];
        if (B->floorH - A->floorH > DG_STEP) return false;
        float top = fminf(PotentialCeil(sim, sa), PotentialCeil(sim, sb));
        float bottom = fmaxf(A->floorH, B->floorH);
        if (top - bottom < DG_HEIGHT) return false;
        door = (sim->sectorDoor[sb] >= 0) ? sim->sectorDoor[sb] : sim->sectorDoor[sa];
    }
    out->to = to->tri;
    out->line = line;
    out->door = door;
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
    float bottom = fmaxf(a->floorH, b->floorH);
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

const char *DGStateName(DGState s)
{
    switch (s) {
    case DG_ADVANCE:   return "advance";
    case DG_WAIT_DOOR: return "wait_door";
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
            if (dg->wpDoor[j] >= 0) break;
        }
        dg->wp[out] = dg->wp[far];
        dg->wpFloor[out] = dg->wpFloor[far];
        dg->wpDoor[out] = dg->wpDoor[far];
        cur = dg->wp[out];
        out++;
        i = far + 1;
    }
    dg->wpCount = out;
    if (dg->wpCursor > dg->wpCount) dg->wpCursor = dg->wpCount;
}

// ---------------------------------------------------------------- sight

// A line blocks sight only when its two sides share no vertical opening
// (solid wall, or a door whose CURRENT ceiling is shut).
static bool LineBlocksSight(const Sim *sim, const Linedef *l)
{
    if (l->sideFront < 0 || l->sideBack < 0) return true;
    int fs = sim->map->sides[l->sideFront].sector;
    int bs = sim->map->sides[l->sideBack].sector;
    if (fs < 0 || bs < 0) return true;
    int dfa = sim->sectorDoor[fs], dfb = sim->sectorDoor[bs];
    float ceilA = (dfa >= 0) ? sim->doors[dfa].ceil : sim->map->sectors[fs].ceilH;
    float ceilB = (dfb >= 0) ? sim->doors[dfb].ceil : sim->map->sectors[bs].ceilH;
    float top = fminf(ceilA, ceilB);
    float bottom = fmaxf(sim->map->sectors[fs].floorH, sim->map->sectors[bs].floorH);
    return (top - bottom) < 8.0f;
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

// Earliest sight-blocking wall along a->b as a fraction of the segment;
// 1.0 means clear. Used for both vision and hitscan wall hits.
static float SightT(Sim *sim, Vector2 a, Vector2 b)
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
                if (!LineBlocksSight(sim, l)) continue;
                float t = SegSegT(a, b, sim->map->verts[l->v1], sim->map->verts[l->v2]);
                if (t < best) best = t;
            }
        }
    }
    return best;
}

static bool SightClear(Sim *sim, Vector2 a, Vector2 b)
{
    return SightT(sim, a, b) >= 1.0f;
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

static bool SightClear(Sim *sim, Vector2 a, Vector2 b);

// Gunfire alerts monsters that can see the muzzle, or are close enough to
// hear it through a wall. Without the sight gate, one shot rallies the whole
// map — and pre-placed ambushes behind walls would never stay asleep.
static void WakeNearby(Sim *sim, Vector2 pos, float radius)
{
    for (int i = 0; i < sim->mobCount; i++) {
        Mob *m = &sim->mobs[i];
        if (m->state == MOB_DEAD || m->awake) continue;
        float d = Dist2D(m->pos, pos);
        if (d > radius) continue;
        if (d > 250.0f && !SightClear(sim, m->pos, pos)) continue;
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
    StateLogFloat("x", m->pos.x);
    StateLogFloat("y", m->pos.y);
    StateLogEnd();
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
    } else if (attackerMob < 0) {
        m->targetMob = -1;   // retaliate at Doom Guy
    }

    if (m->health <= 0) {
        m->state = MOB_DEAD;
        m->stateTics = 0;
        if (attackerMob < 0) sim->kills++;
        LogMobDeath(sim, m, attackerMob);
        int drop = MOB_INFO[m->kind].drop;
        if (drop >= 0) SpawnPickup(sim, (PickupType)drop, m->pos, m->z, true);
        return;
    }
    if (SimRandom(sim) < MOB_INFO[m->kind].painChance) {
        m->state = MOB_PAIN;
        m->stateTics = 8;
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
// the ray; the first body on the line eats it — including other monsters,
// which is exactly how infights start.
static void FireHitscan(Sim *sim, int shooterMob, Vector2 from, float zFrom,
                        Vector2 at, float spreadDeg, int dmg)
{
    float aim = atan2f(at.y - from.y, at.x - from.x);
    float spread = ((float)(SimRandom(sim) - SimRandom(sim)) / 255.0f) * spreadDeg * DEG2RAD;
    float a = aim + spread;
    Vector2 end = { from.x + cosf(a)*HITSCAN_RANGE, from.y + sinf(a)*HITSCAN_RANGE };
    float tWall = SightT(sim, from, end);

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

    Vector2 hit = { from.x + (end.x - from.x)*tBest, from.y + (end.y - from.y)*tBest };
    float zTo = zFrom;
    if (hitDG) zTo = sim->dg.z + DG_EYE*0.7f;
    else if (hitMob >= 0) zTo = sim->mobs[hitMob].z + 24.0f;
    PushTracer(sim, from, zFrom, hit, zTo, shooterMob < 0);

    if (hitDG) DamageDG(sim, dmg, shooterMob);
    else if (hitMob >= 0) DamageMob(sim, hitMob, dmg, shooterMob);
}

static void SpawnProjectile(Sim *sim, int ownerMob, Vector2 from, float zFrom,
                            Vector2 at, float zAt, float speed, int damageMult)
{
    for (int i = 0; i < PROJS_MAX; i++) {
        Proj *p = &sim->projs[i];
        if (p->alive) continue;
        float dist = Dist2D(from, at);
        if (dist < 1.0f) dist = 1.0f;
        Vector2 dir = { (at.x - from.x)/dist, (at.y - from.y)/dist };
        p->alive = true;
        p->pos = p->prevPos = (Vector2){ from.x + dir.x*16.0f, from.y + dir.y*16.0f };
        p->vel = (Vector2){ dir.x*speed, dir.y*speed };
        p->z = p->prevZ = zFrom;
        p->zVel = (zAt - zFrom) / (dist/speed);
        p->damageMult = damageMult;
        p->ownerMob = ownerMob;
        p->ownerKind = (ownerMob >= 0) ? sim->mobs[ownerMob].kind : MK_COUNT;
        return;
    }
}

static void ProjTick(Sim *sim, Proj *p)
{
    p->prevPos = p->pos;
    p->prevZ = p->z;
    Vector2 next = { p->pos.x + p->vel.x, p->pos.y + p->vel.y };
    float nextZ = p->z + p->zVel;

    float tWall = SightT(sim, p->pos, next);
    float tBest = tWall;
    int hitMob = -1;
    bool hitDG = false;
    for (int i = 0; i < sim->mobCount; i++) {
        Mob *m = &sim->mobs[i];
        if (i == p->ownerMob || m->state == MOB_DEAD) continue;
        float t = SegCircleT(p->pos, next, m->pos, MOB_INFO[m->kind].radius + 6.0f);
        if (t < tBest) { tBest = t; hitMob = i; }
    }
    if (sim->dg.state != DG_DEAD && sim->dg.state != DG_EXITED) {
        float t = SegCircleT(p->pos, next, sim->dg.pos, DG_RADIUS + 6.0f);
        if (t < tBest) { tBest = t; hitMob = -1; hitDG = true; }
    }

    if (tBest < 1.0f) {
        int dmg = (SimRandom(sim)%8 + 1) * p->damageMult;
        if (hitDG) DamageDG(sim, dmg, p->ownerMob);
        else if (hitMob >= 0) DamageMob(sim, hitMob, dmg, p->ownerMob);
        p->alive = false;
        return;
    }
    p->pos = next;
    p->z = nextZ;
    bool onMap = false;
    float floorH = SimFloorAtPoint(sim, p->pos.x, p->pos.y, &onMap);
    if (!onMap || p->z < floorH + 2.0f) p->alive = false;   // ground/void hit
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
        }
        if (!took) continue;
        if (dg->health > 100) dg->health = 100;
        if (dg->bullets > 200) dg->bullets = 200;
        if (dg->shells > 50) dg->shells = 50;
        p->alive = false;
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

static void MobAttack(Sim *sim, int idx, Vector2 tpos, float tz, bool targetIsDG, int targetMob)
{
    Mob *m = &sim->mobs[idx];
    const MobInfo *inf = &MOB_INFO[m->kind];
    float dist = Dist2D(m->pos, tpos);
    if (inf->meleeRange > 0.0f && dist < inf->meleeRange + inf->radius) {
        int dmg = (SimRandom(sim)%inf->meleeDie + 1) * inf->meleeMult;
        if (targetIsDG) DamageDG(sim, dmg, idx);
        else DamageMob(sim, targetMob, dmg, idx);
    } else if (inf->pellets > 0) {
        // Monster aim degrades with range, vanilla-feel: point blank ~25%
        // whiff, across a courtyard closer to half.
        int whiff = 55 + (int)(dist*0.07f);
        if (whiff > 130) whiff = 130;
        for (int p = 0; p < inf->pellets; p++) {
            if (SimRandom(sim) < whiff) continue;
            int dmg = (SimRandom(sim)%5 + 1) * 3;   // vanilla zombie bullet
            FireHitscan(sim, idx, m->pos, m->z + 32.0f, tpos, 13.0f, dmg);
        }
        WakeNearby(sim, m->pos, 500.0f);
    } else if (inf->hasMissile) {
        SpawnProjectile(sim, idx, m->pos, m->z + 32.0f, tpos, tz + 24.0f,
                        inf->missileSpeed, inf->missileMult);
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
            SightClear(sim, m->pos, sim->dg.pos)) {
            m->awake = true;
            m->state = MOB_CHASE;
        }
        break;
    case MOB_PAIN:
        if (--m->stateTics <= 0) m->state = MOB_CHASE;
        break;
    case MOB_ATTACK:
        MobFace(m, tpos, 45.0f);
        m->stateTics--;
        if (m->stateTics == 4) MobAttack(sim, idx, tpos, tz, targetIsDG, m->targetMob);
        if (m->stateTics <= 0) {
            m->state = MOB_CHASE;
            m->attackCooldown = 30 + SimRandom(sim)%50;
        }
        break;
    case MOB_CHASE: {
        if (m->attackCooldown > 0) m->attackCooldown--;
        float dist = Dist2D(m->pos, tpos);
        bool melee = (inf->meleeRange > 0.0f && dist < inf->meleeRange + inf->radius);
        bool ranged = (inf->pellets > 0 || inf->hasMissile);
        if (m->attackCooldown <= 0 && (melee ||
            (ranged && dist < 1100.0f && SightClear(sim, m->pos, tpos)))) {
            // Doom-ish gate: the closer the target, the likelier the attack.
            int gate = melee ? 256 : 70 + (int)fmaxf(0.0f, 110.0f - dist*0.09f);
            if (SimRandom(sim) < gate) {
                m->state = MOB_ATTACK;
                m->stateTics = 10;
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
        if (!dg->wp || !dg->wpFloor || !dg->wpDoor) return false;
        dg->wp[0] = goalPos;
        dg->wpFloor[0] = map->sectors[goalSector].floorH;
        dg->wpDoor[0] = -1;
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
    if (!dg->wp || !dg->wpFloor || !dg->wpDoor) { free(n); return false; }
    int k = len - 1;
    for (int s = goalNode; s != startNode; s = n[s].parentNode, k--) {
        const NavEdge *E = &edges[n[s].parentEdge];
        int destSector = triGraph ? sim->lm->floorTriSector[E->to] : E->to;
        dg->wp[k] = E->wp;
        dg->wpFloor[k] = map->sectors[destSector].floorH;
        dg->wpDoor[k] = E->door;
    }
    dg->wp[len] = goalPos;
    dg->wpFloor[len] = map->sectors[goalSector].floorH;
    dg->wpDoor[len] = -1;
    dg->wpCount = len + 1;
    free(n);
    return true;
}

static bool PlanPathTo(Sim *sim, Vector2 goalPos, const char *reason)
{
    DoomGuy *dg = &sim->dg;
    free(dg->wp);
    free(dg->wpFloor);
    free(dg->wpDoor);
    dg->wp = NULL;
    dg->wpFloor = NULL;
    dg->wpDoor = NULL;
    dg->wpCount = 0;
    dg->wpCursor = 0;
    dg->waitDoor = -1;
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

    if (StateLogEnabled()) {
        StateLogBegin("doomguy_replan");
        StateLogInt("tic", sim->tic);
        StateLogStr("reason", reason);
        StateLogStr("objective", (sim->dg.objective == OBJ_HEAL) ? "heal" : "exit");
        StateLogBool("ok", ok);
        StateLogStr("method", method);
        StateLogInt("waypoints", dg->wpCount);
        StateLogEnd();
    }
    return ok;
}

// Route to the current objective; a failed heal route falls back to the
// exit, a failed exit route is fatal (NO_GOAL).
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

// Nearest live monster he can see from eye height, or -1.
static int DgPickTarget(Sim *sim)
{
    DoomGuy *dg = &sim->dg;
    int best = -1;
    float bestD = SIGHT_RANGE + 100.0f;
    for (int i = 0; i < sim->mobCount; i++) {
        Mob *m = &sim->mobs[i];
        if (m->state == MOB_DEAD) continue;
        float d = Dist2D(dg->pos, m->pos);
        if (d >= bestD) continue;
        if (!SightClear(sim, dg->pos, m->pos)) continue;
        bestD = d;
        best = i;
    }
    return best;
}

static void DgFire(Sim *sim)
{
    DoomGuy *dg = &sim->dg;
    Mob *t = &sim->mobs[dg->targetMob];
    if (dg->weapon == WPN_SHOTGUN) {
        dg->shells--;
        for (int p = 0; p < 7; p++) {
            int dmg = (SimRandom(sim)%3 + 1) * 5;   // vanilla 5*(1d3) per pellet
            FireHitscan(sim, -1, dg->pos, dg->z + DG_EYE, t->pos, 6.0f, dmg);
        }
        dg->fireCooldown = 37;                       // vanilla shotgun cadence
    } else {
        dg->bullets--;
        int dmg = (SimRandom(sim)%3 + 1) * 5;
        FireHitscan(sim, -1, dg->pos, dg->z + DG_EYE, t->pos, 2.5f, dmg);
        dg->fireCooldown = 10;                       // vanilla pistol cadence
    }
    WakeNearby(sim, dg->pos, 800.0f);                // gunfire carries
}

static void DgUpdateGround(Sim *sim)
{
    DoomGuy *dg = &sim->dg;
    int sec = SimSectorAtPoint(sim, dg->pos.x, dg->pos.y);
    if (sec >= 0) {
        dg->sector = sec;
        dg->z = sim->map->sectors[sec].floorH;
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
    if (dist > 0.001f) {
        float step = (DG_RUNSPEED < dist) ? DG_RUNSPEED : dist;
        MoveEntity(sim, &dg->pos, dg->z, DG_RADIUS,
                   (Vector2){ dx/dist*step, dy/dist*step }, false, -1);
    }
    DgUpdateGround(sim);

    bool last = (dg->wpCursor == dg->wpCount - 1);
    // Heal runs must end ON the pickup (touch radius 32), not merely near it,
    // or arrival flips the objective while the kit is still on the floor.
    float arrive = last ? ((dg->objective == OBJ_HEAL) ? 12.0f : DG_USERANGE*0.75f)
                        : WP_ARRIVE;
    if (Dist2D(dg->pos, target) < arrive) {
        if (last) {
            if (dg->objective == OBJ_EXIT) {
                if (sim->exitLine >= 0) DgTriggerExit(sim);
                else DgSetState(sim, DG_NO_GOAL, "no_exit_line");
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

    // Weapon choice: shotgun when fed, else pistol.
    DGWeapon want = (dg->hasShotgun && dg->shells > 0) ? WPN_SHOTGUN : WPN_PISTOL;
    if (want != dg->weapon) {
        dg->weapon = want;
        dg->fireCooldown = (dg->fireCooldown < 10) ? 10 : dg->fireCooldown;
        if (StateLogEnabled()) {
            StateLogBegin("weapon_switch");
            StateLogInt("tic", sim->tic);
            StateLogStr("weapon", (want == WPN_SHOTGUN) ? "shotgun" : "pistol");
            StateLogEnd();
        }
    }

    if (dg->fireCooldown > 0) dg->fireCooldown--;
    bool hasAmmo = (dg->weapon == WPN_SHOTGUN) ? dg->shells > 0 : dg->bullets > 0;
    if (dg->fireCooldown <= 0 && hasAmmo && fabsf(DgFacingErr(dg, tpos)) < 8.0f &&
        SightClear(sim, dg->pos, tpos)) {
        DgFire(sim);
    }

    // Never stand still: orbit the target at preferred range, back off from
    // melee chasers, flip orbit direction when a wall interrupts it.
    float wantRange = (dg->weapon == WPN_SHOTGUN) ? 220.0f : 300.0f;
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
    float moved = MoveEntity(sim, &dg->pos, dg->z, DG_RADIUS,
                             (Vector2){ mv.x*speed, mv.y*speed }, false, -1);
    if (moved < speed*0.3f || SimRandom(sim) < 3) dg->strafeDir = -dg->strafeDir;
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

    // Low health: break off for the nearest health kit, if any exists.
    if (dg->health < DG_LOW_HP && dg->objective != OBJ_HEAL &&
        dg->state != DG_RETREAT) {
        int heal = NearestHealPickup(sim, dg->pos);
        if (heal >= 0) {
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
    case DG_FIGHT:
        if (dg->targetMob < 0 || sim->mobs[dg->targetMob].state == MOB_DEAD ||
            !SightClear(sim, dg->pos, sim->mobs[dg->targetMob].pos)) {
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
        dg->hasShotgun = false;
        dg->weapon = WPN_PISTOL;
        dg->targetMob = -1;
        dg->healPickup = -1;
        dg->strafeDir = 1.0f;
        dg->objective = OBJ_EXIT;
        dg->sector = SimSectorAtPoint(sim, dg->pos.x, dg->pos.y);
        dg->z = (dg->sector >= 0) ? map->sectors[dg->sector].floorH : 0.0f;
        dg->prevZ = dg->z;
        dg->waitDoor = -1;
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
    int idx = sim->mobCount++;
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
        inSight = Dist2D(sim->dg.pos, p) < SIGHT_RANGE &&
                  SightClear(sim, sim->dg.pos, p);
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
// (flag 0x10) are skipped; everything else spawns regardless of skill bits.
static void SpawnMapThings(Sim *sim)
{
    static const struct { int ednum; PickupType type; } PICKUP_THINGS[] = {
        { 2011, PK_STIM }, { 2012, PK_MEDIKIT }, { 2018, PK_ARMOR },
        { 2001, PK_SHOTGUN }, { 2007, PK_CLIP }, { 2048, PK_CLIPBOX },
        { 2008, PK_SHELLS }, { 2049, PK_SHELLBOX },
    };
    const DoomMap *map = sim->map;
    int mobs = 0, pickups = 0;
    for (int i = 0; i < map->thingCount; i++) {
        const Thing *t = &map->things[i];
        if (t->flags & 0x10) continue;
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
    TraceLog(LOG_INFO, "SIM: spawned %d monsters, %d pickups from map things",
             mobs, pickups);
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
    free(sim->dg.wp);
    free(sim->dg.wpFloor);
    free(sim->dg.wpDoor);
    memset(sim, 0, sizeof(*sim));
}

bool SimInit(Sim *sim, const DoomMap *map, const LevelMesh *lm)
{
    memset(sim, 0, sizeof(*sim));
    sim->map = map;
    sim->lm = lm;
    RndInit(sim);

    if (!BuildDoors(sim) || !BuildNav(sim) || !BuildTriNav(sim) || !BuildBlockmap(sim)) {
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
             "exit line %d, route %s (%d wps)",
             sim->tEdgeCount, sim->edgeCount, sim->doorCount, lockedDoors,
             sim->exitLine, (sim->dg.wpCount > 0) ? "ok" : "NONE", sim->dg.wpCount);

    if (StateLogEnabled()) {
        StateLogBegin("sim_init");
        StateLogInt("nav_tri_edges", sim->tEdgeCount);
        StateLogInt("nav_edges", sim->edgeCount);
        StateLogInt("doors", sim->doorCount);
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
    DgTick(sim);
    for (int i = 0; i < sim->mobCount; i++) MobTick(sim, i);
    for (int i = 0; i < PROJS_MAX; i++) {
        if (sim->projs[i].alive) ProjTick(sim, &sim->projs[i]);
    }
    for (int i = 0; i < TRACERS_MAX; i++) {
        if (sim->tracers[i].ttl > 0) sim->tracers[i].ttl--;
    }
}
