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
    bool wadExplicit;      // --wad given: skip DOOM.WAD auto-detection
    const char *mapName;
    bool mapExplicit;      // --map given: single-level mode, all unlocked
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
    opt->wadExplicit = false;
    opt->mapName = "E1M1";
    opt->mapExplicit = false;
    opt->noLog = false;
    opt->selftest = false;
    opt->autodrop = false;
    opt->exitAfter = 0;
    opt->turbo = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--wad") == 0 && i + 1 < argc) {
            opt->wadPath = argv[++i];
            opt->wadExplicit = true;
        } else if (strcmp(argv[i], "--map") == 0 && i + 1 < argc) {
            opt->mapName = argv[++i];
            opt->mapExplicit = true;
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

// If the user owns the real thing, use it: every lump we decode (sprites,
// sounds, mugshot, status bar) comes from the WAD, so pointing at an id
// IWAD makes the game look exactly like Doom. We only bundle Freedoom
// because id's assets cannot be redistributed. E1-campaign-compatible
// IWADs only (DOOM2.WAD lacks E1M1 — use it via --wad --map MAPxx).
static const char *AutoDetectWad(void)
{
    static char buf[512];
    static const char *LOCAL[] = {
        "assets/DOOM.WAD", "assets/doom.wad",
        "assets/DOOM1.WAD", "assets/doom1.wad",
    };
    for (size_t i = 0; i < sizeof(LOCAL)/sizeof(LOCAL[0]); i++) {
        if (FileExists(LOCAL[i])) return LOCAL[i];
    }
    const char *home = getenv("HOME");
    if (home) {
        static const char *STEAM[] = {
            "%s/Library/Application Support/Steam/steamapps/common/"
            "Ultimate Doom/base/DOOM.WAD",
            "%s/Library/Application Support/Steam/steamapps/common/"
            "Doom/base/DOOM.WAD",
            "%s/Library/Application Support/Steam/steamapps/common/"
            "DOOM + DOOM II/rerelease/DOOM.WAD",
        };
        for (size_t i = 0; i < sizeof(STEAM)/sizeof(STEAM[0]); i++) {
            snprintf(buf, sizeof(buf), STEAM[i], home);
            if (FileExists(buf)) return buf;
        }
    }
    return NULL;
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
                        Camera3D cam)
{
    Vector3 p = DgRenderPos(sim, alpha);
    const SpriteFrame *f = gs->valid ? DGSpriteFrame(gs, &sim->dg, sim->tic) : NULL;
    if (f) {
        int rot = SpriteRotationFrom(sim->dg.angleDeg,
                                     (Vector2){ cam.position.x, -cam.position.z },
                                     (Vector2){ p.x, -p.z });
        DrawSpriteBillboard(cam, f, rot, p, WHITE);
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
    [MK_SPECTRE]    = {  70,  70,  85, 255 },
    [MK_CACO]       = { 215,  70,  80, 255 },
    [MK_LOSTSOUL]   = { 235, 205, 130, 255 },
    [MK_REVENANT]   = { 205, 195, 175, 255 },
    [MK_BARON]      = { 200, 140, 190, 255 },
    [MK_ARCHVILE]   = { 250, 225, 110, 255 },
    [MK_CHAINGUNNER]   = { 180, 160,  80, 255 },
    [MK_HELLKNIGHT]    = { 170, 190, 130, 255 },
    [MK_MANCUBUS]      = { 165, 120,  90, 255 },
    [MK_PAINELEMENTAL] = { 160,  90, 110, 255 },
};

static Vector3 MobRenderPos(const Mob *m, float alpha)
{
    float x = m->prevPos.x + (m->pos.x - m->prevPos.x)*alpha;
    float y = m->prevPos.y + (m->pos.y - m->prevPos.y)*alpha;
    float z = m->prevZ + (m->z - m->prevZ)*alpha;
    return (Vector3){ x, z, -y };
}

static void DrawMobs(const Sim *sim, float alpha, const GameSprites *gs,
                     Camera3D cam)
{
    Vector2 viewer = { cam.position.x, -cam.position.z };
    for (int i = 0; i < sim->mobCount; i++) {
        const Mob *m = &sim->mobs[i];
        float r = MobRadius(m->kind);
        Vector3 p = MobRenderPos(m, alpha);
        if (m->state == MOB_DEAD) {
            // Blood pools out beneath the corpse over the first second.
            float age = (float)(sim->tic - m->deathTic);
            float pr = r*1.25f*fminf(1.0f, age/35.0f);
            if (pr > 1.0f) {
                DrawCylinder((Vector3){ p.x, p.y + 0.4f, p.z }, pr, pr, 0.4f,
                             12, (Color){ 88, 14, 14, 255 });
            }
        }
        const SpriteFrame *f = gs->valid ? MobSpriteFrame(gs, m, sim->tic) : NULL;
        if (f) {
            // Spectres render as a dark ghost of the pinky sprite.
            Color tint = (m->kind == MK_SPECTRE) ? (Color){ 38, 38, 50, 255 }
                                                 : WHITE;
            int rot = SpriteRotationFrom(m->angleDeg, viewer,
                                         (Vector2){ p.x, -p.z });
            DrawSpriteBillboard(cam, f, rot, p, tint);
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
        const SpriteFrame *f = gs->valid ? ProjSpriteFrame(gs, p, sim->tic) : NULL;
        if (f) {
            // The patch offsets center missiles on their origin (authentic).
            DrawSpriteBillboard(cam, f, 0, (Vector3){ x, z, -y }, WHITE);
        } else {
            Color c = p->isRocket ? (Color){ 220, 220, 220, 255 }
                    : (p->ownerKind == MK_CACO) ? (Color){ 255, 60, 60, 255 }
                                                : (Color){ 255, 140, 40, 255 };
            DrawSphere((Vector3){ x, z, -y }, 7.0f, c);
        }
    }
}

static void DrawBooms(const Sim *sim, const GameSprites *gs, Camera3D cam)
{
    for (int i = 0; i < BOOMS_MAX; i++) {
        const Boom *b = &sim->booms[i];
        long age = sim->tic - b->tic;
        if (b->tic == 0 || age < 0 || age > 11) continue;
        const SpriteFrame *f = gs->valid
                                   ? BoomSpriteFrame(gs, (BoomKind)b->kind, age)
                                   : NULL;
        if (f) {
            DrawSpriteBillboard(cam, f, 0,
                                (Vector3){ b->pos.x, b->z, -b->pos.y }, WHITE);
        } else {
            float r = 20.0f + (float)age*6.0f;
            unsigned char a = (unsigned char)(220 - age*18);
            DrawSphereWires((Vector3){ b->pos.x, b->z, -b->pos.y }, r, 8, 8,
                            (Color){ 255, 170, 60, a });
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

static void DrawBarrels(const Sim *sim, const GameSprites *gs, Camera3D cam)
{
    for (int i = 0; i < sim->barrelCount; i++) {
        const Barrel *b = &sim->barrels[i];
        if (!b->alive) continue;
        const SpriteFrame *f = gs->valid ? BarrelSpriteFrame(gs, sim->tic) : NULL;
        Vector3 base = { b->pos.x, b->z, -b->pos.y };
        if (f) {
            DrawSpriteBillboard(cam, f, 0, base, WHITE);
        } else {
            DrawCylinder(base, 11.0f, 9.0f, 34.0f, 10, (Color){ 90, 140, 90, 255 });
        }
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

// Lift platforms: the static mesh holds lift floors at their LOW position;
// draw the moving cap (the lift sector's own floor triangles, shifted up)
// plus dark side skirts along its boundary.
static void DrawLifts(const Sim *sim, const LevelMesh *lm, float alpha)
{
    const DoomMap *map = sim->map;
    for (int i = 0; i < sim->liftCount; i++) {
        const Lift *l = &sim->lifts[i];
        float floor = l->prevFloor + (l->floor - l->prevFloor)*alpha;
        if (floor - l->lowFloor < 0.5f) continue;   // resting on the slab
        const float *v = lm->floors.vertices;
        Color cap = { 130, 124, 112, 255 };
        for (int t = 0; t < lm->floors.triangleCount; t++) {
            if (lm->floorTriSector[t] != l->sector) continue;
            const float *a = &v[(size_t)t*9], *b = a + 3, *c = a + 6;
            DrawTriangle3D((Vector3){ a[0], floor, a[2] },
                           (Vector3){ b[0], floor, b[2] },
                           (Vector3){ c[0], floor, c[2] }, cap);
        }
        Color skirt = { 78, 74, 66, 255 };
        for (int j = 0; j < map->lineCount; j++) {
            const Linedef *ld = &map->lines[j];
            if (ld->sideFront < 0 || ld->sideBack < 0) continue;
            int fs = map->sides[ld->sideFront].sector;
            int bs = map->sides[ld->sideBack].sector;
            if (fs != l->sector && bs != l->sector) continue;
            Vector2 a = map->verts[ld->v1], b = map->verts[ld->v2];
            Vector3 a0 = { a.x, l->lowFloor, -a.y }, b0 = { b.x, l->lowFloor, -b.y };
            Vector3 a1 = { a.x, floor, -a.y },       b1 = { b.x, floor, -b.y };
            DrawTriangle3D(a0, b0, b1, skirt);
            DrawTriangle3D(a0, b1, a1, skirt);
            DrawTriangle3D(a0, b1, b0, skirt);
            DrawTriangle3D(a0, a1, b1, skirt);
        }
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
    StateLogStr("objective", (sim->dg.objective == OBJ_HEAL) ? "heal"
                : (sim->dg.objective == OBJ_WEAPON) ? "weapon" : "exit");
    StateLogInt("sector", sim->dg.sector);
    StateLogInt("health", sim->dg.health);
    StateLogInt("armor", sim->dg.armor);
    StateLogInt("bullets", sim->dg.bullets);
    StateLogInt("shells", sim->dg.shells);
    StateLogInt("rockets", sim->dg.rockets);
    StateLogStr("weapon", DGWeaponName(sim->dg.weapon));
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
    StateLogArrBegin("lifts");
    for (int i = 0; i < sim->liftCount; i++) {
        static const char *LIFT_STATES[] = { "up", "down", "low", "rising" };
        StateLogObjBegin(NULL);
        StateLogInt("sector", sim->lifts[i].sector);
        StateLogStr("state", LIFT_STATES[sim->lifts[i].state]);
        StateLogFloat("floor", sim->lifts[i].floor);
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

static void ViewReset(ViewState *vs);

// Frame the whole level and widen the clip range to cover it: raylib's
// default far cull distance (4000) equals CAM_DIST, so the far plane would
// otherwise slice through the target plane and clip half the map.
static void SetupView(ViewState *vs, const DoomMap *map)
{
    float extentX = map->maxX - map->minX;
    float extentY = map->maxY - map->minY;
    float extent = (extentX > extentY) ? extentX : extentY;
    memset(vs, 0, sizeof(*vs));
    vs->homeTarget = (Vector3){ (map->minX + map->maxX)*0.5f, 0.0f,
                                -(map->minY + map->maxY)*0.5f };
    vs->homeFovy = Clamp(fmaxf(extent*0.6f, 500.0f), FOVY_MIN, FOVY_MAX);
    ViewReset(vs);
    float diag = sqrtf(extentX*extentX + extentY*extentY);
    rlSetClipPlanes(1.0, CAM_DIST + 2.0*diag);
}

// ------------------------------------------------------------- screen melt

// The classic Doom wipe: the old frame hangs as columns that fall away,
// staggered and accelerating, revealing the new scene beneath.
#define MELT_COLS 160

typedef struct Melt {
    Texture2D tex;
    int y[MELT_COLS];
    bool active;
} Melt;

static void MeltStart(Melt *m)
{
    if (m->active) return;
    Image img = LoadImageFromScreen();
    if (m->tex.id != 0) UnloadTexture(m->tex);
    m->tex = LoadTextureFromImage(img);
    UnloadImage(img);
    int delay = 0;
    for (int i = 0; i < MELT_COLS; i++) {
        delay += GetRandomValue(-1, 1);
        if (delay < 0) delay = 0;
        if (delay > 15) delay = 15;
        m->y[i] = -delay*12;
    }
    m->active = true;
}

static void MeltDraw(Melt *m)
{
    if (!m->active) return;
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    float srcW = (float)m->tex.width/MELT_COLS;
    float dstW = (float)sw/MELT_COLS;
    bool done = true;
    for (int i = 0; i < MELT_COLS; i++) {
        m->y[i] += 16;
        int y = (m->y[i] < 0) ? 0 : m->y[i];
        if (y < sh) done = false;
        else continue;
        DrawTexturePro(m->tex,
                       (Rectangle){ i*srcW, 0, srcW, (float)m->tex.height },
                       (Rectangle){ i*dstW, (float)y, dstW, (float)sh },
                       (Vector2){ 0, 0 }, 0.0f, WHITE);
    }
    if (done) m->active = false;
}

// ------------------------------------------------------------- campaign

// "Knee-Deep in His Blood": E1M1..E1M8 from Hell's side, the bestiary
// unlocking in roughly Doom's own monster-introduction order. Killing him
// inside a secret sector on E1M3 routes through the hidden E1M9.
#define CAMPAIGN_SAVE "helldrop.sav"

static unsigned UnlockMaskFor(const char *mapName)
{
    unsigned m = (1u << MK_ZOMBIE) | (1u << MK_IMP);
    if (strcmp(mapName, "E1M1") == 0) return m;
    m |= (1u << MK_SHOTGUNGUY) | (1u << MK_PINKY);
    if (strcmp(mapName, "E1M2") == 0) return m;
    m |= (1u << MK_LOSTSOUL);
    if (strcmp(mapName, "E1M3") == 0 || strcmp(mapName, "E1M9") == 0) return m;
    m |= (1u << MK_SPECTRE) | (1u << MK_CACO);
    if (strcmp(mapName, "E1M4") == 0) return m;
    m |= (1u << MK_REVENANT);
    if (strcmp(mapName, "E1M5") == 0) return m;
    m |= (1u << MK_BARON);
    if (strcmp(mapName, "E1M6") == 0) return m;
    m |= (1u << MK_ARCHVILE);
    return m;   // E1M7, E1M8, unknown maps: everything
}

static const char *UnlockMapFor(MobKind k)
{
    switch (k) {
    case MK_SHOTGUNGUY: case MK_PINKY: return "E1M2";
    case MK_LOSTSOUL:                  return "E1M3";
    case MK_SPECTRE: case MK_CACO:     return "E1M4";
    case MK_REVENANT:                  return "E1M5";
    case MK_BARON:                     return "E1M6";
    case MK_ARCHVILE:                  return "E1M7";
    case MK_CHAINGUNNER: case MK_HELLKNIGHT:
    case MK_MANCUBUS: case MK_PAINELEMENTAL:
        return "DOOM2";   // outside the E1 campaign; --wad freedoom2.wad
    default:                           return "E1M1";
    }
}

static const char *NextCampaignMap(const char *cur, bool secretKill)
{
    if (strcmp(cur, "E1M1") == 0) return "E1M2";
    if (strcmp(cur, "E1M2") == 0) return "E1M3";
    if (strcmp(cur, "E1M3") == 0) return secretKill ? "E1M9" : "E1M4";
    if (strcmp(cur, "E1M9") == 0) return "E1M4";
    if (strcmp(cur, "E1M4") == 0) return "E1M5";
    if (strcmp(cur, "E1M5") == 0) return "E1M6";
    if (strcmp(cur, "E1M6") == 0) return "E1M7";
    if (strcmp(cur, "E1M7") == 0) return "E1M8";
    return NULL;   // E1M8 won: episode complete
}

static int CampaignNumber(const char *map)
{
    static const char *ORDER[] = { "E1M1", "E1M2", "E1M3", "E1M4",
                                   "E1M5", "E1M6", "E1M7", "E1M8" };
    for (int i = 0; i < 8; i++) {
        if (strcmp(map, ORDER[i]) == 0) return i + 1;
    }
    return 0;   // E1M9 and friends sit outside the numbering
}

static void CampaignSave(const char *map)
{
    FILE *f = fopen(CAMPAIGN_SAVE, "w");
    if (!f) return;
    fprintf(f, "%s\n", map);
    fclose(f);
}

static bool CampaignLoad(char *out, size_t cap)
{
    FILE *f = fopen(CAMPAIGN_SAVE, "r");
    if (!f) return false;
    char buf[16] = "";
    bool ok = (fscanf(f, "%8s", buf) == 1);
    fclose(f);
    if (!ok || strlen(buf) != 4 || buf[0] != 'E') return false;
    snprintf(out, cap, "%s", buf);
    return true;
}

// Tear down and rebuild everything level-scoped; the WAD and its decoded
// graphics/sounds persist across the whole episode.
static bool ReloadLevel(const Wad *wad, const char *mapName, DoomMap *map,
                        LevelMesh *lm, Sim *sim, float **thingHeights)
{
    SimFree(sim);
    free(*thingHeights);
    *thingHeights = NULL;
    LevelMeshFree(lm);
    MapFree(map);
    if (!MapLoadFromWad(map, wad, mapName)) return false;
    if (!LevelMeshBuild(lm, map)) return false;
    *thingHeights = ComputeThingHeights(map, &lm->floors);
    if (!SimInit(sim, map, lm)) return false;
    if (StateLogEnabled()) {
        StateLogBegin("campaign");
        StateLogStr("map", mapName);
        StateLogEnd();
    }
    return true;
}

// One full world pass, camera-agnostic: shared by the iso view and the
// doomcam. Player-facing overlays (route, drop cursor, telegraphs) are
// iso-only and drawn separately.
static void DrawWorld(const Sim *sim, const LevelMesh *lm, const DoomMap *map,
                      const float *thingHeights, const GameSprites *gs,
                      Camera3D cam, float alpha, bool includeDG)
{
    BeginMode3D(cam);
    LevelMeshDraw(lm);
    DrawLifts(sim, lm, alpha);
    DrawDoors(sim, alpha);
    DrawThings(map, thingHeights);
    if (gs->valid) BeginShaderMode(gs->alphaTest);
    DrawBarrels(sim, gs, cam);
    DrawPickups(sim, gs, cam);
    DrawMobs(sim, alpha, gs, cam);
    DrawProjectiles(sim, alpha, gs, cam);
    DrawBooms(sim, gs, cam);
    if (includeDG) DrawDoomGuy(sim, alpha, gs, cam);
    for (int i = 0; i < sim->mobCount; i++) {
        const Mob *m = &sim->mobs[i];
        if (m->kind != MK_ARCHVILE || m->state != MOB_ATTACK) continue;
        const SpriteFrame *ff = FireSpriteFrame(gs, sim->tic);
        if (!ff) break;
        Vector3 at = (m->targetMob < 0)
                         ? DgRenderPos(sim, alpha)
                         : MobRenderPos(&sim->mobs[m->targetMob], alpha);
        DrawSpriteBillboard(cam, ff, 0, at, WHITE);
    }
    if (gs->valid) EndShaderMode();
    DrawTracers(sim);
    EndMode3D();
}

// His eyes: perspective camera at eye height, dropping to the floor over
// the first second of his death, vanilla-style.
static Camera3D DoomcamFrom(const Sim *sim, float alpha)
{
    Vector3 eye = DgRenderPos(sim, alpha);
    float a = sim->dg.angleDeg*DEG2RAD;
    float eyeH = 41.0f;
    if (sim->dg.state == DG_DEAD) {
        long dt = sim->tic - sim->dg.deathTic;
        float drop = (dt > 35) ? 1.0f : (float)dt/35.0f;
        eyeH = 41.0f - 33.0f*drop;
    }
    Camera3D fp = { 0 };
    fp.position = (Vector3){ eye.x, eye.y + eyeH, eye.z };
    fp.target = (Vector3){ eye.x + cosf(a)*64.0f, eye.y + eyeH,
                           eye.z - sinf(a)*64.0f };
    fp.up = (Vector3){ 0.0f, 1.0f, 0.0f };
    fp.fovy = 73.0f;
    fp.projection = CAMERA_PERSPECTIVE;
    return fp;
}

// ------------------------------------------------------- intent + codex

// In-the-moment intent telegraphy: what is he doing RIGHT NOW. Loop Hero's
// lesson — players never infer internal state from movement alone.
static const char *DgIntentLabel(const Sim *sim)
{
    const DoomGuy *dg = &sim->dg;
    switch (dg->state) {
    case DG_FIGHT:
        if (dg->targetMob >= 0) {
            return TextFormat("FIGHTING %s",
                              MobKindName(sim->mobs[dg->targetMob].kind));
        }
        return "FIGHTING";
    case DG_RETREAT:   return "-> MEDKIT";
    case DG_WAIT_DOOR: return "WAITING: DOOR";
    case DG_WAIT_LIFT: return "WAITING: LIFT";
    case DG_ADVANCE:
        if (dg->objective == OBJ_WEAPON && dg->weaponPickup >= 0) {
            switch (sim->pickups[dg->weaponPickup].type) {
            case PK_SHOTGUN:  return "-> SHOTGUN";
            case PK_CHAINGUN: return "-> CHAINGUN";
            case PK_RLAUNCH:  return "-> ROCKET LAUNCHER";
            default: break;
            }
        }
        return "-> EXIT";
    default:
        return NULL;
    }
}

static void DrawIntent(const Sim *sim, Camera3D cam, float alpha,
                       const char *label, int flashFrames)
{
    if (!label) return;
    Vector3 p = DgRenderPos(sim, alpha);
    p.y += 86.0f;
    Vector2 sp = GetWorldToScreen(p, cam);
    if (sp.x < -200 || sp.x > GetScreenWidth() + 200 ||
        sp.y < -50 || sp.y > GetScreenHeight() + 50) return;
    int size = (flashFrames > 0) ? 22 : 16;
    Color c = (flashFrames > 0) ? (Color){ 255, 240, 120, 255 }
                                : (Color){ 190, 255, 205, 230 };
    int w = MeasureText(label, size);
    DrawRectangle((int)sp.x - w/2 - 5, (int)sp.y - 3, w + 10, size + 6,
                  (Color){ 0, 0, 0, 140 });
    DrawTextShadowed(label, (int)sp.x - w/2, (int)sp.y, size, c);
}

// The Rule-of-30 codex: his published, absolutely-true tells. Every line
// here is a real implemented behavior — predictability IS the puzzle.
static void DrawCodex(void)
{
    static const char *RULES[] = {
        "Below 40% health he ALWAYS breaks for the nearest medkit.",
        "He routes for every weapon he doesn't own before the exit.",
        "He presses every switch he passes, crosses every trigger.",
        "The arch-vile ALWAYS jumps to the top of his kill list.",
        "He never fires rockets inside his own blast radius --",
        "  anything hugging him switches his best weapon off.",
        "Rockets gib corpses: he can burn your arch-vile's savings.",
        "Monsters busy infighting are spared -- he lets them work.",
        "A far pinky is beneath his notice; a close one is an emergency.",
        "In sectors darker than 120 he spots monsters at HALF range.",
        "Dead zombies feed him: their clips are his resupply.",
    };
    int n = (int)(sizeof(RULES)/sizeof(RULES[0]));
    int w = 560, h = 64 + n*24;
    int x = 24, y = GetScreenHeight()/2 - h/2;
    DrawRectangle(x, y, w, h, (Color){ 8, 8, 10, 225 });
    DrawRectangleLines(x, y, w, h, (Color){ 200, 60, 50, 255 });
    DrawTextShadowed("KNOW YOUR ENEMY", x + 16, y + 12, 26,
                     (Color){ 255, 80, 60, 255 });
    DrawTextShadowed("his instincts are law - exploit them", x + 16, y + 42,
                     14, GRAY);
    for (int i = 0; i < n; i++) {
        DrawTextShadowed(RULES[i], x + 16, y + 64 + i*24, 15,
                         (Color){ 220, 220, 210, 255 });
    }
}

// Vanilla episode 1 par times, the intermission's report-card yardstick.
static int ParSeconds(const char *mapName)
{
    static const struct { const char *name; int par; } PARS[] = {
        { "E1M1", 30 }, { "E1M2", 75 }, { "E1M3", 120 }, { "E1M4", 90 },
        { "E1M5", 165 }, { "E1M6", 180 }, { "E1M7", 180 }, { "E1M8", 30 },
        { "E1M9", 165 },
    };
    for (size_t i = 0; i < sizeof(PARS)/sizeof(PARS[0]); i++) {
        if (strcmp(PARS[i].name, mapName) == 0) return PARS[i].par;
    }
    return 0;
}

// The intermission screen, from the demons' side of the report card.
static void DrawIntermission(const DoomMap *map, const Sim *sim,
                             const char *winHint)
{
    bool won = (sim->dg.state == DG_DEAD);
    int cx = GetScreenWidth()/2, cy = GetScreenHeight()/2;
    DrawRectangle(0, cy - 130, GetScreenWidth(), 270, (Color){ 0, 0, 0, 195 });

    const char *title = won ? "DOOM GUY IS DEAD - YOU WIN"
                            : "HE REACHED THE EXIT - YOU LOSE";
    Color tcol = won ? (Color){ 90, 255, 110, 255 } : (Color){ 255, 60, 50, 255 };
    DrawTextShadowed(title, cx - MeasureText(title, 40)/2, cy - 116, 40, tcol);

    int secs = (int)(sim->tic/SIM_TICRATE);
    int par = ParSeconds(map->name);
    int total = 0;
    for (int i = 0; i < sim->mobCount; i++) {
        total += sim->mobs[i].playerDropped ? 0 : 1;
    }
    int pct = (total > 0) ? (int)(sim->kills*100/total) : 0;
    const char *lines[5];
    lines[0] = (par > 0)
        ? TextFormat("TIME %d:%02d   (PAR %d:%02d%s)", secs/60, secs%60,
                     par/60, par%60,
                     (secs > par) ? TextFormat("  +%ds over", secs - par) : "")
        : TextFormat("TIME %d:%02d", secs/60, secs%60);
    lines[1] = TextFormat("HIS KILLS  %ld  (%d%% of the level's demons)",
                          sim->kills, pct);
    lines[2] = TextFormat("HIS BLOOD  %ld damage taken", sim->dgDamageTaken);
    lines[3] = TextFormat("YOUR HAND  %d drops   %ld souls spent",
                          sim->dropsMade, sim->soulsSpent);
    lines[4] = won ? "LEVEL COMPLETION: 0%   rip and torn"
                   : "press G for the Performance Improvement Plan";
    int y = cy - 56;
    for (int i = 0; i < 5; i++) {
        Color c = (i == 4) ? (Color){ 255, 200, 120, 255 } : LIGHTGRAY;
        DrawTextShadowed(lines[i], cx - MeasureText(lines[i], 20)/2, y, 20, c);
        y += 28;
    }
    const char *hint = (won && winHint) ? winHint : "G restart   ESC quit";
    Color hc = (won && winHint) ? (Color){ 120, 255, 150, 255 } : GRAY;
    DrawTextShadowed(hint, cx - MeasureText(hint, 16 + (winHint ? 6 : 0))/2,
                     cy + 104, 16 + (winHint ? 6 : 0), hc);
}

static void DrawCampaignComplete(void)
{
    int cx = GetScreenWidth()/2, cy = GetScreenHeight()/2;
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(),
                  (Color){ 5, 0, 0, 215 });
    const char *l1 = "KNEE-DEEP IN HIS BLOOD";
    const char *l2 = "EPISODE COMPLETE - THE INVASION HOLDS";
    const char *l3 = "he never made it past the hangar district";
    const char *l4 = "SPACE to invade again";
    DrawTextShadowed(l1, cx - MeasureText(l1, 44)/2, cy - 80, 44,
                     (Color){ 255, 70, 50, 255 });
    DrawTextShadowed(l2, cx - MeasureText(l2, 24)/2, cy - 24, 24,
                     (Color){ 90, 255, 110, 255 });
    DrawTextShadowed(l3, cx - MeasureText(l3, 18)/2, cy + 14, 18, LIGHTGRAY);
    DrawTextShadowed(l4, cx - MeasureText(l4, 20)/2, cy + 60, 20, RAYWHITE);
}

static void DrawHud(const DoomMap *map, const char *wadBase, const Sim *sim,
                    long turbo, const char *campLabel, const char *winHint)
{
    int y = 12;
    DrawTextShadowed(map->name, 12, y, 24, RAYWHITE);
    if (campLabel) {
        DrawTextShadowed(campLabel, 12 + MeasureText(map->name, 24) + 16, y + 6,
                         16, (Color){ 255, 120, 90, 255 });
    }
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
    DrawTextShadowed(TextFormat("%s  bullets %d  shells %d  rockets %d  |  kills %ld  monsters %d",
                                DGWeaponName(sim->dg.weapon),
                                sim->dg.bullets, sim->dg.shells, sim->dg.rockets,
                                sim->kills, alive),
                     12, y, 16, (Color){ 255, 220, 130, 255 });

    y += 20;
    DrawTextShadowed("WASD pan  RMB drag  wheel zoom  Q/E rotate  Z/X tilt  "
                     "R view  TAB codex  C doomcam  G restart  ESC quit",
                     12, y, 14, GRAY);
    y += 18;
    DrawTextShadowed(TextFormat("HELL'S HAND:  F slam door %d / recall lift %d"
                                "   V detonate barrel %d",
                                VERB_DOOR_COST, VERB_LIFT_COST, VERB_BARREL_COST),
                     12, y, 14, (Color){ 230, 140, 90, 255 });

    if (sim->dg.state == DG_EXITED || sim->dg.state == DG_DEAD) {
        DrawIntermission(map, sim, winHint);
    } else if (sim->dg.state == DG_NO_GOAL) {
        const char *line1 = "NO ROUTE TO EXIT";
        int w1 = MeasureText(line1, 28);
        DrawTextShadowed(line1, GetScreenWidth()/2 - w1/2, 60, 28,
                         (Color){ 255, 170, 60, 255 });
    }
}

// Top-right: the soul economy at a glance — balance, drip rate, the priced
// bestiary with the selected monster highlighted, unaffordables dimmed,
// and campaign-locked entries showing their unlock level.
static void DrawSoulsPanel(const Sim *sim, MobKind selected, unsigned unlocked)
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
    // Display keys must match the actual bindings, not enum order.
    static const char *KEY_LABELS[MK_COUNT] = {
        [MK_ZOMBIE] = "1", [MK_SHOTGUNGUY] = "2", [MK_IMP] = "3",
        [MK_PINKY] = "4", [MK_SPECTRE] = "0", [MK_CACO] = "5",
        [MK_LOSTSOUL] = "6", [MK_REVENANT] = "7", [MK_BARON] = "8",
        [MK_ARCHVILE] = "9", [MK_CHAINGUNNER] = "s1", [MK_HELLKNIGHT] = "s2",
        [MK_MANCUBUS] = "s3", [MK_PAINELEMENTAL] = "s4",
    };
    for (int k = 0; k < MK_COUNT; k++) {
        int cost = SimDropBaseCost((MobKind)k);
        bool isUnlocked = (unlocked & (1u << k)) != 0;
        const char *label = isUnlocked
            ? TextFormat("[%s] %-13s %3d", KEY_LABELS[k],
                         MobKindName((MobKind)k), cost)
            : TextFormat("[%s] %-13s %s", KEY_LABELS[k],
                         MobKindName((MobKind)k), UnlockMapFor((MobKind)k));
        Color c = MOB_COLORS[k];
        bool affordable = sim->souls >= (float)cost;
        if (!isUnlocked) c = (Color){ 95, 85, 85, 255 };
        else if (!affordable) c = (Color){ 120, 70, 70, 255 };
        else if ((MobKind)k != selected) {
            c.r = (unsigned char)(c.r/2 + 50);
            c.g = (unsigned char)(c.g/2 + 50);
            c.b = (unsigned char)(c.b/2 + 50);
        }
        w = MeasureText(label, 18);
        DrawTextShadowed(label, sw - w - 16, y, 18, c);
        if ((MobKind)k == selected && isUnlocked) {
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

    // Single player always covers the bar's deathmatch FRAG box with the
    // ARMS panel; slot digits go yellow as he loots the arsenal ladder.
    if (hud->hasArms) {
        DrawTextureEx(hud->arms, (Vector2){ (float)(x0 + 104*scale), (float)y0 },
                      0.0f, (float)scale, WHITE);
        if (hud->hasGnum && hud->hasYnum) {
            bool owned[6] = { true, dg->hasShotgun, dg->hasChaingun,
                              dg->hasRocketLauncher, false, false };
            for (int i = 0; i < 6; i++) {
                const Texture2D *t = owned[i] ? &hud->ynum[i] : &hud->gnum[i];
                DrawTextureEx(*t, (Vector2){ (float)(x0 + (111 + (i%3)*12)*scale),
                                             (float)(y0 + (4 + (i/3)*10)*scale) },
                              0.0f, (float)scale, WHITE);
            }
        }
    }

    int tier = (100 - dg->health)*4/100;
    if (tier < 0) tier = 0;
    if (tier > 4) tier = 4;

    const HudPatch *face = NULL;
    if (dg->state == DG_DEAD && hud->hasDead) face = &hud->faceDead;
    else if (fs->ouchFrames > 0 && hud->hasOuch) face = &hud->faceOuch[tier];
    else if (fs->grinFrames > 0 && hud->hasGrin) face = &hud->faceGrin[tier];
    else if (hud->hasFaces) face = &hud->face[tier][(frame/24) % 3];
    if (face) {
        // Vanilla anchor: V_DrawPatch(ST_FACESX=143, bar top) minus the
        // patch's own offsets — face lumps carry -5,-2, landing the mug
        // centered in the bar's face slot.
        DrawTextureEx(face->tex,
                      (Vector2){ (float)(x0 + (143 - face->ox)*scale),
                                 (float)(y0 - face->oy*scale) },
                      0.0f, (float)scale, WHITE);
    }

    int ammo = (dg->weapon == WPN_SHOTGUN) ? dg->shells
             : (dg->weapon == WPN_ROCKET)  ? dg->rockets : dg->bullets;
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

    // WAD precedence when the user didn't pass --wad:
    //   1. an id IWAD, if present -> the exact Doom look (their own copy)
    //   2. assets/helldrop.wad    -> our shippable Freedoom-based pack
    //   3. assets/freedoom1.wad   -> the raw bundled fallback (the default)
    // Doom 2 map names (MAPxx) need a Doom 2 IWAD; the E1 campaign needs a
    // Doom 1 IWAD (DOOM2.WAD has no E1M1), so helldrop.wad (E1 maps) applies
    // only to the Doom 1 side.
    if (!opt.wadExplicit) {
        const char *real = NULL;
        if (strncmp(opt.mapName, "MAP", 3) == 0) {
            if (FileExists("assets/DOOM2.WAD")) real = "assets/DOOM2.WAD";
            else if (FileExists("assets/doom2.wad")) real = "assets/doom2.wad";
            else if (FileExists("assets/freedoom2.wad")) real = "assets/freedoom2.wad";
        } else {
            real = AutoDetectWad();
            if (!real && FileExists("assets/helldrop.wad")) real = "assets/helldrop.wad";
        }
        if (real) {
            opt.wadPath = real;
            TraceLog(LOG_INFO, "MAIN: auto-selected WAD: %s", real);
        }
    }

    // Campaign mode unless --map pins a single level; progress persists in
    // a one-line save file beside the binary.
    bool campaign = !opt.mapExplicit;
    char curMap[9];
    snprintf(curMap, sizeof(curMap), "%s", opt.mapName);
    if (campaign) CampaignLoad(curMap, sizeof(curMap));

    if (!WadLoad(&wad, opt.wadPath)) {
        TraceLog(LOG_ERROR, "MAIN: failed to load WAD '%s'", opt.wadPath);
        CloseWindow();
        StateLogClose();
        return 1;
    }
    if (!MapLoadFromWad(&map, &wad, curMap)) {
        TraceLog(LOG_ERROR, "MAIN: failed to load map '%s' from '%s'",
                 curMap, opt.wadPath);
        WadFree(&wad);
        CloseWindow();
        StateLogClose();
        return 1;
    }
    if (!LevelMeshBuild(&lm, &map)) {
        TraceLog(LOG_ERROR, "MAIN: failed to build level mesh for '%s'", curMap);
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
    SetupView(&vs, &map);

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
    bool codexOpen = opt.selftest;   // selftest screenshots include it
    char intentPrev[64] = "";
    int intentFlash = 0;
    bool doomcamOn = true;
    RenderTexture2D doomcamRT = LoadRenderTexture(320, 200);
    SetTextureFilter(doomcamRT.texture, TEXTURE_FILTER_POINT);
    bool campaignComplete = false;
    unsigned unlocked = campaign ? UnlockMaskFor(curMap) : 0xFFFFFFFFu;
    if (!(unlocked & (1u << selectedKind))) selectedKind = MK_ZOMBIE;
    Melt melt = { 0 };
    SetWindowTitle(TextFormat("HELLDROP - %s", curMap));
    while (!WindowShouldClose()) {
        StateLogSetFrame(frame);
        float dt = GetFrameTime();
        CameraUpdate(&vs, dt);
        Camera3D cam = CameraFromView(&vs);

        if (IsKeyPressed(KEY_G)) {
            LogInput("sim_restart");
            MeltStart(&melt);
            SimFree(&sim);
            SimInit(&sim, &map, &lm);
            simAcc = 0.0;
        }
        int wantKind = -1;
        bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        if (IsKeyPressed(KEY_ONE))   wantKind = shift ? MK_CHAINGUNNER : MK_ZOMBIE;
        if (IsKeyPressed(KEY_TWO))   wantKind = shift ? MK_HELLKNIGHT : MK_SHOTGUNGUY;
        if (IsKeyPressed(KEY_THREE)) wantKind = shift ? MK_MANCUBUS : MK_IMP;
        if (IsKeyPressed(KEY_FOUR))  wantKind = shift ? MK_PAINELEMENTAL : MK_PINKY;
        if (IsKeyPressed(KEY_FIVE))  wantKind = MK_CACO;
        if (IsKeyPressed(KEY_SIX))   wantKind = MK_LOSTSOUL;
        if (IsKeyPressed(KEY_SEVEN)) wantKind = MK_REVENANT;
        if (IsKeyPressed(KEY_EIGHT)) wantKind = MK_BARON;
        if (IsKeyPressed(KEY_NINE))  wantKind = MK_ARCHVILE;
        if (IsKeyPressed(KEY_ZERO))  wantKind = MK_SPECTRE;
        if (wantKind >= 0) {
            if (unlocked & (1u << wantKind)) {
                selectedKind = (MobKind)wantKind;
            } else {
                snprintf(dropMsg, sizeof(dropMsg), "%s ARRIVES ON %s",
                         MobKindName((MobKind)wantKind),
                         UnlockMapFor((MobKind)wantKind));
                dropMsgFrames = 80;
            }
        }

        // Campaign advance: a win and a SPACE move the invasion forward.
        if (campaign && IsKeyPressed(KEY_SPACE)) {
            const char *next = NULL;
            bool advance = false;
            if (campaignComplete) {
                next = "E1M1";
                campaignComplete = false;
                advance = true;
            } else if (sim.dg.state == DG_DEAD) {
                bool secret = (strcmp(map.name, "E1M3") == 0 &&
                               sim.dg.sector >= 0 &&
                               map.sectors[sim.dg.sector].special == 9);
                next = NextCampaignMap(curMap, secret);
                if (!next) campaignComplete = true;
                else advance = true;
            }
            if (advance) {
                MeltStart(&melt);
                snprintf(curMap, sizeof(curMap), "%s", next);
                CampaignSave(curMap);
                if (!ReloadLevel(&wad, curMap, &map, &lm, &sim, &thingHeights)) {
                    TraceLog(LOG_ERROR, "MAIN: failed to load '%s'", curMap);
                    break;
                }
                SetupView(&vs, &map);
                unlocked = UnlockMaskFor(curMap);
                if (!(unlocked & (1u << selectedKind))) selectedKind = MK_ZOMBIE;
                simAcc = 0.0;
                intentPrev[0] = '\0';
                SetWindowTitle(TextFormat("HELLDROP - %s", curMap));
                continue;
            }
        }
        if (IsKeyPressed(KEY_TAB)) {
            codexOpen = !codexOpen;
            LogInput(codexOpen ? "codex_open" : "codex_close");
        }
        if (IsKeyPressed(KEY_C)) {
            doomcamOn = !doomcamOn;
            LogInput(doomcamOn ? "doomcam_on" : "doomcam_off");
        }

        Vector2 ground = { 0 };
        bool groundOk = MouseGroundPoint(&lm, cam, &ground);
        if (groundOk && IsKeyPressed(KEY_F)) {
            int cost = 0;
            switch (SimSlamMover(&sim, ground.x, ground.y, &cost)) {
            case VERB_OK: break;
            case VERB_NO_SOULS:
                snprintf(dropMsg, sizeof(dropMsg), "NOT ENOUGH SOULS (%d)", cost);
                dropMsgFrames = 80;
                break;
            case VERB_NO_TARGET:
                snprintf(dropMsg, sizeof(dropMsg), "NO DOOR OR LIFT THERE");
                dropMsgFrames = 80;
                break;
            case VERB_COOLDOWN:
                snprintf(dropMsg, sizeof(dropMsg), "HELL'S HAND IS RESTING");
                dropMsgFrames = 80;
                break;
            }
        }
        if (groundOk && IsKeyPressed(KEY_V)) {
            switch (SimDetonateBarrel(&sim, ground.x, ground.y)) {
            case VERB_OK: break;
            case VERB_NO_SOULS:
                snprintf(dropMsg, sizeof(dropMsg), "NOT ENOUGH SOULS (%d)",
                         VERB_BARREL_COST);
                dropMsgFrames = 80;
                break;
            case VERB_NO_TARGET:
                snprintf(dropMsg, sizeof(dropMsg), "NO BARREL THERE");
                dropMsgFrames = 80;
                break;
            case VERB_COOLDOWN:
                snprintf(dropMsg, sizeof(dropMsg), "HELL'S HAND IS RESTING");
                dropMsgFrames = 80;
                break;
            }
        }
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
            if (opt.autodrop && sim.tic >= 400 && sim.tic % 400 == 0) {
                // Exercise Hell's Hand: "click" the nearest barrel and the
                // nearest door sector to him (cooldowns self-gate).
                int nb = -1;
                float nbd = 1e9f;
                for (int b = 0; b < sim.barrelCount; b++) {
                    if (!sim.barrels[b].alive) continue;
                    float dx = sim.barrels[b].pos.x - sim.dg.pos.x;
                    float dy = sim.barrels[b].pos.y - sim.dg.pos.y;
                    if (dx*dx + dy*dy < nbd) { nbd = dx*dx + dy*dy; nb = b; }
                }
                if (nb >= 0) {
                    SimDetonateBarrel(&sim, sim.barrels[nb].pos.x,
                                      sim.barrels[nb].pos.y);
                }
                SimSlamMover(&sim, sim.dg.pos.x, sim.dg.pos.y, NULL);
            }
            if (opt.autodrop && sim.tic >= 250 && sim.tic % 250 == 0) {
                // Odd waves bring an arch-vile to bank the previous wave's
                // corpses — the smoke test for resurrection.
                static const MobKind WAVE_A[4] = { MK_PINKY, MK_REVENANT, MK_HELLKNIGHT, MK_CHAINGUNNER };
                static const MobKind WAVE_B[4] = { MK_ARCHVILE, MK_MANCUBUS, MK_PAINELEMENTAL, MK_PINKY };
                const MobKind *wave = ((sim.tic/250) % 2 == 0) ? WAVE_A : WAVE_B;
                float a = sim.dg.angleDeg*DEG2RAD;
                for (int w = 0; w < 4; w++) {
                    float side = (w % 2 == 0) ? 60.0f : -60.0f;
                    SimDrop(&sim, wave[w],
                            sim.dg.pos.x + cosf(a)*200.0f - sinf(a)*side,
                            sim.dg.pos.y + sinf(a)*200.0f + cosf(a)*side);
                }
            }
            simAcc -= SIM_TICTIME;
        }
        float simAlpha = (float)(simAcc / SIM_TICTIME);

        GameAudioDrain(&gaudio, &sim, (Vector2){ vs.target.x, -vs.target.z });

        // Doomcam pass: his view into a 320x200 target (drawn as PiP, or
        // fullscreen for the death takeover).
        long deathAge = (sim.dg.state == DG_DEAD) ? (sim.tic - sim.dg.deathTic) : -1;
        bool takeover = (deathAge >= 0 && deathAge < 140);
        if (doomcamOn || takeover) {
            Camera3D fp = DoomcamFrom(&sim, simAlpha);
            BeginTextureMode(doomcamRT);
            ClearBackground((Color){ 10, 10, 12, 255 });
            DrawWorld(&sim, &lm, &map, thingHeights, &gsprites, fp, simAlpha, false);
            EndTextureMode();
        }

        BeginDrawing();
        ClearBackground((Color){ 12, 12, 14, 255 });
        DrawWorld(&sim, &lm, &map, thingHeights, &gsprites, cam, simAlpha, true);
        BeginMode3D(cam);
        DrawRoute(&sim, simAlpha);
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

        if (takeover) {
            // His last moments, fullscreen, fading to Doom's death red.
            DrawTexturePro(doomcamRT.texture,
                           (Rectangle){ 0, 0, 320, -200 },
                           (Rectangle){ 0, 0, (float)GetScreenWidth(),
                                        (float)GetScreenHeight() },
                           (Vector2){ 0, 0 }, 0.0f, WHITE);
            float fade = (deathAge > 70) ? 1.0f : (float)deathAge/70.0f;
            DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(),
                          (Color){ 165, 10, 10, (unsigned char)(fade*120.0f) });
        } else if (doomcamOn && sim.dg.state != DG_EXITED) {
            int px = 12, py = GetScreenHeight() - 212;
            DrawTexturePro(doomcamRT.texture,
                           (Rectangle){ 0, 0, 320, -200 },
                           (Rectangle){ (float)px, (float)py, 320, 200 },
                           (Vector2){ 0, 0 }, 0.0f, WHITE);
            DrawRectangleLines(px - 1, py - 1, 322, 202, (Color){ 90, 90, 96, 255 });
            DrawTextShadowed("DOOMCAM", px + 6, py + 4, 14, (Color){ 200, 60, 50, 255 });
        }
        const char *intent = DgIntentLabel(&sim);
        if (intent && strncmp(intent, intentPrev, sizeof(intentPrev) - 1) != 0) {
            snprintf(intentPrev, sizeof(intentPrev), "%s", intent);
            intentFlash = 30;
        }
        if (intentFlash > 0) intentFlash--;
        DrawIntent(&sim, cam, simAlpha, intent, intentFlash);
        const char *campLabel = NULL;
        if (campaign) {
            int num = CampaignNumber(curMap);
            campLabel = (num > 0)
                ? TextFormat("KNEE-DEEP IN HIS BLOOD - MAP %d/8", num)
                : "KNEE-DEEP IN HIS BLOOD - SECRET LEVEL";
        }
        const char *winHint = (campaign && !campaignComplete)
                                  ? "SPACE: the invasion advances" : NULL;
        DrawHud(&map, PathBasename(opt.wadPath), &sim, opt.turbo,
                campLabel, winHint);
        DrawSoulsPanel(&sim, selectedKind, unlocked);
        if (codexOpen) DrawCodex();
        if (campaignComplete) DrawCampaignComplete();
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
        MeltDraw(&melt);
        EndDrawing();

        if (frame % 12 == 0) LogSnapshot(&vs, &sim);
        frame++;

        if (opt.selftest && frame >= ((opt.exitAfter > 0) ? opt.exitAfter : 60)) {
            TakeScreenshot("selftest.png");
            if (StateLogEnabled()) {
                StateLogBegin("selftest_done");
                StateLogStr("screenshot", "selftest.png");
                StateLogEnd();
            }
            break;
        }
        if (!opt.selftest && opt.exitAfter > 0 && frame >= opt.exitAfter) break;
    }

    LogShutdown(frame, sim.tic);
    StateLogClose();
    if (melt.tex.id != 0) UnloadTexture(melt.tex);
    UnloadRenderTexture(doomcamRT);
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
