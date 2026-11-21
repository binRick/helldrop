#include "levelmesh.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raymath.h"
#include "rlgl.h"
#include "tesselator.h"
#include "wadtex.h"

// Sector light scales base colors but never below this, so nothing is
// pitch black under backface-culled flat shading.
#define LIGHT_MUL_FLOOR 0.15f

static int ClampInt(int v, int lo, int hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

static float LightMul(int light)
{
    float m = (float)ClampInt(light, 0, 255) / 255.0f;
    return (m < LIGHT_MUL_FLOOR) ? LIGHT_MUL_FLOOR : m;
}

static Color Shade(Color base, float m)
{
    return (Color){
        (unsigned char)((float)base.r * m),
        (unsigned char)((float)base.g * m),
        (unsigned char)((float)base.b * m),
        255,
    };
}

static Color FloorBaseColor(const Sector *sec, int sectorIdx)
{
    const char *f = sec->floorFlat;
    if (strncmp(f, "NUKAGE", 6) == 0) return (Color){ 99, 167, 62, 255 };
    if (strncmp(f, "FWATER", 6) == 0 ||
        strncmp(f, "WATER", 5) == 0)  return (Color){ 58, 108, 188, 255 };
    if (strncmp(f, "LAVA", 4) == 0)   return (Color){ 216, 88, 26, 255 };
    if (strncmp(f, "BLOOD", 5) == 0)  return (Color){ 128, 26, 26, 255 };
    if (strncmp(f, "F_SKY", 5) == 0)  return (Color){ 14, 14, 18, 255 };
    // Warm mid-gray; deterministic per-sector jitter so adjacent sectors read.
    unsigned h = (unsigned)sectorIdx * 2654435761u;
    int j = (int)((h >> 24) % 13u) - 6;
    return (Color){
        (unsigned char)ClampInt(152 + j, 0, 255),
        (unsigned char)ClampInt(145 + j, 0, 255),
        (unsigned char)ClampInt(134 + j, 0, 255),
        255,
    };
}

// Growable non-indexed triangle soup (load-time only; meshes draw as-is).
typedef struct GeoBuf {
    float *pos;          // xyz per vertex
    unsigned char *col;  // rgba per vertex
    int count, cap;
} GeoBuf;

static bool GeoPushTri(GeoBuf *g, Vector3 a, Vector3 b, Vector3 c, Color col)
{
    if (g->count + 3 > g->cap) {
        int cap = (g->cap > 0) ? g->cap : 4096;
        while (cap < g->count + 3) cap *= 2;
        float *p = (float *)realloc(g->pos, (size_t)cap * 3 * sizeof(float));
        if (!p) return false;
        g->pos = p;
        unsigned char *cl = (unsigned char *)realloc(g->col, (size_t)cap * 4);
        if (!cl) return false;
        g->col = cl;
        g->cap = cap;
    }
    const Vector3 v[3] = { a, b, c };
    for (int i = 0; i < 3; i++) {
        float *pp = g->pos + (size_t)(g->count + i) * 3;
        pp[0] = v[i].x; pp[1] = v[i].y; pp[2] = v[i].z;
        unsigned char *cp = g->col + (size_t)(g->count + i) * 4;
        cp[0] = col.r; cp[1] = col.g; cp[2] = col.b; cp[3] = col.a;
    }
    g->count += 3;
    return true;
}

static void GeoToMesh(GeoBuf *g, Mesh *m)
{
    if (g->count == 0) {
        free(g->pos);
        free(g->col);
        g->pos = NULL;
        g->col = NULL;
    } else if (g->cap > g->count) {
        float *p = (float *)realloc(g->pos, (size_t)g->count * 3 * sizeof(float));
        if (p) g->pos = p;
        unsigned char *c = (unsigned char *)realloc(g->col, (size_t)g->count * 4);
        if (c) g->col = c;
    }
    m->vertices = g->pos;
    m->colors = g->col;
    m->vertexCount = g->count;
    m->triangleCount = g->count / 3;
}

// ---- Textured render geometry (grouped per WAD texture/flat) ----------
typedef struct TGeo {
    float *pos, *uv;
    unsigned char *col;
    int count, cap;
} TGeo;

typedef struct TexBuild {
    LevelTexSlot *slots;
    TGeo *geo;
    int n, cap;
} TexBuild;

static TexBuild *g_tb;   // non-NULL while building textured geometry
static const WadTex *g_wt;

static int TexSlot(const char *name, bool flat)
{
    TexBuild *tb = g_tb;
    if (!tb) return -1;
    char nm[9] = { 0 };
    for (int i = 0; i < 8 && name[i]; i++) nm[i] = (name[i] >= 'a' && name[i] <= 'z') ? (char)(name[i] - 32) : name[i];
    for (int i = 0; i < tb->n; i++) {
        if (tb->slots[i].flat == flat && strncmp(tb->slots[i].name, nm, 8) == 0) return i;
    }
    if (tb->n == tb->cap) {
        int cap = tb->cap ? tb->cap*2 : 64;
        LevelTexSlot *s = (LevelTexSlot *)realloc(tb->slots, (size_t)cap*sizeof(LevelTexSlot));
        if (!s) return -1;
        tb->slots = s;
        TGeo *g = (TGeo *)realloc(tb->geo, (size_t)cap*sizeof(TGeo));
        if (!g) return -1;
        tb->geo = g;
        tb->cap = cap;
    }
    memset(&tb->slots[tb->n], 0, sizeof(LevelTexSlot));
    memset(&tb->geo[tb->n], 0, sizeof(TGeo));
    memcpy(tb->slots[tb->n].name, nm, 9);
    tb->slots[tb->n].flat = flat;
    tb->slots[tb->n].w = 64;
    tb->slots[tb->n].h = flat ? 64 : 128;
    if (!flat && g_wt) {
        int d = WadTexFind(g_wt, nm);
        tb->slots[tb->n].def = d;
        if (d >= 0) { tb->slots[tb->n].w = g_wt->defs[d].width; tb->slots[tb->n].h = g_wt->defs[d].height; }
    }
    return tb->n++;
}

static bool TGeoPush(TGeo *g, const Vector3 p[3], const Vector2 t[3], const Color c[3])
{
    if (g->count + 3 > g->cap) {
        int cap = g->cap ? g->cap*2 : 384;
        while (cap < g->count + 3) cap *= 2;
        float *np = (float *)realloc(g->pos, (size_t)cap*3*sizeof(float));
        if (!np) return false;
        g->pos = np;
        float *nu = (float *)realloc(g->uv, (size_t)cap*2*sizeof(float));
        if (!nu) return false;
        g->uv = nu;
        unsigned char *nc = (unsigned char *)realloc(g->col, (size_t)cap*4);
        if (!nc) return false;
        g->col = nc;
        g->cap = cap;
    }
    for (int i = 0; i < 3; i++) {
        int k = g->count + i;
        g->pos[k*3] = p[i].x; g->pos[k*3 + 1] = p[i].y; g->pos[k*3 + 2] = p[i].z;
        g->uv[k*2] = t[i].x; g->uv[k*2 + 1] = t[i].y;
        g->col[k*4] = c[i].r; g->col[k*4 + 1] = c[i].g; g->col[k*4 + 2] = c[i].b; g->col[k*4 + 3] = 255;
    }
    g->count += 3;
    return true;
}

// Light as a vertex tint over the texture. Brighter than the flat-shaded
// path (textures carry their own darkness) but sector light still reads:
// dark sectors are ambush terrain and must look it.
static Color TexLight(int light, float extra)
{
    float m = LightMul(light)*1.18f*extra;
    if (m > 1.0f) m = 1.0f;
    if (m < 0.10f) m = 0.10f;
    unsigned char v = (unsigned char)(m*255.0f);
    return (Color){ v, v, v, 255 };
}

static int SectorOfSide(const DoomMap *map, int side)
{
    return (side >= 0) ? map->sides[side].sector : -1;
}

typedef struct IntBuf {
    int *v;
    int count, cap;
} IntBuf;

static bool IntPush(IntBuf *b, int x)
{
    if (b->count + 1 > b->cap) {
        int cap = (b->cap > 0) ? b->cap * 2 : 2048;
        int *p = (int *)realloc(b->v, (size_t)cap * sizeof(int));
        if (!p) return false;
        b->v = p;
        b->cap = cap;
    }
    b->v[b->count++] = x;
    return true;
}

typedef struct BEdge {
    int a, b;            // directed: sector interior on the left of a->b
    bool used;
} BEdge;

static int g_floorSlot = -1;
static Color g_floorCol;

static bool EmitSectorFloor(GeoBuf *g, IntBuf *tri2sec, int sectorIdx,
                            TESStesselator *tess, float floorH, Color col)
{
    int ne = tessGetElementCount(tess);
    const TESSreal *tv = tessGetVertices(tess);
    const TESSindex *el = tessGetElements(tess);
    for (int e = 0; e < ne; e++) {
        TESSindex i0 = el[e*3 + 0], i1 = el[e*3 + 1], i2 = el[e*3 + 2];
        if (i0 == TESS_UNDEF || i1 == TESS_UNDEF || i2 == TESS_UNDEF) continue;
        if (!IntPush(tri2sec, sectorIdx)) return false;
        float x0 = tv[i0*2], y0 = tv[i0*2 + 1];
        float x1 = tv[i1*2], y1 = tv[i1*2 + 1];
        float x2 = tv[i2*2], y2 = tv[i2*2 + 1];
        // CCW in Doom xy maps to an upward (+h) normal under (x, h, -y).
        if ((x1 - x0)*(y2 - y0) - (x2 - x0)*(y1 - y0) < 0.0f) {
            float tx = x1, ty = y1;
            x1 = x2; y1 = y2;
            x2 = tx; y2 = ty;
        }
        if (!GeoPushTri(g, (Vector3){ x0, floorH, -y0 },
                           (Vector3){ x1, floorH, -y1 },
                           (Vector3){ x2, floorH, -y2 }, col)) return false;
        if (g_tb && g_floorSlot >= 0) {
            const Vector3 p[3] = { { x0, floorH, -y0 }, { x1, floorH, -y1 }, { x2, floorH, -y2 } };
            const Vector2 t[3] = { { x0/64.0f, -y0/64.0f }, { x1/64.0f, -y1/64.0f }, { x2/64.0f, -y2/64.0f } };
            const Color c[3] = { g_floorCol, g_floorCol, g_floorCol };
            if (!TGeoPush(&g_tb->geo[g_floorSlot], p, t, c)) return false;
        }
    }
    return true;
}

// Lift sectors render their floor at the LOW position: the sim draws the
// moving platform cap on top, so the static slab must never float above it.
// (Detection mirrors sim.c's BuildLifts: tagged sectors of plat specials.)
static float FloorRenderHeight(const DoomMap *map, int s)
{
    static const int PLATS[] = { 10, 21, 62, 88, 120, 121, 122, 123 };
    const Sector *sec = &map->sectors[s];
    if (sec->tag <= 0) return sec->floorH;
    bool isLift = false;
    for (int i = 0; i < map->lineCount && !isLift; i++) {
        if (map->lines[i].tag != sec->tag) continue;
        for (size_t k = 0; k < sizeof(PLATS)/sizeof(PLATS[0]); k++) {
            if (map->lines[i].special == PLATS[k]) { isLift = true; break; }
        }
    }
    if (!isLift) return sec->floorH;
    float lowest = sec->floorH;
    for (int i = 0; i < map->lineCount; i++) {
        const Linedef *l = &map->lines[i];
        if (l->sideFront < 0 || l->sideBack < 0) continue;
        int fs = map->sides[l->sideFront].sector;
        int bs = map->sides[l->sideBack].sector;
        int other = (fs == s) ? bs : ((bs == s) ? fs : -1);
        if (other < 0 || other == s) continue;
        if (map->sectors[other].floorH < lowest) lowest = map->sectors[other].floorH;
    }
    return lowest;
}

static bool BuildFloors(GeoBuf *g, IntBuf *tri2sec, const DoomMap *map)
{
    int S = map->sectorCount;
    int *start = (int *)calloc((size_t)S + 1, sizeof(int));
    int *fill = (int *)malloc((size_t)S * sizeof(int));
    if (!start || !fill) {
        free(start);
        free(fill);
        return false;
    }

    for (int i = 0; i < map->lineCount; i++) {
        const Linedef *l = &map->lines[i];
        int fs = SectorOfSide(map, l->sideFront);
        int bs = SectorOfSide(map, l->sideBack);
        if (fs == bs) continue;   // self-referencing (or sideless): no boundary
        if (fs >= 0) start[fs + 1]++;
        if (bs >= 0) start[bs + 1]++;
    }
    for (int s = 0; s < S; s++) start[s + 1] += start[s];
    int total = start[S];

    BEdge *edges = (total > 0) ? (BEdge *)malloc((size_t)total * sizeof(BEdge)) : NULL;
    if (total > 0 && !edges) {
        free(start);
        free(fill);
        return false;
    }
    memcpy(fill, start, (size_t)S * sizeof(int));
    for (int i = 0; i < map->lineCount; i++) {
        const Linedef *l = &map->lines[i];
        int fs = SectorOfSide(map, l->sideFront);
        int bs = SectorOfSide(map, l->sideBack);
        if (fs == bs) continue;
        // Front side lies right of v1->v2, so v2->v1 keeps fs on the left.
        if (fs >= 0) edges[fill[fs]++] = (BEdge){ l->v2, l->v1, false };
        if (bs >= 0) edges[fill[bs]++] = (BEdge){ l->v1, l->v2, false };
    }

    int maxPer = 0;
    for (int s = 0; s < S; s++) {
        int n = start[s + 1] - start[s];
        if (n > maxPer) maxPer = n;
    }
    float *coords = (maxPer > 0) ? (float *)malloc((size_t)maxPer * 2 * sizeof(float)) : NULL;
    int *chain = (maxPer > 0) ? (int *)malloc((size_t)maxPer * sizeof(int)) : NULL;
    bool ok = (maxPer == 0) || (coords && chain);

    for (int s = 0; s < S && ok; s++) {
        BEdge *E = edges + start[s];
        int n = start[s + 1] - start[s];
        if (n == 0) continue;
        const Sector *sec = &map->sectors[s];
        Color col = Shade(FloorBaseColor(sec, s), LightMul(sec->light));
        TESStesselator *tess = NULL;
        int contours = 0, dangling = 0;

        for (int i = 0; i < n && ok; i++) {
            if (E[i].used) continue;
            int len = 0;
            chain[len++] = i;
            E[i].used = true;
            int startV = E[i].a;
            int cur = E[i].b;
            while (cur != startV) {
                int next = -1;
                for (int j = 0; j < n; j++) {
                    if (!E[j].used && E[j].a == cur) { next = j; break; }
                }
                if (next < 0) break;
                E[next].used = true;
                chain[len++] = next;
                cur = E[next].b;
            }
            if (cur != startV) {   // open chain: malformed boundary, drop it
                dangling += len;
                continue;
            }
            if (len < 3) continue;
            double area2 = 0.0;
            for (int k = 0; k < len; k++) {
                Vector2 pa = map->verts[E[chain[k]].a];
                Vector2 pb = map->verts[E[chain[k]].b];
                area2 += (double)pa.x * pb.y - (double)pb.x * pa.y;
            }
            if (fabs(area2) < 0.01) continue;
            for (int k = 0; k < len; k++) {
                Vector2 p = map->verts[E[chain[k]].a];
                coords[k*2 + 0] = p.x;
                coords[k*2 + 1] = p.y;
            }
            if (!tess) {
                tess = tessNewTess(NULL);
                if (!tess) { ok = false; break; }
            }
            tessAddContour(tess, 2, coords, 2 * (int)sizeof(float), len);
            contours++;
        }

        if (dangling > 0) {
            TraceLog(LOG_WARNING,
                     "LEVELMESH: sector %d: %d boundary edge(s) never close a loop, skipped",
                     s, dangling);
        }
        if (ok && contours > 0) {
            if (g_tb) {
                g_floorSlot = TexSlot(sec->floorFlat, true);
                g_floorCol = TexLight(sec->light, 1.0f);
            }
            if (tessTesselate(tess, TESS_WINDING_POSITIVE, TESS_POLYGONS, 3, 2, NULL)) {
                ok = EmitSectorFloor(g, tri2sec, s, tess,
                                     FloorRenderHeight(map, s), col);
            } else {
                TraceLog(LOG_WARNING,
                         "LEVELMESH: sector %d: floor tessellation failed, skipped", s);
            }
        }
        if (tess) tessDeleteTess(tess);
    }

    free(coords);
    free(chain);
    free(edges);
    free(fill);
    free(start);
    return ok;
}

static bool BuildWalls(GeoBuf *g, const DoomMap *map)
{
    const Color wallBase = { 106, 112, 124, 255 };
    for (int i = 0; i < map->lineCount; i++) {
        const Linedef *l = &map->lines[i];
        int fs = SectorOfSide(map, l->sideFront);
        int bs = SectorOfSide(map, l->sideBack);
        float lo, hi;
        int light;
        if (fs >= 0 && bs >= 0) {
            // Two-sided: lower step only; uppers/middles would hide the
            // interior from the iso camera.
            float ff = map->sectors[fs].floorH;
            float bf = map->sectors[bs].floorH;
            if (ff == bf) continue;
            lo = (ff < bf) ? ff : bf;
            hi = (ff < bf) ? bf : ff;
            light = map->sectors[fs].light;
        } else if (fs >= 0 || bs >= 0) {
            const Sector *sec = &map->sectors[(fs >= 0) ? fs : bs];
            lo = sec->floorH;
            hi = sec->ceilH;
            light = sec->light;
            if (hi <= lo) continue;
        } else {
            continue;
        }
        Vector2 a = map->verts[l->v1];
        Vector2 b = map->verts[l->v2];
        if (a.x == b.x) light += 16;        // Doom fake contrast
        else if (a.y == b.y) light -= 16;
        Color col = Shade(wallBase, LightMul(light));
        if (g_tb) {
            // Which sidedef's texture faces the camera-visible step/wall,
            // and Doom's vertical alignment (pegging) for it.
            int sideIdx = l->sideFront;
            bool flip = false;
            const char *texName = NULL;
            float vTop = hi;
            if (fs >= 0 && bs >= 0) {
                bool frontLow = map->sectors[fs].floorH < map->sectors[bs].floorH;
                sideIdx = frontLow ? l->sideFront : l->sideBack;
                flip = !frontLow;
                const Sidedef *sd = &map->sides[sideIdx];
                texName = sd->lower;
                int own = frontLow ? fs : bs;
                if (l->flags & 0x10) vTop = map->sectors[own].ceilH;   // lower unpegged
            } else {
                if (sideIdx < 0) { sideIdx = l->sideBack; flip = true; }
                const Sidedef *sd = &map->sides[sideIdx];
                texName = sd->middle;
                if (texName[0] == '-' || !texName[0]) texName = sd->lower[0] != '-' ? sd->lower : sd->upper;
            }
            const Sidedef *sd = &map->sides[sideIdx];
            int slot = TexSlot((texName && texName[0] && texName[0] != '-') ? texName : "-", false);
            if (slot >= 0) {
                LevelTexSlot *ts = &g_tb->slots[slot];
                float tw = ts->w > 0 ? (float)ts->w : 64.0f, th = ts->h > 0 ? (float)ts->h : 128.0f;
                if (!(fs >= 0 && bs >= 0) && (l->flags & 0x10)) vTop = lo + th;   // middle, lower unpegged
                float len = Vector2Distance(a, b);
                float u0 = sd->xoff/tw, u1 = (sd->xoff + len)/tw;
                if (flip) { float t = u0; u0 = u1; u1 = t; }
                float vLo = (vTop - lo + sd->yoff)/th, vHi = (vTop - hi + sd->yoff)/th;
                float tall = hi - lo;
                // Soft contact shadow: wall bases darker than their tops.
                float baseMul = (tall >= 40.0f) ? 0.58f : 0.80f;
                Color cTop = TexLight(light, 1.0f), cBot = TexLight(light, baseMul);
                Vector3 A0 = { a.x, lo, -a.y }, B0 = { b.x, lo, -b.y };
                Vector3 A1 = { a.x, hi, -a.y }, B1 = { b.x, hi, -b.y };
                Vector2 tA0 = { u0, vLo }, tB0 = { u1, vLo }, tA1 = { u0, vHi }, tB1 = { u1, vHi };
                TGeo *tg = &g_tb->geo[slot];
                const Vector3 q1[3] = { A0, B0, B1 }, q2[3] = { A0, B1, A1 };
                const Vector3 q3[3] = { A0, B1, B0 }, q4[3] = { A0, A1, B1 };
                const Vector2 t1[3] = { tA0, tB0, tB1 }, t2[3] = { tA0, tB1, tA1 };
                const Vector2 t3[3] = { tA0, tB1, tB0 }, t4[3] = { tA0, tA1, tB1 };
                const Color c1[3] = { cBot, cBot, cTop }, c2[3] = { cBot, cTop, cTop };
                const Color c3[3] = { cBot, cTop, cBot }, c4[3] = { cBot, cTop, cTop };
                if (!TGeoPush(tg, q1, t1, c1) || !TGeoPush(tg, q2, t2, c2) ||
                    !TGeoPush(tg, q3, t3, c3) || !TGeoPush(tg, q4, t4, c4)) return false;
            }
        }
        Vector3 a0 = { a.x, lo, -a.y }, b0 = { b.x, lo, -b.y };
        Vector3 a1 = { a.x, hi, -a.y }, b1 = { b.x, hi, -b.y };
        // Both windings so walls render from either side with culling on.
        if (!GeoPushTri(g, a0, b0, b1, col) ||
            !GeoPushTri(g, a0, b1, a1, col) ||
            !GeoPushTri(g, a0, b1, b0, col) ||
            !GeoPushTri(g, a0, a1, b1, col)) return false;
    }
    return true;
}

// Non-static but deliberately absent from the public header: the CPU half of
// LevelMeshBuild, callable from headless tests that have no GL context.
bool LevelMeshGenerate(LevelMesh *lm, const DoomMap *map);

bool LevelMeshGenerate(LevelMesh *lm, const DoomMap *map)
{
    memset(lm, 0, sizeof(*lm));
    if (!map || map->sectorCount <= 0 || map->vertCount <= 0) {
        TraceLog(LOG_ERROR, "LEVELMESH: no map data");
        return false;
    }
    GeoBuf fg = { 0 }, wg = { 0 };
    IntBuf ts = { 0 };
    if (!BuildFloors(&fg, &ts, map) || !BuildWalls(&wg, map)) {
        free(fg.pos);
        free(fg.col);
        free(wg.pos);
        free(wg.col);
        free(ts.v);
        memset(lm, 0, sizeof(*lm));
        TraceLog(LOG_ERROR, "LEVELMESH: %s: out of memory", map->name);
        return false;
    }
    GeoToMesh(&fg, &lm->floors);
    GeoToMesh(&wg, &lm->walls);
    if (ts.count > 0 && ts.cap > ts.count) {
        int *p = (int *)realloc(ts.v, (size_t)ts.count * sizeof(int));
        if (p) ts.v = p;
    }
    lm->floorTriSector = ts.v;
    if (ts.count != lm->floors.triangleCount) {
        TraceLog(LOG_WARNING, "LEVELMESH: %s: tri/sector map mismatch (%d vs %d)",
                 map->name, ts.count, lm->floors.triangleCount);
    }
    return true;
}

// Animated flats cycle like vanilla (every 8 tics): NUKAGE1..3 etc.
static int FlatAnimFrames(const char *name, char frames[4][9])
{
    static const struct { const char *base; int n; } SEQ[] = {
        { "NUKAGE", 3 }, { "FWATER", 4 }, { "LAVA", 4 }, { "BLOOD", 3 },
        { "SWATER", 4 }, { "SLIME0", 3 },
    };
    for (size_t s = 0; s < sizeof(SEQ)/sizeof(SEQ[0]); s++) {
        size_t bl = strlen(SEQ[s].base);
        if (strncmp(name, SEQ[s].base, bl) != 0) continue;
        if (name[bl] < '1' || name[bl] > '9') continue;
        for (int i = 0; i < SEQ[s].n; i++) snprintf(frames[i], 9, "%s%d", SEQ[s].base, i + 1);
        return SEQ[s].n;
    }
    return 0;
}

static Texture2D UploadLevelTexture(Image img)
{
    Texture2D t = LoadTextureFromImage(img);
    if (t.id == 0) return t;
    GenTextureMipmaps(&t);
    // Crisp texels up close (it's Doom), no shimmer when zoomed out.
    SetTextureFilter(t, TEXTURE_FILTER_TRILINEAR);
    rlTextureParameters(t.id, RL_TEXTURE_MAG_FILTER, RL_TEXTURE_FILTER_NEAREST);
    rlTextureParameters(t.id, RL_TEXTURE_FILTER_ANISOTROPIC, 8);
    return t;
}

static bool LoadSlotTexture(const WadTex *wt, LevelTexSlot *s)
{
    Image img = { 0 };
    bool ok = false;
    if (s->flat) {
        char frames[4][9];
        int n = FlatAnimFrames(s->name, frames);
        if (n > 0) {
            for (int i = 0; i < n; i++) {
                Image fi;
                if (!WadFlatImage(wt, frames[i], &fi)) { n = i; break; }
                s->tex[i] = UploadLevelTexture(fi);
                UnloadImage(fi);
            }
            s->frames = n;
            if (n > 0) return true;
        }
        ok = WadFlatImage(wt, s->name, &img);
    } else if (s->def >= 0) {
        ok = WadTexImage(wt, s->def, &img);
    }
    if (!ok) {
        // Missing art: a neutral stone tone keeps the geometry readable.
        img = GenImageColor(8, 8, s->flat ? (Color){ 120, 114, 104, 255 } : (Color){ 96, 100, 110, 255 });
    }
    s->tex[0] = UploadLevelTexture(img);
    s->frames = 1;
    UnloadImage(img);
    return s->tex[0].id != 0;
}

bool LevelMeshBuild(LevelMesh *lm, const DoomMap *map, const Wad *wad)
{
    WadTex wt;
    bool textured = wad && WadTexOpen(&wt, wad);
    TexBuild tb = { 0 };
    if (textured) { g_tb = &tb; g_wt = &wt; }
    bool ok = LevelMeshGenerate(lm, map);
    g_tb = NULL; g_wt = NULL;
    if (!ok) {
        for (int i = 0; i < tb.n; i++) { free(tb.geo[i].pos); free(tb.geo[i].uv); free(tb.geo[i].col); }
        free(tb.geo); free(tb.slots);
        if (textured) WadTexClose(&wt);
        return false;
    }
    if (lm->floors.vertexCount > 0) UploadMesh(&lm->floors, false);
    if (lm->walls.vertexCount > 0) UploadMesh(&lm->walls, false);
    lm->mat = LoadMaterialDefault();
    lm->valid = true;

    if (textured && tb.n > 0) {
        lm->slots = tb.slots;
        lm->slotCount = tb.n;
        lm->slotMesh = (Mesh *)calloc((size_t)tb.n, sizeof(Mesh));
        lm->slotMat = (Material *)calloc((size_t)tb.n, sizeof(Material));
        int tris = 0;
        for (int i = 0; i < tb.n && lm->slotMesh && lm->slotMat; i++) {
            TGeo *g = &tb.geo[i];
            LoadSlotTexture(&wt, &lm->slots[i]);
            lm->slotMat[i] = LoadMaterialDefault();
            lm->slotMat[i].maps[MATERIAL_MAP_DIFFUSE].texture = lm->slots[i].tex[0];
            Mesh *m = &lm->slotMesh[i];
            m->vertices = g->pos;
            m->texcoords = g->uv;
            m->colors = g->col;
            m->vertexCount = g->count;
            m->triangleCount = g->count/3;
            tris += m->triangleCount;
            if (g->count > 0) UploadMesh(m, false);
        }
        free(tb.geo);
        lm->textured = (lm->slotMesh && lm->slotMat);
        TraceLog(LOG_INFO, "LEVELMESH: %s: %d texture slots, %d textured tris",
                 map->name, tb.n, tris);
    }
    if (textured) WadTexClose(&wt);
    TraceLog(LOG_INFO, "LEVELMESH: %s: %d floor tris, %d wall tris",
             map->name, lm->floors.triangleCount, lm->walls.triangleCount);
    return true;
}

void LevelMeshFree(LevelMesh *lm)
{
    if (!lm) return;
    free(lm->floorTriSector);
    if (lm->valid) {
        UnloadMesh(lm->floors);
        UnloadMesh(lm->walls);
        UnloadMaterial(lm->mat);
    }
    for (int i = 0; i < lm->slotCount; i++) {
        if (lm->slotMesh) UnloadMesh(lm->slotMesh[i]);
        for (int f = 0; f < lm->slots[i].frames; f++) UnloadTexture(lm->slots[i].tex[f]);
        if (lm->slotMat) { lm->slotMat[i].maps[MATERIAL_MAP_DIFFUSE].texture = (Texture2D){ 0 }; UnloadMaterial(lm->slotMat[i]); }
    }
    free(lm->slotMesh);
    free(lm->slotMat);
    free(lm->slots);
    memset(lm, 0, sizeof(*lm));
}

void LevelMeshDraw(const LevelMesh *lm)
{
    if (!lm || !lm->valid) return;
    Matrix id = MatrixIdentity();
    if (lm->textured) {
        int animTic = (int)(GetTime()*35.0/8.0);
        for (int i = 0; i < lm->slotCount; i++) {
            if (lm->slotMesh[i].vertexCount <= 0) continue;
            const LevelTexSlot *s = &lm->slots[i];
            Material m = lm->slotMat[i];
            if (s->frames > 1) m.maps[MATERIAL_MAP_DIFFUSE].texture = s->tex[animTic % s->frames];
            DrawMesh(lm->slotMesh[i], m, id);
        }
        return;
    }
    if (lm->floors.vertexCount > 0) DrawMesh(lm->floors, lm->mat, id);
    if (lm->walls.vertexCount > 0) DrawMesh(lm->walls, lm->mat, id);
}
