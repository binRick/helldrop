// Helldrop entry point: isometric Doom-level viewer loop.

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"

#include "wad.h"
#include "map.h"
#include "levelmesh.h"
#include "sim.h"
#include "doomgfx.h"
#include "sprites.h"
#include "sound.h"
#include "statelog.h"

#define HELLDROP_VERSION "0.1.0"

#define CAM_DIST   4000.0f   // keeps the ortho camera outside all geometry
#define YAW_HOME     45.0f
#define PITCH_HOME   50.0f
#define PITCH_MIN    25.0f
#define PITCH_MAX    85.0f
#define FOVY_MIN    150.0f
#define FOVY_MAX   8000.0f

typedef struct Options {
    const char *wadPath;
    const char *mapName;
    bool noLog;
    bool selftest;
    bool autodrop;         // dev tool: scripted drops onto his route
    long exitAfter;        // 0 = run until window close
    long turbo;            // sim speed multiplier (1 = real time)
} Options;

typedef struct ViewState {
    Vector3 target;
    float yawDeg;
    float targetYawDeg;    // Q/E snap destination; yawDeg lerps toward it
    float pitchDeg;
    float fovy;            // ortho vertical view height, world units
    Vector3 homeTarget;
    float homeFovy;
    bool atZoomMin, atZoomMax;
} ViewState;

// ---------------------------------------------------------------- args

static void PrintUsage(FILE *out)
{
    fprintf(out,
        "usage: helldrop [--wad <path>] [--map <name>] [--no-log]\n"
        "                [--selftest] [--exit-after <N>] [--turbo <N>] [--autodrop]\n");
}

static bool ParseArgs(int argc, char **argv, Options *opt)
{
    opt->wadPath = "assets/freedoom1.wad";
    opt->mapName = "E1M1";
    opt->noLog = false;
    opt->selftest = false;
    opt->autodrop = false;
    opt->exitAfter = 0;
    opt->turbo = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--wad") == 0 && i + 1 < argc) {
            opt->wadPath = argv[++i];
        } else if (strcmp(argv[i], "--map") == 0 && i + 1 < argc) {
            opt->mapName = argv[++i];
        } else if (strcmp(argv[i], "--no-log") == 0) {
            opt->noLog = true;
        } else if (strcmp(argv[i], "--selftest") == 0) {
            opt->selftest = true;
        } else if (strcmp(argv[i], "--autodrop") == 0) {
            opt->autodrop = true;
        } else if (strcmp(argv[i], "--exit-after") == 0 && i + 1 < argc) {
            char *end = NULL;
            opt->exitAfter = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || opt->exitAfter <= 0) return false;
        } else if (strcmp(argv[i], "--turbo") == 0 && i + 1 < argc) {
            char *end = NULL;
            opt->turbo = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || opt->turbo <= 0 || opt->turbo > 200)
                return false;
        } else {
            return false;
        }
    }
    return true;
}

static const char *PathBasename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// ---------------------------------------------------------------- things

typedef enum ThingCat {
    CAT_OTHER = 0,
    CAT_PLAYER,
    CAT_MONSTER,
    CAT_BOSS,
    CAT_WEAPON,
    CAT_HEALTH,
    CAT_KEY,
} ThingCat;

static const struct { int doomednum; unsigned char cat; } THING_CATS[] = {
    {1, CAT_PLAYER}, {2, CAT_PLAYER}, {3, CAT_PLAYER}, {4, CAT_PLAYER},
    {3004, CAT_MONSTER}, {9, CAT_MONSTER}, {3001, CAT_MONSTER},
    {3002, CAT_MONSTER}, {58, CAT_MONSTER}, {3005, CAT_MONSTER},
    {3006, CAT_MONSTER}, {3003, CAT_MONSTER}, {65, CAT_MONSTER},
    {64, CAT_MONSTER}, {66, CAT_MONSTER}, {67, CAT_MONSTER},
    {68, CAT_MONSTER}, {69, CAT_MONSTER}, {71, CAT_MONSTER},
    {16, CAT_BOSS}, {7, CAT_BOSS},
    {2001, CAT_WEAPON}, {2002, CAT_WEAPON}, {2003, CAT_WEAPON},
    {2004, CAT_WEAPON}, {2005, CAT_WEAPON}, {2006, CAT_WEAPON},
    {2007, CAT_WEAPON}, {2008, CAT_WEAPON}, {2010, CAT_WEAPON},
    {2046, CAT_WEAPON}, {2047, CAT_WEAPON}, {2048, CAT_WEAPON},
    {2049, CAT_WEAPON}, {17, CAT_WEAPON}, {8, CAT_WEAPON},
    {2011, CAT_HEALTH}, {2012, CAT_HEALTH}, {2013, CAT_HEALTH},
    {2014, CAT_HEALTH}, {2015, CAT_HEALTH}, {2018, CAT_HEALTH},
    {2019, CAT_HEALTH}, {2022, CAT_HEALTH}, {2023, CAT_HEALTH},
    {2024, CAT_HEALTH}, {2025, CAT_HEALTH}, {2026, CAT_HEALTH},
    {83, CAT_HEALTH},
    {5, CAT_KEY}, {6, CAT_KEY}, {13, CAT_KEY},
    {38, CAT_KEY}, {39, CAT_KEY}, {40, CAT_KEY},
};

static ThingCat ThingCategory(int doomednum)
{
    for (size_t i = 0; i < sizeof(THING_CATS)/sizeof(THING_CATS[0]); i++) {
        if (THING_CATS[i].doomednum == doomednum) return (ThingCat)THING_CATS[i].cat;
    }
    return CAT_OTHER;
}

static Color ThingColor(ThingCat cat)
{
    switch (cat) {
    case CAT_PLAYER:  return (Color){ 60, 255, 100, 255 };
    case CAT_MONSTER: return (Color){ 225,  45,  40, 255 };
    case CAT_BOSS:    return (Color){ 140,  12,  12, 255 };
    case CAT_WEAPON:  return (Color){ 250, 215,  50, 255 };
    case CAT_HEALTH:  return (Color){  70, 130, 255, 255 };
    case CAT_KEY:     return (Color){ 240,  60, 240, 255 };
    default:          return (Color){ 185, 185, 185, 255 };
    }
}

static bool PointInTri2D(float px, float pz,
                         float ax, float az, float bx, float bz, float cx, float cz)
{
    // sign-of-cross test accepting either winding; eps tolerates edge points
    const float EPS = 0.5f;
    float d1 = (px - bx)*(az - bz) - (ax - bx)*(pz - bz);
    float d2 = (px - cx)*(bz - cz) - (bx - cx)*(pz - cz);
    float d3 = (px - ax)*(cz - az) - (cx - ax)*(pz - az);
    bool hasNeg = (d1 < -EPS) || (d2 < -EPS) || (d3 < -EPS);
    bool hasPos = (d1 >  EPS) || (d2 >  EPS) || (d3 >  EPS);
    return !(hasNeg && hasPos);
}

// Ground height per thing, from the floors triangle soup (world-space
// (x, h, z), z = -doomY). Overlapping sectors: take the highest floor.
static float *ComputeThingHeights(const DoomMap *map, const Mesh *floors)
{
    if (map->thingCount <= 0) return NULL;
    float *heights = calloc((size_t)map->thingCount, sizeof(float));
    if (!heights) {
        TraceLog(LOG_WARNING, "MAIN: out of memory for thing heights");
        return NULL;
    }
    const float *v = floors->vertices;
    for (int i = 0; i < map->thingCount; i++) {
        const Thing *t = &map->things[i];
        float px = t->x, pz = -t->y;
        bool found = false;
        float best = 0.0f;
        for (int base = 0; v && base + 2 < floors->vertexCount; base += 3) {
            const float *a = &v[(size_t)(base + 0)*3];
            const float *b = &v[(size_t)(base + 1)*3];
            const float *c = &v[(size_t)(base + 2)*3];
            if (!PointInTri2D(px, pz, a[0], a[2], b[0], b[2], c[0], c[2])) continue;
            float y = a[1];
            if (b[1] > y) y = b[1];
            if (c[1] > y) y = c[1];
            if (!found || y > best) best = y;
            found = true;
        }
        if (!found) {
            TraceLog(LOG_WARNING,
                     "MAIN: thing %d (type %d) at (%.0f, %.0f) outside all floors, ground = 0",
                     i, t->type, t->x, t->y);
        }
        heights[i] = best;
    }
    return heights;
}

static void DrawThings(const DoomMap *map, const float *heights)
{
    if (!heights) return;
    for (int i = 0; i < map->thingCount; i++) {
        const Thing *t = &map->things[i];
        ThingCat cat = ThingCategory(t->type);
        // Players, monsters, and supported pickups are live sim entities now;
        // only keys and unclassified decoration remain static markers.
        if (cat == CAT_PLAYER || cat == CAT_MONSTER || cat == CAT_BOSS ||
            cat == CAT_WEAPON || cat == CAT_HEALTH) continue;
        float scale = (cat == CAT_BOSS) ? 1.5f : 1.0f;
        float radius = 14.0f * scale;
        float height = 28.0f * scale;
        Vector3 base = { t->x, heights[i], -t->y };
        Color c = ThingColor(cat);
        Color outline = { (unsigned char)(c.r/2), (unsigned char)(c.g/2),
                          (unsigned char)(c.b/2), 255 };
        DrawCylinder(base, radius, radius, height, 10, c);
        DrawCylinderWires(base, radius, radius, height, 10, outline);
    }
}

// ---------------------------------------------------------------- sim draw

// Render-side lerp between the last two sim tics keeps 35 Hz movement
// smooth at 60 fps.
static Vector3 DgRenderPos(const Sim *sim, float alpha)
{
    const DoomGuy *dg = &sim->dg;
    float x = dg->prevPos.x + (dg->pos.x - dg->prevPos.x)*alpha;
    float y = dg->prevPos.y + (dg->pos.y - dg->prevPos.y)*alpha;
    float z = dg->prevZ + (dg->z - dg->prevZ)*alpha;
    return (Vector3){ x, z, -y };
}

static void DrawDoomGuy(const Sim *sim, float alpha, const GameSprites *gs,
                        Camera3D cam, float camYaw)
{
    Vector3 p = DgRenderPos(sim, alpha);
    const SpriteFrame *f = gs->valid ? DGSpriteFrame(gs, &sim->dg, sim->tic) : NULL;
    if (f) {
        DrawSpriteBillboard(cam, f, SpriteRotation(sim->dg.angleDeg, camYaw), p, WHITE);
        return;
    }
    Vector3 feet = { p.x, p.y + DG_RADIUS, p.z };
    Vector3 head = { p.x, p.y + DG_HEIGHT - DG_RADIUS, p.z };
    Color body = (sim->dg.state == DG_EXITED) ? (Color){ 255, 220, 60, 255 }
                                              : (Color){ 60, 255, 100, 255 };
    DrawCapsule(feet, head, DG_RADIUS, 10, 4, body);
    DrawCapsuleWires(feet, head, DG_RADIUS, 10, 4, (Color){ 20, 90, 35, 255 });
    float a = sim->dg.angleDeg*DEG2RAD;
    Vector3 chest = { p.x, p.y + 40.0f, p.z };
    Vector3 nose = { p.x + cosf(a)*42.0f, p.y + 40.0f, p.z - sinf(a)*42.0f };
    DrawLine3D(chest, nose, (Color){ 235, 255, 240, 255 });
    DrawSphere(nose, 4.0f, (Color){ 235, 255, 240, 255 });
}

static void DrawRoute(const Sim *sim, float alpha)
{
    const DoomGuy *dg = &sim->dg;
    const Color path = { 80, 220, 255, 255 };
    if (dg->state == DG_ADVANCE || dg->state == DG_WAIT_DOOR) {
        Vector3 prev = DgRenderPos(sim, alpha);
        prev.y += 6.0f;
        for (int k = dg->wpCursor; k < dg->wpCount; k++) {
            Vector3 w = { dg->wp[k].x, dg->wpFloor[k] + 6.0f, -dg->wp[k].y };
            DrawLine3D(prev, w, path);
            DrawSphere(w, 5.0f, path);
            prev = w;
        }
    }
    if (sim->goalSector >= 0) {
        Vector3 g = { sim->goal.x,
                      sim->map->sectors[sim->goalSector].floorH + 24.0f,
                      -sim->goal.y };
        DrawSphere(g, 12.0f, (Color){ 255, 120, 30, 255 });
        DrawSphereWires(g, 16.0f, 8, 8, (Color){ 255, 180, 90, 180 });
    }
}

static const Color MOB_COLORS[MK_COUNT] = {
    [MK_ZOMBIE]     = { 205, 175, 125, 255 },
    [MK_SHOTGUNGUY] = { 150, 150,  95, 255 },
    [MK_IMP]        = { 185,  90,  55, 255 },
    [MK_PINKY]      = { 240, 135, 160, 255 },
    [MK_CACO]       = { 215,  70,  80, 255 },
};

static Vector3 MobRenderPos(const Mob *m, float alpha)
{
    float x = m->prevPos.x + (m->pos.x - m->prevPos.x)*alpha;
    float y = m->prevPos.y + (m->pos.y - m->prevPos.y)*alpha;
    float z = m->prevZ + (m->z - m->prevZ)*alpha;
    return (Vector3){ x, z, -y };
}

static void DrawMobs(const Sim *sim, float alpha, const GameSprites *gs,
                     Camera3D cam, float camYaw)
{
    for (int i = 0; i < sim->mobCount; i++) {
        const Mob *m = &sim->mobs[i];
        float r = MobRadius(m->kind);
        Vector3 p = MobRenderPos(m, alpha);
        const SpriteFrame *f = gs->valid ? MobSpriteFrame(gs, m, sim->tic) : NULL;
        if (f) {
            DrawSpriteBillboard(cam, f, SpriteRotation(m->angleDeg, camYaw), p, WHITE);
        } else if (m->state == MOB_DEAD) {
            DrawCylinder(p, r*0.9f, r*0.9f, 3.0f, 8, (Color){ 95, 42, 42, 255 });
        } else {
            Color c = MOB_COLORS[m->kind];
            if (m->state == MOB_PAIN) c = (Color){ 255, 255, 255, 255 };
            Color outline = { (unsigned char)(c.r/3), (unsigned char)(c.g/3),
                              (unsigned char)(c.b/3), 255 };
            if (MobIsFlyer(m->kind)) {
                DrawSphere(p, r*0.85f, c);
                DrawSphereWires(p, r*0.85f, 8, 8, outline);
            } else {
                DrawCylinder(p, r*0.85f, r*0.85f, 42.0f, 10, c);
                DrawCylinderWires(p, r*0.85f, r*0.85f, 42.0f, 10, outline);
            }
            float a = m->angleDeg*DEG2RAD;
            float zc = p.y + 28.0f;
            DrawLine3D((Vector3){ p.x, zc, p.z },
                       (Vector3){ p.x + cosf(a)*(r + 14.0f), zc,
                                  p.z - sinf(a)*(r + 14.0f) }, RAYWHITE);
        }
        int maxHp = MobMaxHealth(m->kind);
        if (m->state != MOB_DEAD && m->health < maxHp) {
            float frac = (float)m->health/(float)maxHp;
            float w = 40.0f;
            float top = p.y + 66.0f;
            DrawCube((Vector3){ p.x, top, p.z }, w, 3.0f, 3.0f,
                     (Color){ 60, 10, 10, 255 });
            DrawCube((Vector3){ p.x - w*(1.0f - frac)*0.5f, top, p.z },
                     w*frac, 3.5f, 3.5f, (Color){ 70, 220, 80, 255 });
        }
    }
}

static void DrawProjectiles(const Sim *sim, float alpha, const GameSprites *gs,
                            Camera3D cam)
{
    for (int i = 0; i < PROJS_MAX; i++) {
        const Proj *p = &sim->projs[i];
        if (!p->alive) continue;
        float x = p->prevPos.x + (p->pos.x - p->prevPos.x)*alpha;
        float y = p->prevPos.y + (p->pos.y - p->prevPos.y)*alpha;
        float z = p->prevZ + (p->z - p->prevZ)*alpha;
        const SpriteFrame *f = gs->valid ? BallSpriteFrame(gs, p->ownerKind, sim->tic)
                                         : NULL;
        if (f) {
            // Balls anchor centered, not feet-planted: drop the base by half.
            float h = (float)f->tex[0].height;
            DrawSpriteBillboard(cam, f, 0, (Vector3){ x, z - h*0.5f, -y }, WHITE);
        } else {
            Color c = (p->ownerKind == MK_CACO) ? (Color){ 255, 60, 60, 255 }
                                                : (Color){ 255, 140, 40, 255 };
            DrawSphere((Vector3){ x, z, -y }, 7.0f, c);
        }
    }
}

static void DrawTracers(const Sim *sim)
{
    for (int i = 0; i < TRACERS_MAX; i++) {
        const Tracer *t = &sim->tracers[i];
        if (t->ttl <= 0) continue;
        unsigned char a = (unsigned char)(40 + 35*t->ttl);
        Color c = t->fromDG ? (Color){ 255, 245, 160, a } : (Color){ 255, 190, 150, a };
        DrawLine3D(t->a, t->b, c);
    }
}

static void DrawPickups(const Sim *sim, const GameSprites *gs, Camera3D cam)
{
    for (int i = 0; i < sim->pickupCount; i++) {
        const Pickup *p = &sim->pickups[i];
        if (!p->alive) continue;
        const SpriteFrame *f = gs->valid ? PickupSpriteFrame(gs, p->type, sim->tic)
                                         : NULL;
        if (f) {
            DrawSpriteBillboard(cam, f, 0, (Vector3){ p->pos.x, p->z, -p->pos.y },
                                WHITE);
            continue;
        }
        Color c;
        switch (p->type) {
        case PK_STIM: case PK_MEDIKIT: c = (Color){ 80, 140, 255, 255 }; break;
        case PK_ARMOR:                 c = (Color){ 80, 220, 100, 255 }; break;
        default:                       c = (Color){ 250, 215, 50, 255 }; break;
        }
        float bob = sinf((float)GetTime()*3.0f + (float)i)*2.0f;
        Vector3 pos = { p->pos.x, p->z + 10.0f + bob, -p->pos.y };
        DrawCube(pos, 14.0f, 10.0f, 14.0f, c);
        DrawCubeWires(pos, 14.0f, 10.0f, 14.0f,
                      (Color){ (unsigned char)(c.r/3), (unsigned char)(c.g/3),
                               (unsigned char)(c.b/3), 255 });
    }
}

// Door panels are the only moving geometry; the static level mesh knows
// nothing about them, so draw the visible slab (current ceiling up to the
// fully-open height) over every two-sided line bordering a door sector.
static void DrawDoors(const Sim *sim, float alpha)
{
    const DoomMap *map = sim->map;
    for (int i = 0; i < map->lineCount; i++) {
        const Linedef *l = &map->lines[i];
        if (l->sideFront < 0 || l->sideBack < 0) continue;
        int fs = map->sides[l->sideFront].sector;
        int bs = map->sides[l->sideBack].sector;
        int di = -1;
        if (bs >= 0 && sim->sectorDoor[bs] >= 0) di = sim->sectorDoor[bs];
        else if (fs >= 0 && sim->sectorDoor[fs] >= 0) di = sim->sectorDoor[fs];
        if (di < 0) continue;
        const Door *d = &sim->doors[di];
        float ceil = d->prevCeil + (d->ceil - d->prevCeil)*alpha;
        float top = d->openCeil + 4.0f;
        if (top - ceil < 0.5f) continue;   // fully open: panel hidden
        Vector2 a = map->verts[l->v1], b = map->verts[l->v2];
        Color col = (d->state == DOOR_OPENING || d->state == DOOR_CLOSING)
                        ? (Color){ 150, 130, 90, 255 }
                        : (Color){ 96, 88, 76, 255 };
        Vector3 a0 = { a.x, ceil, -a.y }, b0 = { b.x, ceil, -b.y };
        Vector3 a1 = { a.x, top, -a.y },  b1 = { b.x, top, -b.y };
        DrawTriangle3D(a0, b0, b1, col);
        DrawTriangle3D(a0, b1, a1, col);
        DrawTriangle3D(a0, b1, b0, col);
        DrawTriangle3D(a0, a1, b1, col);
    }
}

// ---------------------------------------------------------------- logging

static void LogInput(const char *action)
{
    if (!StateLogEnabled()) return;
    StateLogBegin("input");
    StateLogStr("action", action);
    StateLogEnd();
}

static void LogStartup(void)
{
    if (!StateLogEnabled()) return;
    StateLogBegin("startup");
    StateLogStr("version", HELLDROP_VERSION);
    StateLogStr("raylib", RAYLIB_VERSION);
    StateLogEnd();
}

static void LogLevelLoaded(const Options *opt, const DoomMap *map, const LevelMesh *lm)
{
    if (!StateLogEnabled()) return;
    StateLogBegin("level_loaded");
    StateLogStr("wad", opt->wadPath);
    StateLogStr("map", map->name);
    StateLogInt("sectors", map->sectorCount);
    StateLogInt("lines", map->lineCount);
    StateLogInt("sides", map->sideCount);
    StateLogInt("verts", map->vertCount);
    StateLogInt("things", map->thingCount);
    StateLogObjBegin("bounds");
    StateLogFloat("minx", map->minX);
    StateLogFloat("miny", map->minY);
    StateLogFloat("maxx", map->maxX);
    StateLogFloat("maxy", map->maxY);
    StateLogObjEnd();
    StateLogInt("floor_tris", lm->floors.vertexCount / 3);
    StateLogInt("wall_tris", lm->walls.vertexCount / 3);
    StateLogEnd();
}

static const char *DoorStateName(DoorState s)
{
    switch (s) {
    case DOOR_CLOSED:  return "closed";
    case DOOR_OPENING: return "opening";
    case DOOR_OPEN:    return "open";
    case DOOR_CLOSING: return "closing";
    }
    return "?";
}

// 5 Hz gamestate snapshot (every 12th frame at 60 fps): the debugging
// contract — every piece of live gameplay state belongs in here.
static void LogSnapshot(const ViewState *vs, const Sim *sim)
{
    if (!StateLogEnabled()) return;
    StateLogBegin("snapshot");
    StateLogStr("mode", "sim");
    StateLogInt("fps", GetFPS());
    StateLogInt("tic", sim->tic);
    StateLogInt("rnd_index", sim->rndIndex);
    StateLogObjBegin("camera");
    StateLogFloat("tx", vs->target.x);
    StateLogFloat("ty", vs->target.y);
    StateLogFloat("tz", vs->target.z);
    StateLogFloat("yaw", vs->yawDeg);
    StateLogFloat("pitch", vs->pitchDeg);
    StateLogFloat("fovy", vs->fovy);
    StateLogObjEnd();
    StateLogObjBegin("doomguy");
    StateLogFloat("x", sim->dg.pos.x);
    StateLogFloat("y", sim->dg.pos.y);
    StateLogFloat("z", sim->dg.z);
    StateLogFloat("angle", sim->dg.angleDeg);
    StateLogStr("state", DGStateName(sim->dg.state));
    StateLogStr("objective", (sim->dg.objective == OBJ_HEAL) ? "heal" : "exit");
    StateLogInt("sector", sim->dg.sector);
    StateLogInt("health", sim->dg.health);
    StateLogInt("armor", sim->dg.armor);
    StateLogInt("bullets", sim->dg.bullets);
    StateLogInt("shells", sim->dg.shells);
    StateLogStr("weapon", (sim->dg.weapon == WPN_SHOTGUN) ? "shotgun" : "pistol");
    StateLogInt("target", sim->dg.targetMob);
    StateLogInt("wp_cursor", sim->dg.wpCursor);
    StateLogInt("wp_count", sim->dg.wpCount);
    StateLogInt("wait_door", sim->dg.waitDoor);
    StateLogInt("stuck_tics", sim->dg.stuckTics);
    StateLogInt("replans", sim->dg.replans);
    StateLogObjEnd();
    StateLogFloat("souls", sim->souls);
    StateLogInt("souls_spent", (int)sim->soulsSpent);
    StateLogInt("drops", sim->dropsMade);
    int pendingN = 0;
    for (int i = 0; i < PENDING_MAX; i++) pendingN += sim->pending[i].active ? 1 : 0;
    StateLogInt("pending_drops", pendingN);
    StateLogInt("kills", (int)sim->kills);
    int corpses = 0, alive = 0, projs = 0;
    for (int i = 0; i < sim->mobCount; i++) {
        if (sim->mobs[i].state == MOB_DEAD) corpses++;
        else alive++;
    }
    for (int i = 0; i < PROJS_MAX; i++) projs += sim->projs[i].alive ? 1 : 0;
    StateLogInt("mobs_alive", alive);
    StateLogInt("corpses", corpses);
    StateLogInt("projectiles", projs);
    StateLogArrBegin("mobs");
    for (int i = 0; i < sim->mobCount; i++) {
        const Mob *m = &sim->mobs[i];
        if (m->state == MOB_DEAD) continue;
        StateLogObjBegin(NULL);
        StateLogStr("kind", MobKindName(m->kind));
        StateLogFloat("x", m->pos.x);
        StateLogFloat("y", m->pos.y);
        StateLogInt("hp", m->health);
        StateLogStr("state", MobStateName(m->state));
        StateLogInt("target", m->targetMob);
        StateLogBool("dropped", m->playerDropped);
        StateLogObjEnd();
    }
    StateLogArrEnd();
    StateLogArrBegin("doors");
    for (int i = 0; i < sim->doorCount; i++) {
        StateLogObjBegin(NULL);
        StateLogInt("sector", sim->doors[i].sector);
        StateLogStr("state", DoorStateName(sim->doors[i].state));
        StateLogFloat("ceil", sim->doors[i].ceil);
        StateLogObjEnd();
    }
    StateLogArrEnd();
    StateLogEnd();
}

static void LogShutdown(long frames, long tics)
{
    if (!StateLogEnabled()) return;
    StateLogBegin("shutdown");
    StateLogInt("frames", frames);
    StateLogInt("tics", tics);
    StateLogFloat("seconds", GetTime());
    StateLogEnd();
}

// ---------------------------------------------------------------- camera

static void ViewReset(ViewState *vs)
{
    vs->target = vs->homeTarget;
    vs->yawDeg = YAW_HOME;
    vs->targetYawDeg = YAW_HOME;
    vs->pitchDeg = PITCH_HOME;
    vs->fovy = vs->homeFovy;
    vs->atZoomMin = false;
    vs->atZoomMax = false;
}

static void CameraUpdate(ViewState *vs, float dt)
{
    if (IsKeyPressed(KEY_R)) {
        ViewReset(vs);
        LogInput("reset");
    }
    if (IsKeyPressed(KEY_Q)) {
        vs->targetYawDeg += 90.0f;
        LogInput("rotate_left");
    }
    if (IsKeyPressed(KEY_E)) {
        vs->targetYawDeg -= 90.0f;
        LogInput("rotate_right");
    }
    float yawErr = vs->targetYawDeg - vs->yawDeg;
    if (fabsf(yawErr) < 0.05f) vs->yawDeg = vs->targetYawDeg;
    else vs->yawDeg += yawErr*fminf(1.0f, dt*12.0f);

    if (IsKeyDown(KEY_Z)) vs->pitchDeg -= 60.0f*dt;
    if (IsKeyDown(KEY_X)) vs->pitchDeg += 60.0f*dt;
    vs->pitchDeg = Clamp(vs->pitchDeg, PITCH_MIN, PITCH_MAX);

    float wheel = GetMouseWheelMove();
    if (wheel != 0.0f) {
        vs->fovy *= powf(0.9f, wheel);
        bool atMin = vs->fovy <= FOVY_MIN;
        bool atMax = vs->fovy >= FOVY_MAX;
        vs->fovy = Clamp(vs->fovy, FOVY_MIN, FOVY_MAX);
        if (atMin && !vs->atZoomMin) LogInput("zoom_min");
        if (atMax && !vs->atZoomMax) LogInput("zoom_max");
        vs->atZoomMin = atMin;
        vs->atZoomMax = atMax;
    }

    float yawRad = vs->yawDeg*DEG2RAD;
    Vector3 fwd   = { -sinf(yawRad), 0.0f, -cosf(yawRad) };  // screen-up, ground plane
    Vector3 right = {  cosf(yawRad), 0.0f, -sinf(yawRad) };

    Vector3 pan = { 0 };
    if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP))    pan = Vector3Add(pan, fwd);
    if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN))  pan = Vector3Subtract(pan, fwd);
    if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) pan = Vector3Add(pan, right);
    if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT))  pan = Vector3Subtract(pan, right);
    if (pan.x != 0.0f || pan.z != 0.0f) {
        pan = Vector3Scale(Vector3Normalize(pan), vs->fovy*0.9f*dt);
        vs->target = Vector3Add(vs->target, pan);
    }

    if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
        Vector2 d = GetMouseDelta();
        float unitsPerPixel = vs->fovy/(float)GetScreenHeight();
        float foreshorten = sinf(vs->pitchDeg*DEG2RAD);   // >= sin(25 deg)
        vs->target = Vector3Add(vs->target, Vector3Scale(right, -d.x*unitsPerPixel));
        vs->target = Vector3Add(vs->target, Vector3Scale(fwd, d.y*unitsPerPixel/foreshorten));
    }
}

static Camera3D CameraFromView(const ViewState *vs)
{
    float yaw = vs->yawDeg*DEG2RAD;
    float pitch = vs->pitchDeg*DEG2RAD;
    Camera3D cam = { 0 };
    cam.target = vs->target;
    cam.position = Vector3Add(vs->target, (Vector3){
        CAM_DIST*cosf(pitch)*sinf(yaw),
        CAM_DIST*sinf(pitch),
        CAM_DIST*cosf(pitch)*cosf(yaw) });
    cam.up = (Vector3){ 0.0f, 1.0f, 0.0f };
    cam.fovy = vs->fovy;
    cam.projection = CAMERA_ORTHOGRAPHIC;
    return cam;
}

// ---------------------------------------------------------------- hud

static void DrawTextShadowed(const char *text, int x, int y, int size, Color color)
{
    DrawText(text, x + 2, y + 2, size, (Color){ 0, 0, 0, 200 });
    DrawText(text, x, y, size, color);
}

static void DrawHud(const DoomMap *map, const char *wadBase, const Sim *sim, long turbo)
{
    int y = 12;
    DrawTextShadowed(map->name, 12, y, 24, RAYWHITE);
    y += 30;
    DrawTextShadowed(wadBase, 12, y, 16, LIGHTGRAY);
    y += 20;
    DrawTextShadowed(TextFormat("sectors %d  lines %d  things %d",
                                map->sectorCount, map->lineCount, map->thingCount),
                     12, y, 16, LIGHTGRAY);
    y += 20;
    DrawTextShadowed(TextFormat("%d fps", GetFPS()), 12, y, 16, GREEN);
    y += 20;
    DrawTextShadowed(TextFormat("DOOMGUY %s  hp %d  armor %d  sector %d  wp %d/%d  tic %ld%s",
                                DGStateName(sim->dg.state), sim->dg.health,
                                sim->dg.armor,
                                sim->dg.sector, sim->dg.wpCursor, sim->dg.wpCount,
                                sim->tic,
                                (turbo > 1) ? TextFormat("  turbo x%ld", turbo) : ""),
                     12, y, 16, (Color){ 120, 255, 150, 255 });
    y += 20;
    int alive = 0;
    for (int i = 0; i < sim->mobCount; i++) {
        if (sim->mobs[i].state != MOB_DEAD) alive++;
    }
    DrawTextShadowed(TextFormat("%s  bullets %d  shells %d  |  kills %ld  monsters %d",
                                (sim->dg.weapon == WPN_SHOTGUN) ? "SHOTGUN" : "PISTOL",
                                sim->dg.bullets, sim->dg.shells, sim->kills, alive),
                     12, y, 16, (Color){ 255, 220, 130, 255 });

    y += 20;
    DrawTextShadowed("WASD pan  RMB drag  wheel zoom  Q/E rotate  Z/X tilt  "
                     "R view  G restart  ESC quit",
                     12, y, 14, GRAY);

    if (sim->dg.state == DG_EXITED || sim->dg.state == DG_DEAD) {
        bool won = (sim->dg.state == DG_DEAD);
        const char *line1 = won ? "DOOM GUY IS DEAD - YOU WIN"
                                : "HE REACHED THE EXIT - YOU LOSE";
        const char *line2 = "press G to restart";
        const char *line3 = TextFormat("time %.0fs   his kills %ld   "
                                       "your drops %d   souls spent %ld",
                                       (double)sim->tic/SIM_TICRATE, sim->kills,
                                       sim->dropsMade, sim->soulsSpent);
        Color col = won ? (Color){ 90, 255, 110, 255 } : (Color){ 255, 60, 50, 255 };
        int w1 = MeasureText(line1, 40);
        int w2 = MeasureText(line2, 20);
        int w3 = MeasureText(line3, 18);
        int cx = GetScreenWidth()/2, cy = GetScreenHeight()/2;
        DrawRectangle(0, cy - 56, GetScreenWidth(), 124, (Color){ 0, 0, 0, 170 });
        DrawTextShadowed(line1, cx - w1/2, cy - 40, 40, col);
        DrawTextShadowed(line2, cx - w2/2, cy + 6, 20, RAYWHITE);
        DrawTextShadowed(line3, cx - w3/2, cy + 36, 18, LIGHTGRAY);
    } else if (sim->dg.state == DG_NO_GOAL) {
        const char *line1 = "NO ROUTE TO EXIT";
        int w1 = MeasureText(line1, 28);
        DrawTextShadowed(line1, GetScreenWidth()/2 - w1/2, 60, 28,
                         (Color){ 255, 170, 60, 255 });
    }
}

// Top-right: the soul economy at a glance — balance, drip rate, the priced
// bestiary with the selected monster highlighted and unaffordables dimmed.
static void DrawSoulsPanel(const Sim *sim, MobKind selected)
{
    int sw = GetScreenWidth();
    int y = 12;
    const char *souls = TextFormat("SOULS %d", (int)sim->souls);
    int w = MeasureText(souls, 30);
    DrawTextShadowed(souls, sw - w - 16, y, 30, (Color){ 255, 170, 70, 255 });
    y += 34;
    const char *rate = TextFormat("+%.1f/s", SimSoulRate(sim));
    w = MeasureText(rate, 16);
    DrawTextShadowed(rate, sw - w - 16, y, 16, (Color){ 200, 140, 70, 255 });
    y += 28;
    for (int k = 0; k < MK_COUNT; k++) {
        int cost = SimDropBaseCost((MobKind)k);
        const char *label = TextFormat("[%d] %-11s %3d", k + 1,
                                       MobKindName((MobKind)k), cost);
        Color c = MOB_COLORS[k];
        bool affordable = sim->souls >= (float)cost;
        if (!affordable) c = (Color){ 120, 70, 70, 255 };
        else if ((MobKind)k != selected) {
            c.r = (unsigned char)(c.r/2 + 50);
            c.g = (unsigned char)(c.g/2 + 50);
            c.b = (unsigned char)(c.b/2 + 50);
        }
        w = MeasureText(label, 18);
        DrawTextShadowed(label, sw - w - 16, y, 18, c);
        if ((MobKind)k == selected) {
            DrawRectangle(sw - w - 16, y + 20, w, 2, MOB_COLORS[k]);
        }
        y += 24;
    }
}

static void DrawPendingDrops(const Sim *sim, long frame)
{
    for (int i = 0; i < PENDING_MAX; i++) {
        const PendingDrop *p = &sim->pending[i];
        if (!p->active) continue;
        bool ok = false;
        float gz = SimFloorAtPoint(sim, p->pos.x, p->pos.y, &ok);
        if (!ok) continue;
        float pulse = 1.0f + 0.3f*sinf((float)frame*0.4f);
        Color c = { 120, 255, 140, 255 };
        Vector3 base = { p->pos.x, gz + 2.0f, -p->pos.y };
        DrawCircle3D(base, (MobRadius(p->kind) + 6.0f)*pulse,
                     (Vector3){ 1.0f, 0.0f, 0.0f }, 90.0f, c);
        DrawLine3D(base, (Vector3){ base.x, gz + 70.0f, base.z }, c);
    }
}

// ------------------------------------------------------------- status bar

// Inverted classic status bar: his mugshot bloodies as YOU win. Tracks the
// authentic face behaviors that matter (dead, ouch on 20+ damage bursts,
// evil grin on a new weapon) with plain-text fallback when lumps are missing.
typedef struct FaceState {
    int prevHealth;
    int hpRefFrames;
    int hpRef;
    int ouchFrames;
    int grinFrames;
    bool prevShotgun;
} FaceState;

static void FaceStateUpdate(FaceState *fs, const Sim *sim)
{
    if (sim->dg.hasShotgun && !fs->prevShotgun) fs->grinFrames = 70;
    fs->prevShotgun = sim->dg.hasShotgun;
    if (--fs->hpRefFrames <= 0) {
        fs->hpRefFrames = 35;
        fs->hpRef = sim->dg.health;
    }
    if (fs->hpRef - sim->dg.health >= 20 && fs->ouchFrames <= 0) fs->ouchFrames = 40;
    if (fs->ouchFrames > 0) fs->ouchFrames--;
    if (fs->grinFrames > 0) fs->grinFrames--;
    fs->prevHealth = sim->dg.health;
}

static void DrawBigNum(const DoomHud *hud, int value, int rightX, int y, int scale)
{
    if (value < 0) value = 0;
    if (value > 999) value = 999;
    int x = rightX;
    do {
        const Texture2D *t = &hud->digit[value % 10];
        x -= t->width*scale;
        DrawTextureEx(*t, (Vector2){ (float)x, (float)y }, 0.0f, (float)scale, WHITE);
        value /= 10;
    } while (value > 0);
}

static void DrawStatusBar(const DoomHud *hud, const Sim *sim, const FaceState *fs,
                          long frame)
{
    const int scale = 2;
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    int barW = 320*scale, barH = 32*scale;
    int x0 = (sw - barW)/2, y0 = sh - barH;

    if (hud->hasStbar) {
        DrawTextureEx(hud->stbar, (Vector2){ (float)x0, (float)y0 }, 0.0f,
                      (float)scale, WHITE);
    } else {
        DrawRectangle(x0, y0, barW, barH, (Color){ 40, 40, 40, 255 });
        DrawRectangleLines(x0, y0, barW, barH, (Color){ 90, 90, 90, 255 });
    }

    const DoomGuy *dg = &sim->dg;
    int tier = (100 - dg->health)*4/100;
    if (tier < 0) tier = 0;
    if (tier > 4) tier = 4;

    const Texture2D *face = NULL;
    if (dg->state == DG_DEAD && hud->hasDead) face = &hud->faceDead;
    else if (fs->ouchFrames > 0 && hud->hasOuch) face = &hud->faceOuch[tier];
    else if (fs->grinFrames > 0 && hud->hasGrin) face = &hud->faceGrin[tier];
    else if (hud->hasFaces) face = &hud->face[tier][(frame/24) % 3];
    if (face) {
        // ST_FACESX = 143 in the 320-wide bar; faces are ~24px wide.
        DrawTextureEx(*face, (Vector2){ (float)(x0 + 143*scale), (float)(y0 + 1*scale) },
                      0.0f, (float)scale, WHITE);
    }

    int ammo = (dg->weapon == WPN_SHOTGUN) ? dg->shells : dg->bullets;
    int numY = y0 + 3*scale;
    if (hud->hasDigits) {
        DrawBigNum(hud, ammo, x0 + 44*scale, numY, scale);
        DrawBigNum(hud, dg->health, x0 + 90*scale, numY, scale);
        DrawBigNum(hud, dg->armor, x0 + 221*scale, numY, scale);
        if (hud->hasPercent) {
            DrawTextureEx(hud->percent, (Vector2){ (float)(x0 + 90*scale), (float)numY },
                          0.0f, (float)scale, WHITE);
            DrawTextureEx(hud->percent, (Vector2){ (float)(x0 + 221*scale), (float)numY },
                          0.0f, (float)scale, WHITE);
        }
    } else {
        DrawTextShadowed(TextFormat("%d", ammo), x0 + 20*scale, numY, 14*scale, RED);
        DrawTextShadowed(TextFormat("%d%%", dg->health), x0 + 70*scale, numY, 14*scale, RED);
        DrawTextShadowed(TextFormat("%d%%", dg->armor), x0 + 200*scale, numY, 14*scale, RED);
    }
}

// Where the mouse points on the level floors, in Doom map coords.
static bool MouseGroundPoint(const LevelMesh *lm, Camera3D cam, Vector2 *out)
{
    Ray ray = GetScreenToWorldRay(GetMousePosition(), cam);
    RayCollision rc = GetRayCollisionMesh(ray, lm->floors, MatrixIdentity());
    if (!rc.hit) return false;
    out->x = rc.point.x;
    out->y = -rc.point.z;
    return true;
}

// ---------------------------------------------------------------- main

int main(int argc, char **argv)
{
    Options opt;
    if (!ParseArgs(argc, argv, &opt)) {
        PrintUsage(stderr);
        return 2;
    }

    if (!opt.noLog && !StateLogOpen(NULL)) {
        fprintf(stderr, "helldrop: state log unavailable, continuing without\n");
    }
    LogStartup();

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(1600, 900, "HELLDROP");
    SetTargetFPS(60);
    // Left edge of the monitor so the terminal stays visible alongside.
    SetWindowPosition(8, (GetMonitorHeight(GetCurrentMonitor()) - 900)/2);

    Wad wad = { 0 };
    DoomMap map = { 0 };
    LevelMesh lm = { 0 };

    if (!WadLoad(&wad, opt.wadPath)) {
        TraceLog(LOG_ERROR, "MAIN: failed to load WAD '%s'", opt.wadPath);
        CloseWindow();
        StateLogClose();
        return 1;
    }
    if (!MapLoadFromWad(&map, &wad, opt.mapName)) {
        TraceLog(LOG_ERROR, "MAIN: failed to load map '%s' from '%s'",
                 opt.mapName, opt.wadPath);
        WadFree(&wad);
        CloseWindow();
        StateLogClose();
        return 1;
    }
    if (!LevelMeshBuild(&lm, &map)) {
        TraceLog(LOG_ERROR, "MAIN: failed to build level mesh for '%s'", opt.mapName);
        MapFree(&map);
        WadFree(&wad);
        CloseWindow();
        StateLogClose();
        return 1;
    }

    float *thingHeights = ComputeThingHeights(&map, &lm.floors);
    LogLevelLoaded(&opt, &map, &lm);

    Sim sim = { 0 };
    if (!SimInit(&sim, &map, &lm)) {
        TraceLog(LOG_ERROR, "MAIN: failed to init sim");
        free(thingHeights);
        LevelMeshFree(&lm);
        MapFree(&map);
        WadFree(&wad);
        CloseWindow();
        StateLogClose();
        return 1;
    }

    ViewState vs = { 0 };
    float extentX = map.maxX - map.minX;
    float extentY = map.maxY - map.minY;
    float extent = (extentX > extentY) ? extentX : extentY;
    vs.homeTarget = (Vector3){ (map.minX + map.maxX)*0.5f, 0.0f,
                               -(map.minY + map.maxY)*0.5f };
    vs.homeFovy = Clamp(fmaxf(extent*0.6f, 500.0f), FOVY_MIN, FOVY_MAX);
    ViewReset(&vs);

    // raylib's default far cull distance (4000) equals CAM_DIST, so the far
    // plane would slice through the target plane and clip half the map.
    // Widen the clip range to cover the whole level from any orbit angle.
    float diag = sqrtf(extentX*extentX + extentY*extentY);
    rlSetClipPlanes(1.0, CAM_DIST + 2.0*diag);

    DoomHud hud = { 0 };
    if (!DoomHudLoad(&hud, &wad)) {
        TraceLog(LOG_WARNING, "MAIN: no status-bar graphics in WAD, using fallback HUD");
    }
    FaceState faceState = { .prevHealth = 100, .hpRef = 100, .hpRefFrames = 35 };

    GameSprites gsprites = { 0 };
    if (!GameSpritesLoad(&gsprites, &wad)) {
        TraceLog(LOG_WARNING, "MAIN: no sprites in WAD, using shape renderer");
    }
    InitAudioDevice();
    GameAudio gaudio = { 0 };
    GameAudioLoad(&gaudio, &wad);

    long frame = 0;
    double simAcc = 0.0;
    MobKind selectedKind = MK_PINKY;
    char dropMsg[64] = "";
    int dropMsgFrames = 0;
    while (!WindowShouldClose()) {
        StateLogSetFrame(frame);
        float dt = GetFrameTime();
        CameraUpdate(&vs, dt);
        Camera3D cam = CameraFromView(&vs);

        if (IsKeyPressed(KEY_G)) {
            LogInput("sim_restart");
            SimFree(&sim);
            SimInit(&sim, &map, &lm);
            simAcc = 0.0;
        }
        if (IsKeyPressed(KEY_ONE))   selectedKind = MK_ZOMBIE;
        if (IsKeyPressed(KEY_TWO))   selectedKind = MK_SHOTGUNGUY;
        if (IsKeyPressed(KEY_THREE)) selectedKind = MK_IMP;
        if (IsKeyPressed(KEY_FOUR))  selectedKind = MK_PINKY;
        if (IsKeyPressed(KEY_FIVE))  selectedKind = MK_CACO;

        Vector2 ground = { 0 };
        bool groundOk = MouseGroundPoint(&lm, cam, &ground);
        if (groundOk && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            switch (SimTryDrop(&sim, selectedKind, ground.x, ground.y)) {
            case DROP_OK:
                break;
            case DROP_NO_SOULS:
                snprintf(dropMsg, sizeof(dropMsg), "NOT ENOUGH SOULS");
                dropMsgFrames = 80;
                break;
            case DROP_BAD_SPOT:
                snprintf(dropMsg, sizeof(dropMsg), "CAN'T DROP THERE");
                dropMsgFrames = 80;
                break;
            case DROP_FULL:
                snprintf(dropMsg, sizeof(dropMsg), "TOO MUCH HELL ALREADY");
                dropMsgFrames = 80;
                break;
            }
        }

        // Fixed 35 Hz sim: render owns wall time, feeds whole tics in.
        // The accumulator cap bounds catch-up work after hitches (and
        // bounds --turbo to ~17 tics per rendered frame).
        simAcc += (double)dt * (double)opt.turbo;
        if (simAcc > 0.5) simAcc = 0.5;
        while (simAcc >= SIM_TICTIME) {
            SimTick(&sim);
            // Dev smoke test: periodically drop a squad just ahead of him so
            // combat/infighting/drops exercise without a human at the mouse.
            if (opt.autodrop && sim.tic >= 250 && sim.tic % 250 == 0) {
                static const MobKind WAVE[4] = { MK_PINKY, MK_IMP, MK_CACO, MK_PINKY };
                float a = sim.dg.angleDeg*DEG2RAD;
                for (int w = 0; w < 4; w++) {
                    float side = (w % 2 == 0) ? 60.0f : -60.0f;
                    SimDrop(&sim, WAVE[w],
                            sim.dg.pos.x + cosf(a)*200.0f - sinf(a)*side,
                            sim.dg.pos.y + sinf(a)*200.0f + cosf(a)*side);
                }
            }
            simAcc -= SIM_TICTIME;
        }
        float simAlpha = (float)(simAcc / SIM_TICTIME);

        GameAudioDrain(&gaudio, &sim, (Vector2){ vs.target.x, -vs.target.z });

        BeginDrawing();
        ClearBackground((Color){ 12, 12, 14, 255 });
        BeginMode3D(cam);
        LevelMeshDraw(&lm);
        DrawDoors(&sim, simAlpha);
        DrawThings(&map, thingHeights);
        DrawRoute(&sim, simAlpha);
        // Billboards go through the alpha-test shader so they write depth
        // like opaque geometry: no sorting needed, no halo artifacts.
        if (gsprites.valid) BeginShaderMode(gsprites.alphaTest);
        DrawPickups(&sim, &gsprites, cam);
        DrawMobs(&sim, simAlpha, &gsprites, cam, vs.yawDeg);
        DrawProjectiles(&sim, simAlpha, &gsprites, cam);
        DrawDoomGuy(&sim, simAlpha, &gsprites, cam, vs.yawDeg);
        if (gsprites.valid) EndShaderMode();
        DrawTracers(&sim);
        DrawPendingDrops(&sim, frame);
        bool cursorHot = false;
        int cursorCost = 0;
        bool cursorAfford = false;
        if (groundOk) {
            bool sOk = false;
            float gz = SimFloorAtPoint(&sim, ground.x, ground.y, &sOk);
            if (sOk) {
                cursorCost = SimDropCost(&sim, selectedKind, ground.x, ground.y,
                                         &cursorHot);
                cursorAfford = sim.souls >= (float)cursorCost;
                Color ring = !cursorAfford ? (Color){ 255, 60, 50, 255 }
                           : cursorHot     ? (Color){ 255, 170, 60, 255 }
                                           : (Color){ 120, 255, 140, 255 };
                DrawCircle3D((Vector3){ ground.x, gz + 2.0f, -ground.y },
                             MobRadius(selectedKind) + 4.0f,
                             (Vector3){ 1.0f, 0.0f, 0.0f }, 90.0f, ring);
            }
        }
        EndMode3D();
        DrawHud(&map, PathBasename(opt.wadPath), &sim, opt.turbo);
        DrawSoulsPanel(&sim, selectedKind);
        FaceStateUpdate(&faceState, &sim);
        DrawStatusBar(&hud, &sim, &faceState, frame);
        if (groundOk && cursorCost > 0) {
            Vector2 mp = GetMousePosition();
            Color tcol = !cursorAfford ? (Color){ 255, 80, 70, 255 }
                       : cursorHot     ? (Color){ 255, 180, 80, 255 }
                                       : (Color){ 160, 255, 170, 255 };
            DrawTextShadowed(TextFormat("%d%s", cursorCost, cursorHot ? " HOT" : ""),
                             (int)mp.x + 18, (int)mp.y - 6, 18, tcol);
        }
        if (dropMsgFrames > 0) {
            dropMsgFrames--;
            int w = MeasureText(dropMsg, 24);
            DrawTextShadowed(dropMsg, GetScreenWidth()/2 - w/2,
                             GetScreenHeight() - 110, 24,
                             (Color){ 255, 80, 70, 255 });
        }
        EndDrawing();

        if (frame % 12 == 0) LogSnapshot(&vs, &sim);
        frame++;

        if (opt.selftest && frame >= 60) {
            TakeScreenshot("selftest.png");
            if (StateLogEnabled()) {
                StateLogBegin("selftest_done");
                StateLogStr("screenshot", "selftest.png");
                StateLogEnd();
            }
            break;
        }
        if (opt.exitAfter > 0 && frame >= opt.exitAfter) break;
    }

    LogShutdown(frame, sim.tic);
    StateLogClose();
    GameAudioUnload(&gaudio);
    CloseAudioDevice();
    GameSpritesUnload(&gsprites);
    DoomHudUnload(&hud);
    SimFree(&sim);
    free(thingHeights);
    LevelMeshFree(&lm);
    MapFree(&map);
    WadFree(&wad);
    CloseWindow();
    return 0;
}
