#include "levelmesh.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "raymath.h"
#include "tesselator.h"

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
    }
    return true;
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
            if (tessTesselate(tess, TESS_WINDING_POSITIVE, TESS_POLYGONS, 3, 2, NULL)) {
                ok = EmitSectorFloor(g, tri2sec, s, tess, sec->floorH, col);
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

bool LevelMeshBuild(LevelMesh *lm, const DoomMap *map)
{
    if (!LevelMeshGenerate(lm, map)) return false;
    if (lm->floors.vertexCount > 0) UploadMesh(&lm->floors, false);
    if (lm->walls.vertexCount > 0) UploadMesh(&lm->walls, false);
    lm->mat = LoadMaterialDefault();
    lm->valid = true;
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
    memset(lm, 0, sizeof(*lm));
}

void LevelMeshDraw(const LevelMesh *lm)
{
    if (!lm || !lm->valid) return;
    Matrix id = MatrixIdentity();
    if (lm->floors.vertexCount > 0) DrawMesh(lm->floors, lm->mat, id);
    if (lm->walls.vertexCount > 0) DrawMesh(lm->walls, lm->mat, id);
}
