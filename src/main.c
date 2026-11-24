// Helldrop entry point: isometric Doom-level viewer loop.

#include <errno.h>
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
#include "models.h"
#define KIT_BACKGROUND_IMPLEMENTATION
#include "kit_background.h"

#if defined(PLATFORM_WEB) || defined(PLATFORM_IOS)
#define HELLDROP_TOUCH
#include "touch_host.h"
#endif
#ifdef PLATFORM_WEB
#include <emscripten/emscripten.h>
#define TOUCH_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define TOUCH_EXPORT
#endif

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
    const char *dumpDir;   // --dump-frames <dir>: PNG per frame (media capture)
    long dumpEvery;        // --dump-every <N>: dump cadence in frames
    const char *dumpVideo; // --dump-video <out.mp4>: stream frames into ffmpeg (35 fps, 1080p)
    long simOnly;          // --simonly <tics>: headless match, prints outcome JSON
    bool viewer;           // --viewer: model gallery (dev mode), screenshot with --selftest
    int winW, winH;        // --size WxH (trailer capture at 1920x1080)
    uint32_t seed;
    int garrisonSkill;     // --garrison-skill <1|2|4>: map-thing skill bit (balance)
    int garrisonDmg;       // --garrison-dmg <pct>: native monster damage (balance)
} Options;

static GameModels gmodels;     // Blender-built 3D models (M toggles sprites)
static float SectorLight01(const Sim *sim, int sector);
static Vector3 DgRenderPos(const Sim *sim, float alpha);
static Vector3 MobRenderPos(const Mob *m, float alpha);

// ---- Dynamic lights: fireballs, rockets, blasts and muzzle flashes light
// the level and the models around them (render-only, from sim state).
#define MAX_LIGHTS 24
#ifdef HELLDROP_TOUCH
#define LVL_GLSL "#version 300 es\nprecision highp float;\n"
#else
#define LVL_GLSL "#version 330\n"
#endif
static const char *LEVEL_VS = LVL_GLSL
    "in vec3 vertexPosition; in vec2 vertexTexCoord; in vec4 vertexColor;\n"
    "uniform mat4 mvp; uniform mat4 matModel;\n"
    "out vec2 fragTexCoord; out vec4 fragColor; out vec3 fragPos;\n"
    "void main() {\n"
    "    fragTexCoord = vertexTexCoord; fragColor = vertexColor;\n"
    "    fragPos = (matModel*vec4(vertexPosition, 1.0)).xyz;\n"
    "    gl_Position = mvp*vec4(vertexPosition, 1.0);\n"
    "}\n";
static const char *LEVEL_FS = LVL_GLSL
    "in vec2 fragTexCoord; in vec4 fragColor; in vec3 fragPos;\n"
    "uniform sampler2D texture0; uniform vec4 colDiffuse;\n"
    "uniform int lightCount; uniform vec4 lightPos[24]; uniform vec3 lightCol[24];\n"
    "out vec4 finalColor;\n"
    "void main() {\n"
    "    vec3 tex = texture(texture0, fragTexCoord).rgb*colDiffuse.rgb;\n"
    "    vec3 add = vec3(0.0);\n"
    "    for (int i = 0; i < 24; i++) {\n"
    "        if (i >= lightCount) break;\n"
    "        float f = max(0.0, 1.0 - length(lightPos[i].xyz - fragPos)/lightPos[i].w);\n"
    "        add += lightCol[i]*f*f;\n"
    "    }\n"
    "    finalColor = vec4(tex*(fragColor.rgb + add), 1.0);\n"
    "}\n";
static Shader levelShader;
static int locLvlCount, locLvlPos, locLvlCol, locMdlCount, locMdlPos, locMdlCol;

static void AddLight(Vector4 *pos, Vector3 *col, int *n, Vector3 at, float radius,
                     float r, float g, float b, float k)
{
    if (*n >= MAX_LIGHTS || k <= 0.01f) return;
    pos[*n] = (Vector4){ at.x, at.y, at.z, radius };
    col[*n] = (Vector3){ r*k, g*k, b*k };
    (*n)++;
}

static void UpdateDynamicLights(const Sim *sim, float alpha)
{
    Vector4 pos[MAX_LIGHTS];
    Vector3 col[MAX_LIGHTS];
    int n = 0;
    for (int i = 0; i < sim->boomHead && i < BOOMS_MAX; i++) {   // blasts first: the brightest
        const Boom *b = &sim->booms[i];
        long age = sim->tic - b->tic;
        if (age < 0 || age > 11) continue;
        bool barrel = b->kind == BOOM_BARREL;
        float fade = 1.0f - ((float)age + alpha)/12.0f;
        float k = barrel ? 3.5f*fade*fade : 1.8f*fade;
        AddLight(pos, col, &n, (Vector3){ b->pos.x, b->z + 24.0f, -b->pos.y },
                 barrel ? 340.0f : 260.0f,
                 1.0f, 0.62f, 0.25f, k);
    }
    for (int i = 0; i < PROJS_MAX; i++) {
        const Proj *p = &sim->projs[i];
        if (!p->alive) continue;
        float x = p->prevPos.x + (p->pos.x - p->prevPos.x)*alpha;
        float y = p->prevPos.y + (p->pos.y - p->prevPos.y)*alpha;
        float z = p->prevZ + (p->z - p->prevZ)*alpha;
        Vector3 at = { x, z, -y };
        if (p->isRocket)                       AddLight(pos, col, &n, at, 170, 1.0f, 0.75f, 0.35f, 1.1f);
        else if (p->ownerKind == MK_CACO)      AddLight(pos, col, &n, at, 140, 1.0f, 0.25f, 0.55f, 1.1f);
        else if (p->ownerKind == MK_BARON || p->ownerKind == MK_HELLKNIGHT)
                                               AddLight(pos, col, &n, at, 150, 0.35f, 1.0f, 0.35f, 1.1f);
        else                                   AddLight(pos, col, &n, at, 140, 1.0f, 0.5f, 0.15f, 1.1f);
    }
    for (int i = 0; i < sim->mobCount; i++) {   // arch-vile fire on his target
        const Mob *m = &sim->mobs[i];
        if (m->kind != MK_ARCHVILE || m->state != MOB_ATTACK) continue;
        Vector3 at = (m->targetMob < 0) ? DgRenderPos(sim, alpha)
                                        : MobRenderPos(&sim->mobs[m->targetMob], alpha);
        at.y += 30.0f;
        AddLight(pos, col, &n, at, 200, 1.0f, 0.55f, 0.1f, 1.4f);
    }
    for (int i = 0; i < TRACERS_MAX; i++) {     // muzzle flashes
        const Tracer *t = &sim->tracers[i];
        if (t->ttl <= 0) continue;
        float k = (t->fromDG ? 0.9f : 0.6f)*(float)t->ttl/4.0f;
        AddLight(pos, col, &n, t->a, 120, 1.0f, 0.85f, 0.55f, k);
    }
    SetShaderValue(levelShader, locLvlCount, &n, SHADER_UNIFORM_INT);
    if (n > 0) {
        SetShaderValueV(levelShader, locLvlPos, pos, SHADER_UNIFORM_VEC4, n);
        SetShaderValueV(levelShader, locLvlCol, col, SHADER_UNIFORM_VEC3, n);
    }
    if (gmodels.any) {
        SetShaderValue(gmodels.shader, locMdlCount, &n, SHADER_UNIFORM_INT);
        if (n > 0) {
            SetShaderValueV(gmodels.shader, locMdlPos, pos, SHADER_UNIFORM_VEC4, n);
            SetShaderValueV(gmodels.shader, locMdlCol, col, SHADER_UNIFORM_VEC3, n);
        }
    }
}
// Render-only smoothed facings: the sim turns in 35 Hz steps; models
// would visibly snap, so the displayed yaw eases toward the sim's.
static float mobYaw[MOBS_MAX];
static bool mobYawInit[MOBS_MAX];
static float dgYaw;
// Cull: skinning ~130 monsters twice a frame (iso view + doomcam) was the
// models' whole CPU cost; most of them are off-screen.
static int gViewW, gViewH;   // target size of the DrawWorld pass in flight
static bool InView(Camera3D cam, Vector3 p, float margin)
{
    if (cam.projection == CAMERA_PERSPECTIVE) {
        Vector3 f = Vector3Subtract(cam.target, cam.position);
        Vector3 d = Vector3Subtract(p, cam.position);
        if (Vector3DotProduct(f, d) <= 0.0f) return false;
        if (Vector3Length(d) > 2200.0f) return false;
    }
    Vector2 s = GetWorldToScreenEx(p, cam, gViewW, gViewH);
    return s.x > -margin && s.y > -margin && s.x < gViewW + margin && s.y < gViewH + margin;
}

static float SmoothYaw(float *cur, bool *init, float target)
{
    if (!*init) { *cur = target; *init = true; return target; }
    float d = fmodf(target - *cur + 540.0f, 360.0f) - 180.0f;
    float k = 1.0f - expf(-GetFrameTime()*14.0f);
    *cur += d*k;
    return *cur;
}
static bool debugHud;          // F1: the developer overlay
static int howtoFrames;         // start-of-match "how to play" card

typedef struct ViewState {
    Vector3 target;
    float yawDeg;
    float targetYawDeg;    // Q/E snap destination; yawDeg lerps toward it
    float pitchDeg;
    float fovy;            // ortho vertical view height, world units
    Vector3 homeTarget;
    float homeFovy;
    bool atZoomMin, atZoomMax;
    bool follow;           // camera tracks the marine (SPACE); any pan releases it
} ViewState;

// ---------------------------------------------------------------- args

static void PrintUsage(FILE *out)
{
    fprintf(out,
        "usage: helldrop [--wad <path>] [--map <name>] [--no-log]\n"
        "                [--selftest] [--exit-after <N>] [--turbo <N>] [--seed <N>] [--autodrop]\n"
        "                [--dump-frames <dir>] [--dump-every <N>]\n");
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
    opt->dumpDir = NULL;
    opt->dumpEvery = 1;
    opt->dumpVideo = NULL;
    opt->simOnly = 0;
    opt->viewer = false;
    opt->winW = 1600;
    opt->winH = 900;
    opt->seed = 0;
    opt->garrisonSkill = -1;
    opt->garrisonDmg = -1;

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
        } else if (strcmp(argv[i], "--dump-frames") == 0 && i + 1 < argc) {
            opt->dumpDir = argv[++i];
        } else if (strcmp(argv[i], "--simonly") == 0 && i + 1 < argc) {
            char *end = NULL;
            opt->simOnly = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || opt->simOnly <= 0) return false;
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            const char *value = argv[++i];
            char *end = NULL;
            errno = 0;
            if (value[0] < '0' || value[0] > '9') return false;
            unsigned long long seed = strtoull(value, &end, 10);
            if (errno || *end || seed > UINT32_MAX) return false;
            opt->seed = (uint32_t)seed;
        } else if (strcmp(argv[i], "--garrison-skill") == 0 && i + 1 < argc) {
            opt->garrisonSkill = atoi(argv[++i]);
            if (opt->garrisonSkill < 1 || opt->garrisonSkill > 4) return false;
        } else if (strcmp(argv[i], "--garrison-dmg") == 0 && i + 1 < argc) {
            opt->garrisonDmg = atoi(argv[++i]);
            if (opt->garrisonDmg < 1 || opt->garrisonDmg > 400) return false;
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &opt->winW, &opt->winH) != 2 ||
                opt->winW < 320 || opt->winH < 240) return false;
        } else if (strcmp(argv[i], "--viewer") == 0) {
            opt->viewer = true;
        } else if (strcmp(argv[i], "--bg") == 0 || strcmp(argv[i], "--show") == 0) {
            // handled by kit_window_flags (hidden, unfocused, silent test runs)
        } else if (strcmp(argv[i], "--dump-video") == 0 && i + 1 < argc) {
            opt->dumpVideo = argv[++i];
        } else if (strcmp(argv[i], "--dump-every") == 0 && i + 1 < argc) {
            char *end = NULL;
            opt->dumpEvery = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || opt->dumpEvery <= 0) return false;
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

// Decorations and keys: WAD sprites standing on the floor (they used to be
// grey placeholder cylinders). Live sim entities (monsters, simulated
// pickups, barrels) are drawn by their own passes.
static void DrawThings(const DoomMap *map, const float *heights,
                       const GameSprites *gs, Camera3D cam, long tic)
{
    if (!heights || !gs->valid) return;
    static const int SIM_PICKUPS[] = { 2011, 2012, 2018, 2001, 2007, 2048, 2008,
                                       2049, 2002, 2003, 2010, 2046, 2035 };
    unsigned skillBit = SimGetTuning().garrisonSkillBit;
    for (int i = 0; i < map->thingCount; i++) {
        const Thing *t = &map->things[i];
        if (t->flags & 0x10) continue;                 // multiplayer only
        if (!(t->flags & skillBit) && (t->flags & 0x07)) continue;
        ThingCat cat = ThingCategory(t->type);
        if (cat == CAT_PLAYER || cat == CAT_MONSTER || cat == CAT_BOSS) continue;
        bool live = false;
        for (size_t k = 0; k < sizeof(SIM_PICKUPS)/sizeof(SIM_PICKUPS[0]); k++)
            if (SIM_PICKUPS[k] == t->type) { live = true; break; }
        if (live) continue;
        const SpriteFrame *f = DecoSpriteFrame(gs, t->type, tic + i*7);
        if (!f) continue;
        DrawSpriteBillboard(cam, f, 0, (Vector3){ t->x, heights[i], -t->y }, WHITE);
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

static float InterpolateFacing(float previous, float current, float alpha)
{
    return previous + (fmodf(current - previous + 540.0f, 360.0f) - 180.0f)*alpha;
}

static Color SpriteLighting(const Sim *sim, int sector)
{
    unsigned char shade = (unsigned char)(212.0f + 43.0f*SectorLight01(sim, sector));
    return (Color){ shade, shade, shade, 255 };
}

static void DrawDoomGuy(const Sim *sim, float alpha, const GameSprites *gs,
                        Camera3D cam)
{
    Vector3 p = DgRenderPos(sim, alpha);
    if (gmodels.enabled && gmodels.marine.loaded) {
        ModelClip clip; float ct; bool loop;
        DgModelPose(&sim->dg, sim->tic, alpha, &clip, &ct, &loop);
        static bool dgInit;
        if (sim->tic < 2) dgInit = false;
        float yaw = SmoothYaw(&dgYaw, &dgInit, sim->dg.angleDeg);
        GameModelDrawEx(&gmodels, &gmodels.marine, p, yaw, clip, ct, loop,
                        WHITE, SectorLight01(sim, sim->dg.sector),
                        (Color){ 90, 255, 120, 110 });
        return;
    }
    const DoomGuy *dg = &sim->dg;
    float travel = Vector2Distance(dg->prevPos, dg->pos);
    float stride = (dg->walkDistance - travel*(1.0f - alpha))/22.0f;
    const SpriteFrame *f = gs->valid ? DGSpriteFrame(gs, dg, sim->tic, stride) : NULL;
    if (f) {
        int rot = SpriteRotationFrom(InterpolateFacing(dg->prevAngleDeg, dg->angleDeg, alpha),
                                     (Vector2){ cam.position.x, -cam.position.z },
                                     (Vector2){ p.x, -p.z });
        DrawWalkingSprite(cam, f, rot, p, SpriteLighting(sim, dg->sector),
                          stride, fminf(travel/4.0f, 1.0f));
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

static float SectorLight01(const Sim *sim, int sector)
{
    if (sector < 0 || sector >= sim->map->sectorCount) return 0.8f;
    return (float)sim->map->sectors[sector].light/255.0f;
}

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
        float travel = Vector2Distance(m->prevPos, m->pos);
        float stride = (m->walkDistance - travel*(1.0f - alpha))/14.0f;
        const GameModel *gmdl = gmodels.enabled ? &gmodels.mob[m->kind] : NULL;
        const SpriteFrame *sprite = (!gmdl || !gmdl->loaded) && gs->valid
                                      ? MobSpriteFrame(gs, m, sim->tic, stride) : NULL;
        if (gmdl && gmdl->loaded && !InView(cam, (Vector3){ p.x, p.y + 28.0f, p.z }, 120.0f)) continue;
        if (gmdl && gmdl->loaded) {
            ModelClip clip; float ct; bool loop;
            MobModelPose(m, i, sim->tic, alpha, &clip, &ct, &loop);
            Color tint = WHITE;
            if (m->kind == MK_SPECTRE) tint = (Color){ 40, 40, 58, 255 };
            if (m->state == MOB_PAIN && ((sim->tic/2) & 1)) tint = (Color){ 255, 200, 200, 255 };
            float yaw = (m->state == MOB_DEAD) ? mobYaw[i] : SmoothYaw(&mobYaw[i], &mobYawInit[i], m->angleDeg);
            // Live demons x-ray through iso walls (iso view only; the
            // doomcam is his eyes, he can't see through walls).
            Color sil = (m->state != MOB_DEAD && cam.projection == CAMERA_ORTHOGRAPHIC && m->kind != MK_SPECTRE)
                            ? (Color){ 230, 60, 40, 70 } : (Color){ 0 };
            GameModelDrawEx(&gmodels, gmdl, p, yaw, clip, ct, loop, tint,
                            SectorLight01(sim, m->sector), sil);
        } else if (sprite) {
            // Spectres render as a dark ghost of the pinky sprite.
            Color tint = (m->kind == MK_SPECTRE) ? (Color){ 38, 38, 50, 255 }
                                                 : SpriteLighting(sim, m->sector);
            int rot = SpriteRotationFrom(InterpolateFacing(m->prevAngleDeg, m->angleDeg, alpha),
                                         viewer, (Vector2){ p.x, -p.z });
            float motion = m->state == MOB_CHASE && !MobIsFlyer(m->kind) ? fminf(travel/2.5f, 1.0f) : 0;
            DrawWalkingSprite(cam, sprite, rot, p, tint, stride, motion);
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
    for (int i = 0; i < sim->boomHead && i < BOOMS_MAX; i++) {
        const Boom *b = &sim->booms[i];
        long age = sim->tic - b->tic;
        if (age < 0 || age > 11) continue;
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
        if (gmodels.enabled && gmodels.barrel.loaded) {
            GameModelDraw(&gmodels, &gmodels.barrel, base, (float)(i*47 % 360), CLIP_IDLE, 0, true,
                          WHITE, 0.8f);
        } else if (f) {
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
        if (gmodels.enabled && gmodels.pickup[p->type].loaded) {
            // Items idle-spin and bob so they read as "take me".
            float tt = (float)sim->tic/35.0f + (float)i*0.7f;
            Vector3 at = { p->pos.x, p->z + 3.0f + sinf(tt*2.5f)*2.0f, -p->pos.y };
            GameModelDraw(&gmodels, &gmodels.pickup[p->type], at, tt*70.0f, CLIP_IDLE, 0, true,
                          WHITE, 0.9f);
            continue;
        }
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
    StateLogBool("follow", vs->follow);
    StateLogObjEnd();
    StateLogObjBegin("doomguy");
    StateLogFloat("x", sim->dg.pos.x);
    StateLogFloat("y", sim->dg.pos.y);
    StateLogFloat("z", sim->dg.z);
    StateLogFloat("angle", sim->dg.angleDeg);
    StateLogStr("state", DGStateName(sim->dg.state));
    StateLogStr("objective", (sim->dg.objective == OBJ_HEAL) ? "heal"
                : (sim->dg.objective == OBJ_WEAPON) ? "weapon"
                : (sim->dg.objective == OBJ_SWITCH) ? "switch" : "exit");
    StateLogInt("switch_line", sim->dg.switchLine);
    StateLogInt("nav_skips", sim->dg.navSkips);
    StateLogInt("no_progress_tics", sim->dg.noProgressTics);
    StateLogInt("heal_retry_tic", sim->dg.healRetryTic);
    StateLogInt("next_plan_tic", sim->dg.nextPlanTic);
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
        if (sim->lifts[i].stairTag) StateLogInt("stair_tag", sim->lifts[i].stairTag);
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

#define FOLLOW_FOVY 1100.0f   // readable default: the marine ~50px tall

static void ViewReset(ViewState *vs)
{
    vs->follow = false;
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
    if (IsKeyPressed(KEY_SPACE) && !vs->follow) {
        vs->follow = true;
        if (vs->fovy > 2.0f*FOLLOW_FOVY) vs->fovy = FOLLOW_FOVY;
        LogInput("follow");
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
        if (vs->follow) LogInput("follow_off");
        vs->follow = false;
        pan = Vector3Scale(Vector3Normalize(pan), vs->fovy*0.9f*dt);
        vs->target = Vector3Add(vs->target, pan);
    }

    if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
        Vector2 d = GetMouseDelta();
        if ((d.x != 0.0f || d.y != 0.0f) && vs->follow) { vs->follow = false; LogInput("follow_off"); }
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
    // Start on the marine, close enough to read the fight; R shows the map.
    vs->follow = true;
    vs->fovy = FOVY_MIN > FOLLOW_FOVY ? FOVY_MIN : FOLLOW_FOVY;
    const char *fz = getenv("HELLDROP_FOVY");   // test captures: zoom override
    if (fz && atof(fz) > 0) vs->fovy = Clamp((float)atof(fz), FOVY_MIN, FOVY_MAX);
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
    if (!LevelMeshBuild(lm, map, wad)) return false;
    if (levelShader.id) LevelMeshSetShader(lm, levelShader);
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
    if (gs->valid) BeginShaderMode(gs->alphaTest);
    DrawThings(map, thingHeights, gs, cam, sim->tic);
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
    float a = InterpolateFacing(sim->dg.prevAngleDeg, sim->dg.angleDeg, alpha)*DEG2RAD;
    float eyeH = 41.0f;
    if (sim->dg.state == DG_DEAD) {
        float dt = (float)(sim->tic - sim->dg.deathTic) + alpha;
        float drop = fminf(dt/35.0f, 1.0f);
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

    int secs = (int)(sim->dg.finishTic/SIM_TICRATE);
    int par = ParSeconds(map->name);
    const char *lines[5];
    lines[0] = (par > 0)
        ? TextFormat("TIME %d:%02d   (PAR %d:%02d%s)", secs/60, secs%60,
                     par/60, par%60,
                     (secs > par) ? TextFormat("  +%ds over", secs - par) : "")
        : TextFormat("TIME %d:%02d", secs/60, secs%60);
    lines[1] = TextFormat("DEMONS HE KILLED  %ld", sim->kills);
    lines[2] = TextFormat("HIS BLOOD  %ld damage taken", sim->dgDamageTaken);
    lines[3] = TextFormat("YOUR HAND  %d drops   %ld souls spent",
                          sim->dropsMade, sim->soulsSpent);
    lines[4] = TextFormat("MARINE HEALTH  %d%%", sim->dg.health);
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
    if (!debugHud && getenv("HELLDROP_CLEAN_HUD")) goto end_hud;   // trailer footage
    if (!debugHud) {
        DrawTextShadowed("LMB drop demon   1-9,0 pick   F slam door/lift   V barrel   "
                         "TAB his tells   SPACE follow   R map   F1 debug",
                         12, y, 14, (Color){ 150, 150, 150, 220 });
        goto end_hud;
    }
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
                     "R view  TAB codex  C doomcam  U mute  P pause  G restart  ESC quit",
                     12, y, 14, GRAY);
    y += 18;
    DrawTextShadowed(TextFormat("HELL'S HAND:  F slam door %d / recall lift %d"
                                "   V detonate barrel %d",
                                VERB_DOOR_COST, VERB_LIFT_COST, VERB_BARREL_COST),
                     12, y, 14, (Color){ 230, 140, 90, 255 });

end_hud:
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
// The summoning column: each demon's own WAD sprite as its portrait, the
// hotkey, and the price. Dim = can't afford, dark = not unlocked yet.
static void DrawSoulsPanel(const Sim *sim, MobKind selected, unsigned unlocked,
                           const GameSprites *gs)
{
    int sw = GetScreenWidth();
    int y = 12;
    const char *souls = TextFormat("SOULS %d", (int)sim->souls);
    int w = MeasureText(souls, 30);
    DrawTextShadowed(souls, sw - w - 16, y, 30, (Color){ 255, 170, 70, 255 });
    y += 34;
    const char *rate = TextFormat("+%.1f/s  +1 per HP of his blood", SimSoulRate(sim));
    w = MeasureText(rate, 14);
    DrawTextShadowed(rate, sw - w - 16, y, 14, (Color){ 200, 140, 70, 255 });
    y += 22;

    // Hell's Hand cooldowns: ready verbs glow, resting ones count down.
    static const struct { int verb; const char *key; const char *name; int cost; } VERBS[] = {
        { VERB_DOOR, "F", "SLAM DOOR", VERB_DOOR_COST },
        { VERB_LIFT, "F", "RECALL LIFT", VERB_LIFT_COST },
        { VERB_BARREL, "V", "BLOW BARREL", VERB_BARREL_COST },
    };
    for (int v = 0; v < 3; v++) {
        long left = sim->verbReadyTic[VERBS[v].verb] - sim->tic;
        bool ready = left <= 0;
        bool afford = sim->souls >= (float)VERBS[v].cost;
        const char *t = ready ? TextFormat("[%s] %s %d", VERBS[v].key, VERBS[v].name, VERBS[v].cost)
                              : TextFormat("[%s] %s %ds", VERBS[v].key, VERBS[v].name, (int)(left/35 + 1));
        Color c = !ready ? (Color){ 110, 90, 80, 255 }
                : afford ? (Color){ 255, 150, 90, 255 } : (Color){ 150, 80, 70, 255 };
        w = MeasureText(t, 14);
        DrawTextShadowed(t, sw - w - 16, y, 14, c);
        y += 17;
    }
    y += 8;

    // Display keys must match the actual bindings, not enum order.
    static const char *KEY_LABELS[MK_COUNT] = {
        [MK_ZOMBIE] = "1", [MK_SHOTGUNGUY] = "2", [MK_IMP] = "3",
        [MK_PINKY] = "4", [MK_SPECTRE] = "0", [MK_CACO] = "5",
        [MK_LOSTSOUL] = "6", [MK_REVENANT] = "7", [MK_BARON] = "8",
        [MK_ARCHVILE] = "9", [MK_CHAINGUNNER] = "S1", [MK_HELLKNIGHT] = "S2",
        [MK_MANCUBUS] = "S3", [MK_PAINELEMENTAL] = "S4",
    };
    static const MobKind ORDER[MK_COUNT] = {
        MK_ZOMBIE, MK_SHOTGUNGUY, MK_IMP, MK_PINKY, MK_CACO, MK_LOSTSOUL,
        MK_REVENANT, MK_BARON, MK_ARCHVILE, MK_SPECTRE, MK_CHAINGUNNER,
        MK_HELLKNIGHT, MK_MANCUBUS, MK_PAINELEMENTAL,
    };
    const int rowH = 40, colW = 236;
    int x0 = sw - colW - 12;
    for (int o = 0; o < MK_COUNT; o++) {
        MobKind k = ORDER[o];
        bool isUnlocked = (unlocked & (1u << k)) != 0;
        if (!isUnlocked && !gs->mobs[k].any && !gmodels.hasPortrait[k]) continue;
        int cost = SimDropBaseCost(k);
        bool affordable = sim->souls >= (float)cost;
        bool sel = (k == selected) && isUnlocked;
        Color bg = sel ? (Color){ 70, 20, 16, 210 } : (Color){ 10, 8, 8, 150 };
        DrawRectangle(x0, y, colW, rowH - 4, bg);
        if (sel) DrawRectangleLines(x0, y, colW, rowH - 4, (Color){ 255, 120, 60, 255 });
        // Portrait: frame A, facing the viewer, fitted into the row.
        const SpriteFrame *pf = (gs->mobs[k].any && gs->mobs[k].frames[0].present)
                                    ? &gs->mobs[k].frames[0] : NULL;
        Color tint = !isUnlocked ? (Color){ 40, 40, 40, 255 }
                   : !affordable ? (Color){ 120, 100, 100, 255 } : WHITE;
        if (k == MK_SPECTRE && isUnlocked) tint.a = 110;
        if (gmodels.enabled && gmodels.hasPortrait[k]) {
            Texture2D t = gmodels.portrait[k].texture;
            DrawTexturePro(t, (Rectangle){ 0, 0, (float)t.width, -(float)t.height },
                           (Rectangle){ x0 + 2, y - 1, 38, 38 }, (Vector2){ 0, 0 }, 0.0f, tint);
        } else if (pf) {
            Texture2D t = pf->tex[0];
            float s = fminf((rowH - 8)/(float)t.height, 40.0f/(float)t.width);
            float dw = t.width*s, dh = t.height*s;
            Rectangle src = { 0, 0, pf->mirror[0] ? -(float)t.width : (float)t.width, (float)t.height };
            DrawTexturePro(t, src, (Rectangle){ x0 + 4 + (40 - dw)/2, y + (rowH - 4 - dh)/2, dw, dh },
                           (Vector2){ 0, 0 }, 0.0f, tint);
        }
        Color nameC = !isUnlocked ? (Color){ 95, 85, 85, 255 }
                    : !affordable ? (Color){ 140, 95, 90, 255 } : MOB_COLORS[k];
        DrawTextShadowed(KEY_LABELS[k], x0 + 50, y + 4, 12, (Color){ 200, 200, 200, 255 });
        DrawTextShadowed(MobKindName(k), x0 + 50, y + 17, 16, nameC);
        const char *right = isUnlocked ? TextFormat("%d", cost) : UnlockMapFor(k);
        int rw = MeasureText(right, isUnlocked ? 20 : 12);
        DrawTextShadowed(right, x0 + colW - rw - 8, y + (isUnlocked ? 9 : 13), isUnlocked ? 20 : 12,
                         isUnlocked ? (affordable ? (Color){ 255, 190, 90, 255 } : (Color){ 150, 90, 80, 255 })
                                    : (Color){ 110, 100, 100, 255 });
        y += rowH;
    }
}

// First seconds of a match: what you are and how to hurt him.
static void DrawHowTo(int frames)
{
    if (frames <= 0) return;
    unsigned char a = (unsigned char)((frames > 60 ? 1.0f : frames/60.0f)*230.0f);
    static const char *LINES[] = {
        "YOU ARE HELL.  Kill the marine before he flips the exit switch.",
        "1-9 pick a demon, LEFT-CLICK to drop it. Out of his sight = cheap and instant.",
        "Every point of damage he takes pays you a soul. Healing he finds is your loss.",
        "F slams a door or recalls a lift. V blows a barrel. TAB shows his weaknesses.",
    };
    int w = 760, h = 40 + 4*26 + 20;
    int x = GetScreenWidth()/2 - w/2, y = GetScreenHeight()/2 - h/2 - 60;
    DrawRectangle(x, y, w, h, (Color){ 12, 6, 6, a });
    DrawRectangleLines(x, y, w, h, (Color){ 220, 70, 50, a });
    for (int i = 0; i < 4; i++) {
        Color c = (i == 0) ? (Color){ 255, 110, 80, a } : (Color){ 235, 225, 215, a };
        int fs = (i == 0) ? 22 : 17;
        int tw = MeasureText(LINES[i], fs);
        DrawTextShadowed(LINES[i], x + w/2 - tw/2, y + 20 + i*26, fs, c);
    }
    const char *dismiss = "press any key";
    DrawTextShadowed(dismiss, x + w/2 - MeasureText(dismiss, 12)/2, y + h - 18, 12,
                     (Color){ 150, 150, 150, a });
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
static bool ScreenGroundPoint(const LevelMesh *lm, Camera3D cam, Vector2 screen, Vector2 *out)
{
    Ray ray = GetScreenToWorldRay(screen, cam);
    RayCollision rc = GetRayCollisionMesh(ray, lm->floors, MatrixIdentity());
    if (!rc.hit) return false;
    out->x = rc.point.x;
    out->y = -rc.point.z;
    return true;
}

// ---------------------------------------------------------------- main

// ---- Loop state hoisted to file scope so one per-frame step can be driven
// by emscripten_set_main_loop() on web. On native this is identical to the
// previous locals (same zero/explicit initialisation, single-threaded). ----
static Options opt;
static Wad wad;
static DoomMap map;
static LevelMesh lm;
static float *thingHeights;
static Sim sim;
static ViewState vs;
static DoomHud hud;
static FaceState faceState = { .prevHealth = 100, .hpRef = 100, .hpRefFrames = 35 };
static GameSprites gsprites;
static GameAudio gaudio;
static long frame;
static double simAcc;
static bool paused, muted;
static MobKind selectedKind = MK_PINKY;
static char dropMsg[64];
static int dropMsgFrames;
static bool codexOpen;
static char intentPrev[64];
static int intentFlash;
static bool doomcamOn = true;
static RenderTexture2D doomcamRT;
static bool campaignComplete;
static unsigned unlocked;
static Melt melt;
static bool campaign;
static char curMap[9];

static bool HelldropFrame(void);   // one game-loop iteration (false => stop)
static void AutodropTick(Sim *simp);
static void MainCleanup(void);
#ifdef HELLDROP_TOUCH
#ifdef PLATFORM_WEB
static void HelldropFrameWeb(void);
#endif

#ifdef PLATFORM_IOS
static int quickAmbushLimit = -1;
static Vector3 ambushMarker;
static long ambushQuoteTic = -1;
static MobKind ambushQuoteKind;

// Follow the marine's actual route around corners. Quote the same spot that
// the button deploys to, including the normal in-sight summon premium.
static void QuoteAmbush(void)
{
    if (ambushQuoteTic >= 0 && sim.tic >= ambushQuoteTic &&
        sim.tic - ambushQuoteTic < 5 && ambushQuoteKind == selectedKind) return;
    float ahead = 112.0f;
    Vector2 pos = sim.dg.pos;
    for (int i = sim.dg.wpCursor; i < sim.dg.wpCount && ahead > 0; i++) {
        Vector2 next = sim.dg.wp[i];
        float distance = Vector2Distance(pos, next);
        if (distance < 0.01f) continue;
        pos = Vector2Lerp(pos, next, fminf(ahead / distance, 1.0f));
        ahead -= distance;
    }
    bool valid;
    float floor = SimFloorAtPoint(&sim, pos.x, pos.y, &valid);
    if (!valid) { pos = sim.dg.pos; floor = sim.dg.z; }
    ambushMarker = (Vector3){pos.x, floor + 4.0f, -pos.y};
    int price = SimDropCost(&sim, selectedKind, pos.x, pos.y, NULL);
    ambushQuoteTic = sim.tic;
    ambushQuoteKind = selectedKind;
    TouchAmbushPrice(price);
}
#endif
static bool webHover;
static Vector2 webHoverPosition;
static bool webStarted, webRetry, webTap, webBlast, webModelsReady;
static bool webModelsRequested, webPause, webInterrupted, webFollow, webDoomcam;
static int webMute = -1;
enum {
    TRAIN_BARREL = 0, TRAIN_CHAIN = 1,
    TRAIN_FIRST = 11, TRAIN_SHADOW, TRAIN_DOOR, TRAIN_CROSSFIRE, TRAIN_GAUNTLET
};

static int webLevelRequest = -1;
static int webLevel = TRAIN_FIRST;
static Vector2 webPan;
static float webZoom;
static int webPick = -1;
static int webTool;
static Vector2 webTapPosition;
static bool webAmbush = true;
static bool ambushReady, trainingDoorUsed;
static int trainingDrops;
static bool trainingCleared;

typedef struct TrainingLesson {
    const char *map;
    int health, souls, drops;
    bool door;
    Vector2 spots[2];
} TrainingLesson;

static const TrainingLesson training[] = {
    [TRAIN_BARREL] = { "AMBUSH", 65, 75, 1, false, {{ 370, 154 }} },
    [TRAIN_CHAIN] = { "CHAIN", 65, 75, 1, false, {{ 370, 154 }} },
    [TRAIN_FIRST] = { "FIRST", 8, 35, 1, false, {{ 460, 128 }} },
    [TRAIN_SHADOW] = { "SHADOW", 100, 22, 1, false, {{ 304, 96 }} },
    [TRAIN_DOOR] = { "HOLD", 12, 55, 1, true, {{ 305, 104 }} },
    [TRAIN_CROSSFIRE] = { "CROSSFIR", 42, 55, 2, false, {{ 306, 58 }, { 306, 133 }} },
    [TRAIN_GAUNTLET] = { "GAUNTLET", 80, 95, 2, true, {{ 372, 151 }, { 451, 143 }} },
};

static bool TrainingPlaced(void)
{
    return trainingDrops >= training[webLevel].drops;
}
static int ambushBlastDamage, ambushBlastCount;


#ifdef PLATFORM_WEB
EM_JS(void, WebReady, (void), { setupGame(); });
TOUCH_EXPORT void WebResizeStage(int width, int height)
{
    // raylib's web resize callback clamps the viewport to the window maximum.
    if (width > 0 && height > 0) SetWindowMaxSize(width, height);
}
EM_JS(void, TouchAddDemon, (int kind, const char *name, int cost), {
    addDemon(kind, UTF8ToString(name), cost);
});
EM_JS(void, TouchStatus, (int stage, int kind, int souls, int health, int near, int mode, int blastDamage, int level, int stopped, int chain), {
    updateGame(stage, kind, souls, health, near, mode, blastDamage, level, stopped, chain);
});
EM_JS(void, WebLoadModels, (void), { loadModels(); });
EM_JS(void, TouchEpisodeStatus, (int next, int ticks, int barrelCooldown, int doorCooldown), {
    updateEpisode(next, ticks, barrelCooldown, doorCooldown);
});
EM_JS(void, TouchNotice, (const char *message), { setNotice(UTF8ToString(message)); });

#else
static void WebLoadModels(void) { webModelsReady = true; }
#endif
TOUCH_EXPORT void WebAction(int action, int value)
{
    switch (action) {
    case WEB_START: webStarted = true; break;
    case WEB_RETRY: webRetry = true; break;
    case WEB_CONTINUE: webLevelRequest = value; break;
    case WEB_PICK: webPick = value; webTool = 0; break;
    case WEB_TOOL: webTool = value; break;
    case WEB_BLAST: webBlast = true; break;
    case WEB_MODELS: webModelsReady = true; break;
    case WEB_PAUSE: webPause = true; break;
    case WEB_FOLLOW: webFollow = true; break;
    case WEB_ZOOM: webZoom += value; break;
    case WEB_MUTE: webMute = value; break;
    case WEB_DOOMCAM: webDoomcam = true; break;
#ifdef PLATFORM_IOS
    case IOS_AMBUSH: quickAmbushLimit = value; break;
#endif
    case WEB_INTERRUPT: webInterrupted = true; break;
    }
}

TOUCH_EXPORT void WebTap(float x, float y)
{
    webTap = true;
    webTapPosition = (Vector2){ x, y };
}

TOUCH_EXPORT void WebHover(float x, float y, int visible)
{
    webHover = visible;
    webHoverPosition = (Vector2){ x, y };
}

TOUCH_EXPORT void WebPan(float x, float y)
{
    webPan.x += x;
    webPan.y += y;
}

static void UpdateWebCamera(void)
{
    if (webFollow) {
        vs.follow = true;
        vs.fovy = FOLLOW_FOVY;
    }
    if (webZoom != 0.0f) vs.fovy = Clamp(vs.fovy*powf(0.85f, webZoom), FOVY_MIN, FOVY_MAX);
    if (webPan.x != 0.0f || webPan.y != 0.0f) {
        vs.follow = false;
        float yaw = vs.yawDeg*DEG2RAD;
        Vector3 right = { cosf(yaw), 0.0f, -sinf(yaw) };
        Vector3 forward = { -sinf(yaw), 0.0f, -cosf(yaw) };
        vs.target = Vector3Add(vs.target, Vector3Scale(right, -webPan.x*vs.fovy));
        vs.target = Vector3Add(vs.target, Vector3Scale(forward,
                                   webPan.y*vs.fovy/sinf(vs.pitchDeg*DEG2RAD)));
    }
    webPan = (Vector2){ 0 };
    webZoom = 0;
    webFollow = false;
}

static void FrameAmbush(void)
{
    float aspect = (float)GetScreenWidth()/GetScreenHeight();
    // Look along the corridor on tall screens so both rooms fit.
    vs.yawDeg = aspect < 1.0f ? 72.0f : 45.0f;
    vs.pitchDeg = 62.0f;
    float yaw = vs.yawDeg*DEG2RAD;
    float width = map.maxX - map.minX;
    float depth = map.maxY - map.minY;
    float horizontal = width*cosf(yaw) + depth*sinf(yaw);
    float vertical = (width*sinf(yaw) + depth*cosf(yaw))*sinf(vs.pitchDeg*DEG2RAD);
    vs.target.y = 30.0f;
    vs.fovy = fmaxf((horizontal + 44.0f)/aspect, vertical + 104.0f);
}

static void ResetAmbush(void)
{
    const TrainingLesson *lesson = &training[webLevel];
    sim.souls = (float)lesson->souls;
    sim.dg.health = lesson->health;
    trainingDrops = 0;
    trainingCleared = false;
    trainingDoorUsed = false;
    ambushReady = false;
    ambushBlastDamage = -1;
    ambushBlastCount = 0;
    selectedKind = webLevel >= TRAIN_FIRST && webLevel < TRAIN_GAUNTLET ? MK_SHOTGUNGUY : MK_IMP;
    unlocked = webLevel < TRAIN_FIRST ? (1u << MK_IMP) | (1u << MK_SHOTGUNGUY)
                                     : 1u << selectedKind;
    doomcamOn = false;
    ViewReset(&vs);
}

static bool AmbushNearBarrel(void)
{
    return sim.barrelCount > 0 && sim.barrels[0].alive &&
           Vector2Distance(sim.dg.pos, sim.barrels[0].pos) < 72.0f;
}

static bool SwitchWebLevel(int level)
{
    if (level < 0 || level >= (int)(sizeof(training)/sizeof(training[0]))) return true;
    webAmbush = training[level].map != NULL;
    if (webAmbush) snprintf(curMap, sizeof(curMap), "%s", training[level].map);
    else snprintf(curMap, sizeof(curMap), "E1M%d", level - 1);
    if (!ReloadLevel(&wad, curMap, &map, &lm, &sim, &thingHeights)) return false;
    webLevel = level;
#ifdef PLATFORM_IOS
    quickAmbushLimit = -1;
    ambushQuoteTic = -1;
    if (!webAmbush) selectedKind = MK_SHOTGUNGUY;
#endif
    SetupView(&vs, &map);
    unlocked = 0xFFFFFFFFu;
#ifdef PLATFORM_IOS
    doomcamOn = false;
#else
    doomcamOn = true;
#endif
    if (webAmbush) ResetAmbush();
    else if (!webModelsRequested) {
        webModelsRequested = true;
        WebLoadModels();
    }
    gaudio.cursor = 0;
    dropMsgFrames = 0;
    simAcc = 0;
    paused = false;
    codexOpen = false;
    intentPrev[0] = '\0';
    memset(mobYawInit, 0, sizeof(mobYawInit));
    webTap = webBlast = webFollow = false;
    webPick = -1;
    webTool = 0;
    webPan = (Vector2){ 0 };
    webZoom = 0;
    return true;
}

#endif

#ifdef PLATFORM_IOS
int HelldropIOSStart(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
    if (!ParseArgs(argc, argv, &opt)) {
        PrintUsage(stderr);
        return 2;
    }

#ifdef HELLDROP_TOUCH
    // Start with the authored ambush before offering the full E1M1 demo.
    // Browser sessions have no writable working directory for debug logs.
    opt.mapName = training[webLevel].map;
    opt.mapExplicit = true;
    opt.noLog = true;
    opt.wadPath = "assets/helldrop.wad";
    opt.wadExplicit = true;          // no IWAD auto-detection on web
#endif

    {
        SimTuning tn = SimGetTuning();
        if (opt.garrisonSkill > 0) tn.garrisonSkillBit = (unsigned)opt.garrisonSkill;
        if (opt.garrisonDmg > 0) tn.garrisonDmgPct = opt.garrisonDmg;
        tn.seed = opt.seed;
        SimSetTuning(tn);
    }
    if (!opt.noLog && !StateLogOpen(NULL)) {
        fprintf(stderr, "helldrop: state log unavailable, continuing without\n");
    }
    LogStartup();

    // Runs that quit by themselves are tests: no window, focus, Dock tile or sound.
    if (opt.dumpVideo && !opt.dumpDir) {
        // Video capture shares --dump-frames' fixed-tic timing; sound.txt and
        // snd/ land next to the video.
        static char dir[1024];
        snprintf(dir, sizeof(dir), "%s", opt.dumpVideo);
        char *slash = strrchr(dir, '/');
        if (slash) *slash = '\0'; else snprintf(dir, sizeof(dir), ".");
        opt.dumpDir = dir;
        opt.dumpEvery = 1000000000;   // no PNGs
    }
    if (opt.selftest || opt.exitAfter > 0 || opt.dumpDir || opt.simOnly) kit_background(1);
    SetConfigFlags(kit_window_flags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT |
                                    (opt.dumpVideo ? FLAG_WINDOW_HIGHDPI : 0),
                                    argc, argv));
    InitWindow(opt.winW, opt.winH, "HELLDROP");
    kit_background_apply();   // hidden runs leave the Dock before any loading
#ifdef HELLDROP_TOUCH
    // requestAnimationFrame drives cadence on web; calling SetTargetFPS would
    // make raylib's EndDrawing() block in WaitTime()->emscripten_sleep, which
    // needs ASYNCIFY (which we deliberately avoid). ESC must also not trip the
    // default raylib exit key and cancel the canvas-hosted loop.
    SetExitKey(KEY_NULL);
#else
    SetTargetFPS(60);
    // Left edge of the monitor so the terminal stays visible alongside.
    SetWindowPosition(8, (GetMonitorHeight(GetCurrentMonitor()) - opt.winH)/2);
#endif

    // wad/map/lm live at file scope (above); the loader fills them below.
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
    campaign = !opt.mapExplicit;
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
    levelShader = LoadShaderFromMemory(LEVEL_VS, LEVEL_FS);
    locLvlCount = GetShaderLocation(levelShader, "lightCount");
    locLvlPos = GetShaderLocation(levelShader, "lightPos");
    locLvlCol = GetShaderLocation(levelShader, "lightCol");
    levelShader.locs[SHADER_LOC_MATRIX_MODEL] = GetShaderLocation(levelShader, "matModel");
    if (!LevelMeshBuild(&lm, &map, &wad)) {
        TraceLog(LOG_ERROR, "MAIN: failed to build level mesh for '%s'", curMap);
        MapFree(&map);
        WadFree(&wad);
        CloseWindow();
        StateLogClose();
        return 1;
    }

    if (levelShader.id) LevelMeshSetShader(&lm, levelShader);
    thingHeights = ComputeThingHeights(&map, &lm.floors);
    LogLevelLoaded(&opt, &map, &lm);

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

    // Headless match harness: play the whole level as fast as the CPU allows
    // (no rendering) and print the outcome as one JSON line on stdout.
    if (opt.simOnly > 0) {
        while (sim.tic < opt.simOnly && sim.dg.state != DG_DEAD &&
               sim.dg.state != DG_EXITED && sim.dg.state != DG_NO_GOAL) {
            SimTick(&sim);
            if (opt.autodrop) AutodropTick(&sim);
            if (sim.tic % 175 == 0) { StateLogSetFrame(sim.tic); LogSnapshot(&vs, &sim); }
        }
        int alive = 0;
        for (int m = 0; m < sim.mobCount; m++) if (sim.mobs[m].state != MOB_DEAD) alive++;
        printf("{\"map\":\"%s\",\"autodrop\":%s,\"result\":\"%s\",\"tic\":%ld,"
               "\"secs\":%.1f,\"hp\":%d,\"armor\":%d,\"weapon\":\"%s\",\"kills\":%ld,"
               "\"alive\":%d,\"replans\":%d,\"dmg_taken\":%ld,\"souls\":%.0f,\"nav_skips\":%d}\n",
               curMap, opt.autodrop ? "true" : "false", DGStateName(sim.dg.state),
               sim.tic, sim.tic/35.0, sim.dg.health, sim.dg.armor,
               DGWeaponName(sim.dg.weapon), sim.kills, alive, sim.dg.replans,
               sim.dgDamageTaken, sim.souls, sim.dg.navSkips);
        fflush(stdout);
        LogShutdown(0, sim.tic);
        StateLogClose();
        CloseWindow();
        return 0;
    }

    SetupView(&vs, &map);

    if (!DoomHudLoad(&hud, &wad)) {
        TraceLog(LOG_WARNING, "MAIN: no status-bar graphics in WAD, using fallback HUD");
    }

    if (!GameSpritesLoad(&gsprites, &wad)) {
        TraceLog(LOG_WARNING, "MAIN: no sprites in WAD, using shape renderer");
    }
#ifndef HELLDROP_TOUCH
    {
        const char *mdir = getenv("HELLDROP_MODEL_DIR");
        if (GameModelsLoad(&gmodels, mdir ? mdir : "assets/models")) {
            locMdlCount = GetShaderLocation(gmodels.shader, "lightCount");
            locMdlPos = GetShaderLocation(gmodels.shader, "lightPos");
            locMdlCol = GetShaderLocation(gmodels.shader, "lightCol");
            GameModelsRenderPortraits(&gmodels);
            TraceLog(LOG_INFO, "MAIN: 3D models loaded (M toggles sprites)");
        }
    }
#endif
    if (opt.viewer) {
        // Dev mode: every model on a floor under the iso camera, cycling
        // clips (idle/walk/attack/pain/death per row), depth-tested against
        // a real floor. --selftest --exit-after N screenshots it.
        const char *CLIPN[CLIP_COUNT] = { "idle", "walk", "attack", "pain", "death" };
        for (long f = 0; !WindowShouldClose(); f++) {
            float t = (float)f/60.0f;
            Camera3D c = { 0 };
            c.target = opt.dumpVideo ? (Vector3){ 340, 30, -150 } : (Vector3){ 300, 30, -200 };
            float yawV = (25.0f + t*9.0f)*DEG2RAD;   // slow orbit (reads well in video)
            c.position = Vector3Add(c.target, (Vector3){ 2000*cosf(40*DEG2RAD)*sinf(yawV), 2000*sinf(40*DEG2RAD), 2000*cosf(40*DEG2RAD)*cosf(yawV) });
            c.up = (Vector3){ 0, 1, 0 }; c.fovy = opt.dumpVideo ? 500 : 700; c.projection = CAMERA_ORTHOGRAPHIC;
            int clip = (int)(t/2.0f) % CLIP_COUNT;
            BeginDrawing();
            ClearBackground((Color){ 10, 6, 6, 255 });
            BeginMode3D(c);
            DrawPlane((Vector3){ 300, 0, -200 }, (Vector2){ 4000, 4000 }, opt.dumpVideo ? (Color){ 38, 22, 20, 255 } : (Color){ 70, 68, 64, 255 });
            if (!opt.dumpVideo) DrawGrid(28, 50.0f);
            int n = 0;
            for (int k = 0; k < MK_COUNT; k++) {
                if (k == MK_SPECTRE || !gmodels.mob[k].loaded) continue;
                Vector3 p = { (float)(n % 5)*140.0f, 0, -(float)(n/5)*140.0f };
                GameModelDraw(&gmodels, &gmodels.mob[k], p, 270.0f + 30.0f, (ModelClip)clip, fmodf(t, 2.0f),
                              clip < 2, WHITE, 0.9f);
                n++;
            }
            if (gmodels.marine.loaded)
                GameModelDraw(&gmodels, &gmodels.marine, (Vector3){ 700, 0, 0 }, 300.0f, (ModelClip)clip,
                              fmodf(t, 2.0f), clip < 2, WHITE, 0.9f);
            EndMode3D();
            if (!opt.dumpVideo) DrawText(TextFormat("--viewer  clip: %s", CLIPN[clip]), 12, 12, 24, RAYWHITE);
            EndDrawing();
#ifndef HELLDROP_TOUCH
            if (opt.dumpVideo) {
                static FILE *vp;
                Image shot = LoadImageFromScreen();
                if (!vp) {
                    char cmd[2048];
                    snprintf(cmd, sizeof(cmd), "ffmpeg -v error -y -f rawvideo -pix_fmt rgba -s %dx%d -r 60 -i - "
                             "-vf scale=1920:1080:flags=lanczos,format=yuv420p -c:v libx264 -preset veryfast -crf 14 '%s'",
                             shot.width, shot.height, opt.dumpVideo);
                    vp = popen(cmd, "w");
                }
                if (vp) fwrite(shot.data, 4, (size_t)shot.width*shot.height, vp);
                UnloadImage(shot);
                if (opt.exitAfter > 0 && f + 1 >= opt.exitAfter) { if (vp) pclose(vp); break; }
            }
#endif
            if (opt.selftest && f >= ((opt.exitAfter > 0) ? opt.exitAfter : 60)) {
                TakeScreenshot("selftest.png");
                break;
            }
        }
        GameModelsUnload(&gmodels);
        CloseWindow();
        return 0;
    }
    InitAudioDevice();
    GameAudioLoad(&gaudio, &wad);
    if (opt.dumpDir) GameAudioCaptureOpen(&gaudio, &wad, opt.dumpDir);
    kit_background_apply();
    if (kit_in_background()) SetTargetFPS(0);   // test runs: measure real throughput

    frame = 0;
    simAcc = 0.0;
    selectedKind = MK_PINKY;
    dropMsg[0] = '\0';
    dropMsgFrames = 0;
    codexOpen = false;
    howtoFrames = (opt.selftest || opt.dumpDir) ? 0 : 60*20;
    debugHud = false;
    intentPrev[0] = '\0';
    intentFlash = 0;
    doomcamOn = true;
    doomcamRT = LoadRenderTexture(320, 200);
    SetTextureFilter(doomcamRT.texture, TEXTURE_FILTER_POINT);
    campaignComplete = false;
    unlocked = campaign ? UnlockMaskFor(curMap) : 0xFFFFFFFFu;
    if (!(unlocked & (1u << selectedKind))) selectedKind = MK_ZOMBIE;
    SetWindowTitle(TextFormat("HELLDROP - %s", curMap));

#ifdef HELLDROP_TOUCH
    howtoFrames = 0;
    ResetAmbush();
    for (int i = 0; i < MK_COUNT; i++)
        TouchAddDemon(i, MobKindName((MobKind)i), SimDropBaseCost((MobKind)i));
#ifdef PLATFORM_WEB
    WebReady();
    // requestAnimationFrame-driven (fps=0); simulate_infinite_loop=1 means this
    // never returns and the browser owns the frame cadence.
    emscripten_set_main_loop(HelldropFrameWeb, 0, 1);
#endif
#else
    while (!WindowShouldClose())
        if (!HelldropFrame()) break;
    MainCleanup();
#endif
    return 0;
}

// One iteration of the game loop. Returns false to stop (the native break
// paths: level-load failure, selftest/exit-after). State lives at file scope
// so emscripten_set_main_loop() can invoke this once per browser frame.
static void AutodropTick(Sim *simp)
{
        // Dev smoke test: periodically drop a squad just ahead of him so
        // combat/infighting/drops exercise without a human at the mouse.
        if (simp->tic >= 400 && simp->tic % 400 == 0) {
            // Exercise Hell's Hand: "click" the nearest barrel and the
            // nearest door sector to him (cooldowns self-gate).
            int nb = -1;
            float nbd = 1e9f;
            for (int b = 0; b < simp->barrelCount; b++) {
                if (!simp->barrels[b].alive) continue;
                float dx = simp->barrels[b].pos.x - simp->dg.pos.x;
                float dy = simp->barrels[b].pos.y - simp->dg.pos.y;
                if (dx*dx + dy*dy < nbd) { nbd = dx*dx + dy*dy; nb = b; }
            }
            if (nb >= 0) {
                SimDetonateBarrel(simp, simp->barrels[nb].pos.x,
                                  simp->barrels[nb].pos.y);
            }
            SimSlamMover(simp, simp->dg.pos.x, simp->dg.pos.y, NULL);
        }
        if (simp->tic >= 250 && simp->tic % 250 == 0) {
            // Odd waves bring an arch-vile to bank the previous wave's
            // corpses — the smoke test for resurrection.
            static const MobKind WAVE_A[4] = { MK_PINKY, MK_REVENANT, MK_HELLKNIGHT, MK_CHAINGUNNER };
            static const MobKind WAVE_B[4] = { MK_ARCHVILE, MK_MANCUBUS, MK_PAINELEMENTAL, MK_PINKY };
            const MobKind *wave = ((simp->tic/250) % 2 == 0) ? WAVE_A : WAVE_B;
            float a = simp->dg.angleDeg*DEG2RAD;
            for (int w = 0; w < 4; w++) {
                float side = (w % 2 == 0) ? 60.0f : -60.0f;
                SimDrop(simp, wave[w],
                        simp->dg.pos.x + cosf(a)*200.0f - sinf(a)*side,
                        simp->dg.pos.y + sinf(a)*200.0f + cosf(a)*side);
            }
        }
}

static double frameWorkStart, frameWorkMs;

static bool HelldropFrame(void)
{
        frameWorkStart = GetTime();
        StateLogSetFrame(frame);
        float dt = GetFrameTime();
#ifdef HELLDROP_TOUCH
        bool retry = webRetry;
        bool togglePause = webPause;
#else
        bool retry = IsKeyPressed(KEY_G);
        bool togglePause = IsKeyPressed(KEY_P);
#endif
        bool placed = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
        bool slam = IsKeyPressed(KEY_F);
        bool blast = IsKeyPressed(KEY_V);
        bool toggleDoomcam = IsKeyPressed(KEY_C);
#ifdef HELLDROP_TOUCH
        int muteRequest = -1;
#else
        int muteRequest = IsKeyPressed(KEY_U) ? !muted : -1;
#endif
        Vector2 pointer = GetMousePosition();
#ifdef HELLDROP_TOUCH
        if (!webStarted) return true;
        if (webLevelRequest >= 0) {
            if (!SwitchWebLevel(webLevelRequest)) return false;
            webLevelRequest = -1;
        }
        webPause = false;
        toggleDoomcam |= webDoomcam;
        webDoomcam = false;
        if (webMute >= 0) muteRequest = webMute;
        webMute = -1;
        if (webModelsReady) {
            webModelsReady = false;
            if (GameModelsLoad(&gmodels, "assets/models")) {
                locMdlCount = GetShaderLocation(gmodels.shader, "lightCount");
                locMdlPos = GetShaderLocation(gmodels.shader, "lightPos");
                GameModelsRenderPortraits(&gmodels);
            }
        }
        webRetry = false;
        if (retry) { webTap = false; webBlast = false; }
        if (webTap || webHover) {
            Vector2 normalized = webTap ? webTapPosition : webHoverPosition;
            pointer = (Vector2){ normalized.x * GetScreenWidth(),
                                 normalized.y * GetScreenHeight() };
        }
        bool tapped = webTap;
        placed = webTap && webTool == 0 && (!webAmbush || !TrainingPlaced());
        slam |= webTap && webTool == 1;
        blast |= webTap && webTool == 2;
        webTap = false;
        if (webAmbush) FrameAmbush();
        else {
            CameraUpdate(&vs, dt);
            UpdateWebCamera();
        }
#else
        CameraUpdate(&vs, dt);
#endif
        if (vs.follow) {
            Vector3 want = DgRenderPos(&sim, (float)(simAcc/SIM_TICTIME));
            float k = 1.0f - expf(-dt*5.0f);   // smooth, frame-rate independent
            vs.target = Vector3Add(vs.target, Vector3Scale(Vector3Subtract(want, vs.target), k));
        }
        if (IsKeyPressed(KEY_F1)) { debugHud = !debugHud; LogInput(debugHud ? "debug_on" : "debug_off"); }
        if (howtoFrames > 0) {
            howtoFrames--;
            if (GetKeyPressed() != 0 || IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) howtoFrames = 0;
        }
        bool finished = sim.dg.state == DG_DEAD || sim.dg.state == DG_EXITED ||
                        sim.dg.state == DG_NO_GOAL;
#ifdef HELLDROP_TOUCH
        finished |= webAmbush && trainingCleared;
#endif
        if (togglePause && !finished) paused = !paused;
        if (retry) {
            paused = finished = false;
            LogInput("sim_restart");
#ifdef HELLDROP_TOUCH
            if (!webAmbush) MeltStart(&melt);
#else
            MeltStart(&melt);
#endif
            SimFree(&sim);
            if (!SimInit(&sim, &map, &lm)) return false;
            gaudio.cursor = 0;
            dropMsgFrames = 0;
#ifdef HELLDROP_TOUCH
#ifdef PLATFORM_IOS
            quickAmbushLimit = -1;
            ambushQuoteTic = -1;
            if (!webAmbush) selectedKind = MK_SHOTGUNGUY;
#endif
            if (webAmbush) ResetAmbush();
            else SetupView(&vs, &map);
            webTool = 0;
#endif
            simAcc = 0.0;
            memset(mobYawInit, 0, sizeof(mobYawInit));
        }
#ifdef HELLDROP_TOUCH
        if (webInterrupted && !finished) paused = true;
        webInterrupted = false;
#endif
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
#ifdef HELLDROP_TOUCH
        if (webPick >= 0) wantKind = webPick;
        webPick = -1;
#endif
        if (wantKind >= 0 && wantKind < MK_COUNT) {
            if (unlocked & (1u << wantKind)) {
                selectedKind = (MobKind)wantKind;
#ifdef HELLDROP_TOUCH
                webTool = 0;
#endif
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
                    return false;
                }
                SetupView(&vs, &map);
                unlocked = UnlockMaskFor(curMap);
                if (!(unlocked & (1u << selectedKind))) selectedKind = MK_ZOMBIE;
                simAcc = 0.0;
                intentPrev[0] = '\0';
                SetWindowTitle(TextFormat("HELLDROP - %s", curMap));
                return true;
            }
        }
        if (IsKeyPressed(KEY_TAB)) {
            codexOpen = !codexOpen;
            LogInput(codexOpen ? "codex_open" : "codex_close");
        }
        if (IsKeyPressed(KEY_M) && gmodels.any) {
            gmodels.enabled = !gmodels.enabled;
            LogInput(gmodels.enabled ? "models_on" : "models_off");
        }
        if (muteRequest >= 0) {
            muted = muteRequest;
            if (IsAudioDeviceReady()) SetMasterVolume(muted ? 0.0f : 1.0f);
        }
        if (toggleDoomcam) {
            doomcamOn = !doomcamOn;
            LogInput(doomcamOn ? "doomcam_on" : "doomcam_off");
        }

        Camera3D cam = CameraFromView(&vs);
        Vector2 ground = { 0 };
        bool groundOk = ScreenGroundPoint(&lm, cam, pointer, &ground);
#ifdef HELLDROP_TOUCH
#ifdef PLATFORM_IOS
        if (!webAmbush && !finished && !paused && webTool == 0) {
            if (quickAmbushLimit >= 0 && ambushQuoteTic >= 0) {
                // Resolve the displayed marker before updating it for this frame.
                ground = (Vector2){ambushMarker.x, -ambushMarker.z};
                int cost = SimDropCost(&sim, selectedKind, ground.x, ground.y, NULL);
                if (cost <= quickAmbushLimit) {
                    groundOk = placed = true;
                } else {
                    snprintf(dropMsg, sizeof(dropMsg), "Marine spotted the ring. Check the new price.");
                    dropMsgFrames = 80;
                }
                ambushQuoteTic = -1;
            } else if (placed && !groundOk) {
                snprintf(dropMsg, sizeof(dropMsg), "Tap a floor or use Ambush.");
                dropMsgFrames = 80;
            }
            QuoteAmbush();
        }
        quickAmbushLimit = -1;
#endif
        if (webAmbush) {
            const TrainingLesson *lesson = &training[webLevel];
            slam = TrainingPlaced() && lesson->door && !trainingDoorUsed && (slam || webTool == 1);
            if (slam) {
                ground = (Vector2){ 240, 96 };
                groundOk = true;
                webTool = 0;
            }
            if (TrainingPlaced() && groundOk && sim.barrelCount > 0 &&
                Vector2Distance(ground, sim.barrels[0].pos) < 34.0f)
                blast |= tapped;
            if (webBlast && TrainingPlaced() && sim.barrelCount > 0) {
                ground = sim.barrels[0].pos;
                groundOk = true;
                blast = true;
            }
            if (!TrainingPlaced()) blast = false;
            if (placed && webLevel >= TRAIN_FIRST && groundOk) {
                Vector2 spot = lesson->spots[trainingDrops];
                if (Vector2Distance(ground, spot) <= 47.0f) ground = spot;
                else groundOk = false;
            }
            if (placed && (!groundOk || ground.x < 264)) {
                snprintf(dropMsg, sizeof(dropMsg), "%s", webLevel >= TRAIN_FIRST ? "TAP THE MARKED RING" : "AIM PAST THE DOOR");
                dropMsgFrames = 80;
                placed = false;
            }
        }
        webBlast = false;
#endif
        if (!paused && !finished && groundOk && slam) {
            int cost = 0;
            switch (SimSlamMover(&sim, ground.x, ground.y, &cost)) {
            case VERB_OK:
#ifdef HELLDROP_TOUCH
                if (webAmbush) trainingDoorUsed = true;
#endif
                break;
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
        if (!paused && !finished && groundOk && blast) {
#ifdef HELLDROP_TOUCH
            long damageBefore = sim.dgDamageTaken;
            int blastsBefore = sim.boomHead;
#endif
            switch (SimDetonateBarrel(&sim, ground.x, ground.y)) {
            case VERB_OK:
#ifdef HELLDROP_TOUCH
                if (webAmbush) {
                    ambushBlastDamage = sim.dgDamageTaken - damageBefore;
                    ambushBlastCount = sim.boomHead - blastsBefore;
                }
#endif
                break;
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
        if (!paused && !finished && groundOk && placed) {
#ifdef PLATFORM_IOS
            long spentBefore = sim.soulsSpent;
#endif
            switch (SimTryDrop(&sim, selectedKind, ground.x, ground.y)) {
            case DROP_OK:
#ifdef PLATFORM_IOS
                if (!webAmbush) {
                    snprintf(dropMsg, sizeof(dropMsg), "Demon summoned · %ld souls spent", sim.soulsSpent - spentBefore);
                    dropMsgFrames = 80;
                }
#endif
#ifdef HELLDROP_TOUCH
                if (webAmbush) {
                    trainingDrops++;
                    if (webLevel == TRAIN_CROSSFIRE && !TrainingPlaced()) {
                        selectedKind = MK_IMP;
                        unlocked = 1u << MK_IMP;
                    }
                }
#endif
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
        // bounds --turbo to ~17 tics per rendered frame). In frame-dump mode
        // we ignore wall time entirely and advance exactly `turbo` tics per
        // rendered frame, so captured footage is smooth and reproducible
        // (per-frame PNG export would otherwise balloon dt).
#ifdef HELLDROP_TOUCH
#ifdef PLATFORM_IOS
        if (!webAmbush && !sim.soulsSpent) dt = 0;
#endif
        // A stalled phone frame must not fast-forward past a training cue.
        if (webAmbush) dt = fminf(dt, 0.10f);
        if (webAmbush && (trainingCleared || !TrainingPlaced() ||
                          (training[webLevel].door && !trainingDoorUsed))) dt = 0;
        if (webAmbush && AmbushNearBarrel()) {
            // The first barrel lesson gives the player time to find the button.
            // Chain Reaction and the Gauntlet still require a timed detonation.
            dt *= webLevel == TRAIN_BARREL ? 0.0f : 0.4f;
        }
#endif
        if (!paused) simAcc += opt.dumpDir ? SIM_TICTIME * (double)opt.turbo
                              : (double)dt * (double)opt.turbo;
        if (simAcc > 0.5) simAcc = 0.5;
        while (simAcc >= SIM_TICTIME) {
            SimTick(&sim);
            if (opt.autodrop) AutodropTick(&sim);
            simAcc -= SIM_TICTIME;
        }
        float simAlpha = (float)(simAcc / SIM_TICTIME);

        gaudio.captureTime = (double)frame/35.0*(double)opt.turbo/(double)opt.turbo;   // dump: 1 frame = turbo tics, played back at 35 fps
        GameAudioDrain(&gaudio, &sim, (Vector2){ vs.target.x, -vs.target.z });
        if (levelShader.id) UpdateDynamicLights(&sim, simAlpha);

        // Doomcam pass: his view into a 320x200 target (drawn as PiP, or
        // fullscreen for the death takeover).
        long deathAge = (sim.dg.state == DG_DEAD) ? (sim.tic - sim.dg.deathTic) : -1;
        bool takeover = (deathAge >= 0 && deathAge < 140);
#ifdef HELLDROP_TOUCH
        if (webAmbush) {
            takeover = false;
            bool ready = sim.dg.state != DG_DEAD && sim.dg.state != DG_EXITED &&
                         TrainingPlaced() && AmbushNearBarrel();
            if (ready && !ambushReady && !paused) GameAudioBarrelReady(&gaudio);
            ambushReady = ready;
            // The first two lessons reward the setup itself. Crossfire
            // requires both summons and a hit; the other encounters require a kill.
            if ((webLevel == TRAIN_FIRST && sim.mobCount > 0) ||
                (webLevel == TRAIN_SHADOW && sim.mobCount > 0 && sim.tic >= 35) ||
                (webLevel == TRAIN_CROSSFIRE && TrainingPlaced() && sim.dgDamageTaken > 0)) {
                trainingCleared = true;
            }
            int stage = trainingCleared ? 3 : sim.dg.state == DG_DEAD ? (deathAge < 12 ? 6 : 3)
                       : sim.dg.state == DG_EXITED ? 4
                       : !TrainingPlaced() ? (trainingDrops ? 13 : 0)
                       : training[webLevel].door && !trainingDoorUsed ? 12
                       : sim.barrelCount == 0 ? 10 : sim.barrels[0].alive ? 1 : 2;
            TouchStatus(stage, selectedKind, (int)sim.souls, sim.dg.health, ready,
                      webTool, ambushBlastDamage, webLevel, paused, ambushBlastCount);
        } else {
            int stage = sim.dg.state == DG_DEAD ? 7 : sim.dg.state == DG_EXITED ? 8
                       : sim.dg.state == DG_NO_GOAL ? 9 : 5;
#ifdef PLATFORM_IOS
            if (stage == 5 && !sim.soulsSpent) stage = 14;
#endif
            int next = -1;
            if (stage == 7) {
                bool secret = strcmp(curMap, "E1M3") == 0 && sim.dg.sector >= 0 &&
                              map.sectors[sim.dg.sector].special == 9;
                const char *mapName = NextCampaignMap(curMap, secret);
                if (mapName) next = mapName[3] - '0' + 1;
            }
            TouchStatus(stage, selectedKind, (int)sim.souls, sim.dg.health, 0, webTool,
                      -1, webLevel, paused, 0);
            long barrelTics = sim.verbReadyTic[VERB_BARREL] - sim.tic;
            long doorTics = sim.verbReadyTic[VERB_DOOR] - sim.tic;
            TouchEpisodeStatus(next, stage == 7 ? (int)sim.dg.finishTic : -1,
                             barrelTics > 0 ? (int)((barrelTics + SIM_TICRATE - 1)/SIM_TICRATE) : 0,
                             doorTics > 0 ? (int)((doorTics + SIM_TICRATE - 1)/SIM_TICRATE) : 0);
        }
#endif
        bool compactHud = false;
#ifdef PLATFORM_IOS
        compactHud = true;
#elif defined(PLATFORM_WEB)
        compactHud = GetScreenWidth() < 640;
#endif
        if (doomcamOn || takeover) {
            Camera3D fp = DoomcamFrom(&sim, simAlpha);
            BeginTextureMode(doomcamRT);
            ClearBackground((Color){ 10, 10, 12, 255 });
            gViewW = 320; gViewH = 200;
            DrawWorld(&sim, &lm, &map, thingHeights, &gsprites, fp, simAlpha, false);
            EndTextureMode();
        }

        BeginDrawing();
        ClearBackground((Color){ 12, 12, 14, 255 });
        gViewW = GetScreenWidth(); gViewH = GetScreenHeight();
        DrawWorld(&sim, &lm, &map, thingHeights, &gsprites, cam, simAlpha, true);
        BeginMode3D(cam);
        DrawRoute(&sim, simAlpha);
        DrawPendingDrops(&sim, frame);
        for (int i = 0; i < sim.boomHead && i < BOOMS_MAX; i++) {
            const Boom *boom = &sim.booms[i];
            if (boom->kind != BOOM_BARREL) continue;
            float age = (float)(sim.tic - boom->tic) + simAlpha;
            if (age < 0.0f || age >= 13.0f) continue;
            Color ember = { 255, 163, 77, (unsigned char)(210.0f*(1.0f - age/13.0f)) };
            DrawCircle3D((Vector3){ boom->pos.x, boom->z - 17.0f, -boom->pos.y },
                         22.0f + 8.0f*age, (Vector3){ 1, 0, 0 }, 90, ember);
        }
#ifdef HELLDROP_TOUCH
        if (webAmbush && sim.dg.state != DG_DEAD && sim.dg.state != DG_EXITED) {
            Vector3 marine = DgRenderPos(&sim, simAlpha);
            marine.y += 3;
            DrawCircle3D(marine, 23, (Vector3){ 1, 0, 0 }, 90, SKYBLUE);
        }
#ifdef PLATFORM_IOS
        if (!webAmbush && !finished && webTool == 0 && ambushQuoteTic >= 0) {
            DrawCircle3D(ambushMarker, 30, (Vector3){1, 0, 0}, 90, LIME);
            DrawCircle3D(ambushMarker, 34, (Vector3){1, 0, 0}, 90, LIME);
        }
#endif
        if (webAmbush && !TrainingPlaced()) {
            Vector2 spot = training[webLevel].spots[trainingDrops];
            Vector3 marker = { spot.x, 3, -spot.y };
            DrawCircle3D(marker, 28, (Vector3){ 1, 0, 0 }, 90, LIME);
            DrawCircle3D(marker, 31, (Vector3){ 1, 0, 0 }, 90, (Color){ 124, 242, 121, 120 });
            DrawLine3D((Vector3){ marker.x - 6, marker.y, marker.z },
                       (Vector3){ marker.x + 6, marker.y, marker.z }, LIME);
            DrawLine3D((Vector3){ marker.x, marker.y, marker.z - 6 },
                       (Vector3){ marker.x, marker.y, marker.z + 6 }, LIME);
        }
        if (webAmbush && TrainingPlaced()) {
            if (training[webLevel].door && !trainingDoorUsed) {
                DrawLine3D((Vector3){ 224, 96, -48 }, (Vector3){ 224, 96, -144 }, ORANGE);
                DrawLine3D((Vector3){ 256, 96, -48 }, (Vector3){ 256, 96, -144 }, ORANGE);
            }
            for (int i = 0; i < sim.barrelCount; i++) {
                const Barrel *barrel = &sim.barrels[i];
                if (!barrel->alive) continue;
                DrawCircle3D((Vector3){ barrel->pos.x, barrel->z + 3, -barrel->pos.y }, 27,
                             (Vector3){ 1, 0, 0 }, 90, ambushReady ? RED : ORANGE);
            }
        }
#endif
        bool cursorHot = false;
        int cursorCost = 0;
        bool cursorAfford = false;
        bool showDropCursor = groundOk;
#ifdef HELLDROP_TOUCH
        showDropCursor &= webHover && webTool == 0 && (!webAmbush || !TrainingPlaced());
#endif
        if (showDropCursor) {
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
            int width = compactHud ? GetScreenWidth()/3 : 320;
            int height = width*5/8;
            int px = compactHud ? 6 : 12, py = GetScreenHeight() - height - px;
            DrawTexturePro(doomcamRT.texture,
                           (Rectangle){ 0, 0, 320, -200 },
                           (Rectangle){ (float)px, (float)py, (float)width, (float)height },
                           (Vector2){ 0, 0 }, 0.0f, WHITE);
            DrawRectangleLines(px - 1, py - 1, width + 2, height + 2, (Color){ 90, 90, 96, 255 });
            DrawTextShadowed("DOOMCAM", px + 6, py + 4, 14, (Color){ 200, 60, 50, 255 });
        }
#ifdef HELLDROP_TOUCH
        if (webAmbush && sim.dg.state != DG_DEAD && sim.dg.state != DG_EXITED) {
            Vector3 marine = DgRenderPos(&sim, simAlpha);
            marine.y += 60.0f;
            Vector2 label = GetWorldToScreen(marine, cam);
            int size = 15;
            DrawTextShadowed("MARINE", (int)label.x - MeasureText("MARINE", size)/2,
                             (int)label.y, size, SKYBLUE);
        }
        if (!webAmbush)
#endif
        {
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
            if (!compactHud) {
                DrawHud(&map, PathBasename(opt.wadPath), &sim, opt.turbo,
                        campLabel, winHint);
                DrawSoulsPanel(&sim, selectedKind, unlocked, &gsprites);
                FaceStateUpdate(&faceState, &sim);
                DrawStatusBar(&hud, &sim, &faceState, frame);
            }
            if (codexOpen) DrawCodex();
            DrawHowTo(howtoFrames);
            if (campaignComplete) DrawCampaignComplete();
        }
        if (groundOk && cursorCost > 0) {
            Vector2 mp = pointer;
            Color tcol = !cursorAfford ? (Color){ 255, 80, 70, 255 }
                       : cursorHot     ? (Color){ 255, 180, 80, 255 }
                                       : (Color){ 160, 255, 170, 255 };
            const char *quote = TextFormat("%s  %d souls", cursorHot ? "Spotted" : "Hidden", cursorCost);
            int x = (int)mp.x + 18;
            int quoteWidth = MeasureText(quote, 18);
            if (x + quoteWidth >= GetScreenWidth()) x = (int)mp.x - quoteWidth - 8;
            DrawTextShadowed(quote, x, (int)mp.y - 6, 18, tcol);
        }
#ifdef HELLDROP_TOUCH
        TouchNotice(dropMsgFrames > 0 ? dropMsg : "");
#endif
        if (dropMsgFrames > 0) {
            dropMsgFrames--;
#ifndef HELLDROP_TOUCH
            int w = MeasureText(dropMsg, 24);
            DrawTextShadowed(dropMsg, GetScreenWidth()/2 - w/2,
                             GetScreenHeight() - 110, 24,
                             (Color){ 255, 80, 70, 255 });
#endif
        }
        if (paused) {
            const char *label = "PAUSED";
#ifndef HELLDROP_TOUCH
            label = "PAUSED  |  P TO RESUME";
#endif
            DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), (Color){ 0, 0, 0, 120 });
            DrawTextShadowed(label, (GetScreenWidth() - MeasureText(label, 26))/2,
                             GetScreenHeight()/2 - 13, 26, RAYWHITE);
        }
        MeltDraw(&melt);
        frameWorkMs = (GetTime() - frameWorkStart)*1000.0;
        EndDrawing();

#ifndef HELLDROP_TOUCH
        if (opt.dumpVideo) {
            static FILE *pipe;
            Image shot = LoadImageFromScreen();
            if (!pipe) {
                char cmd[2048];
                snprintf(cmd, sizeof(cmd),
                         "ffmpeg -v error -y -f rawvideo -pix_fmt rgba -s %dx%d -r 35 -i - "
                         "-vf scale=1920:1080:flags=lanczos,format=yuv420p "
                         "-c:v libx264 -preset veryfast -crf 14 '%s'",
                         shot.width, shot.height, opt.dumpVideo);
                pipe = popen(cmd, "w");
                TraceLog(LOG_INFO, "MAIN: streaming %dx%d frames to %s", shot.width, shot.height, opt.dumpVideo);
            }
            if (pipe) fwrite(shot.data, 4, (size_t)shot.width*shot.height, pipe);
            UnloadImage(shot);
            if (opt.exitAfter > 0 && frame + 1 >= opt.exitAfter && pipe) { pclose(pipe); pipe = NULL; }
        }
#endif
        // Media capture: a numbered PNG per (Nth) frame for stitching into
        // GIF/MP4 with ffmpeg. Deterministic, so footage is reproducible.
        if (opt.dumpDir && (frame % opt.dumpEvery == 0)) {
            Image shot = LoadImageFromScreen();
            char path[1024];
            snprintf(path, sizeof(path), "%s/frame_%05ld.png",
                     opt.dumpDir, frame / opt.dumpEvery);
            ExportImage(shot, path);
            UnloadImage(shot);
        }

        if (frame % 12 == 0) LogSnapshot(&vs, &sim);
        {   // perf: wall ms per frame, summarized once a second
            static double last, acc, worst, winStart, workAcc, workWorst; static int n;
            workAcc += frameWorkMs;
            if (frameWorkMs > workWorst) workWorst = frameWorkMs;
            double now = GetTime();
            if (last > 0) { double ms = (now - last)*1000.0; acc += ms; n++; if (ms > worst) worst = ms; }
            last = now;
            if (winStart == 0) winStart = now;
            if (now - winStart >= 1.0 && n > 0) {
                if (StateLogEnabled()) {
                    StateLogBegin("perf");
                    StateLogFloat("fps", n/(now - winStart));
                    StateLogFloat("ms_avg", acc/n);
                    StateLogFloat("ms_max", worst);
                    // CPU work per frame before the swap: what a hidden test
                    // window's WindowServer-bound present can't distort.
                    StateLogFloat("work_ms_avg", workAcc/n);
                    StateLogFloat("work_ms_max", workWorst);
                    StateLogInt("mobs", sim.mobCount);
                    StateLogEnd();
                }
                acc = worst = workAcc = workWorst = 0; n = 0; winStart = now;
            }
        }
        frame++;

        if (opt.selftest && frame >= ((opt.exitAfter > 0) ? opt.exitAfter : 60)) {
            TakeScreenshot("selftest.png");
            if (StateLogEnabled()) {
                StateLogBegin("selftest_done");
                StateLogStr("screenshot", "selftest.png");
                StateLogEnd();
            }
            return false;
        }
        if (!opt.selftest && opt.exitAfter > 0 && frame >= opt.exitAfter) return false;
        return true;
}

static void MainCleanup(void)
{
    LogShutdown(frame, sim.tic);
    StateLogClose();
    if (melt.tex.id != 0) UnloadTexture(melt.tex);
    UnloadRenderTexture(doomcamRT);
    GameAudioUnload(&gaudio);
    CloseAudioDevice();
    GameModelsUnload(&gmodels);
    if (levelShader.id) UnloadShader(levelShader);
    GameSpritesUnload(&gsprites);
    DoomHudUnload(&hud);
    SimFree(&sim);
    free(thingHeights);
    LevelMeshFree(&lm);
    MapFree(&map);
    WadFree(&wad);
    CloseWindow();
}

#ifdef PLATFORM_IOS
bool HelldropIOSFrame(void)
{
    if (HelldropFrame()) return true;
    MainCleanup();
    return false;
}
#elif defined(PLATFORM_WEB)
static void HelldropFrameWeb(void)
{
    // No native window-close button on web; in practice HelldropFrame never
    // returns false here (selftest / exit-after are CLI-only), so this runs
    // for the lifetime of the page.
    if (WindowShouldClose() || !HelldropFrame()) {
        MainCleanup();
        emscripten_cancel_main_loop();
    }
}
#endif
