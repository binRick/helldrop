#include "map.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

// Vanilla map lumps all live within this many directory entries after the
// map marker (THINGS..BLOCKMAP = 10, +1 for a Hexen BEHAVIOR lump).
#define MAP_LUMP_WINDOW 11

#define THING_REC 10
#define LINEDEF_REC 14
#define SIDEDEF_REC 30
#define VERTEX_REC 4
#define SECTOR_REC 26

static int16_t ReadI16(const unsigned char *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint16_t ReadU16(const unsigned char *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

// src is 8 bytes, space/NUL padded; dst becomes uppercase, NUL-terminated.
static void CopyName8(char dst[9], const unsigned char *src)
{
    int n = 0;
    while (n < 8 && src[n] != '\0') {
        dst[n] = (char)toupper(src[n]);
        n++;
    }
    dst[n] = '\0';
    while (n > 0 && dst[n - 1] == ' ') dst[--n] = '\0';
}

static int FindMapLump(const Wad *wad, int marker, const char *name)
{
    int last = marker + MAP_LUMP_WINDOW;
    if (last > wad->lumpCount - 1) last = wad->lumpCount - 1;
    for (int i = marker + 1; i <= last; i++) {
        if (strcmp(wad->lumps[i].name, name) == 0) return i;
    }
    return -1;
}

static bool GetRecords(const Wad *wad, int marker, const char *mapName,
                       const char *lumpName, int recSize,
                       const unsigned char **recs, int *count)
{
    *recs = NULL;
    *count = 0;
    int idx = FindMapLump(wad, marker, lumpName);
    if (idx < 0) {
        TraceLog(LOG_ERROR, "MAP: %s: missing %s lump", mapName, lumpName);
        return false;
    }
    int size = 0;
    const unsigned char *data = WadLumpData(wad, idx, &size);
    if (size % recSize != 0) {
        TraceLog(LOG_ERROR, "MAP: %s: %s size %d not a multiple of %d",
                 mapName, lumpName, size, recSize);
        return false;
    }
    *recs = data;
    *count = size / recSize;
    return true;
}

bool MapLoadFromWad(DoomMap *map, const Wad *wad, const char *mapName)
{
    memset(map, 0, sizeof(*map));

    int marker = WadFindLump(wad, mapName, 0);
    if (marker < 0) {
        TraceLog(LOG_ERROR, "MAP: %s not found in WAD", mapName);
        return false;
    }
    memcpy(map->name, wad->lumps[marker].name, sizeof(map->name));

    if (FindMapLump(wad, marker, "BEHAVIOR") >= 0) {
        TraceLog(LOG_ERROR, "MAP: %s is Hexen-format (BEHAVIOR lump), not vanilla", map->name);
        return false;
    }

    const unsigned char *rThings, *rLines, *rSides, *rVerts, *rSectors;
    int nThings, nLines, nSides, nVerts, nSectors;
    if (!GetRecords(wad, marker, map->name, "THINGS", THING_REC, &rThings, &nThings) ||
        !GetRecords(wad, marker, map->name, "LINEDEFS", LINEDEF_REC, &rLines, &nLines) ||
        !GetRecords(wad, marker, map->name, "SIDEDEFS", SIDEDEF_REC, &rSides, &nSides) ||
        !GetRecords(wad, marker, map->name, "VERTEXES", VERTEX_REC, &rVerts, &nVerts) ||
        !GetRecords(wad, marker, map->name, "SECTORS", SECTOR_REC, &rSectors, &nSectors)) {
        return false;
    }

    map->verts = (nVerts > 0) ? (Vector2 *)calloc((size_t)nVerts, sizeof(Vector2)) : NULL;
    map->lines = (nLines > 0) ? (Linedef *)calloc((size_t)nLines, sizeof(Linedef)) : NULL;
    map->sides = (nSides > 0) ? (Sidedef *)calloc((size_t)nSides, sizeof(Sidedef)) : NULL;
    map->sectors = (nSectors > 0) ? (Sector *)calloc((size_t)nSectors, sizeof(Sector)) : NULL;
    map->things = (nThings > 0) ? (Thing *)calloc((size_t)nThings, sizeof(Thing)) : NULL;
    if ((nVerts > 0 && map->verts == NULL) || (nLines > 0 && map->lines == NULL) ||
        (nSides > 0 && map->sides == NULL) || (nSectors > 0 && map->sectors == NULL) ||
        (nThings > 0 && map->things == NULL)) {
        TraceLog(LOG_ERROR, "MAP: %s: out of memory", map->name);
        MapFree(map);
        return false;
    }
    map->vertCount = nVerts;
    map->lineCount = nLines;
    map->sideCount = nSides;
    map->sectorCount = nSectors;
    map->thingCount = nThings;

    for (int i = 0; i < nVerts; i++) {
        const unsigned char *r = rVerts + (size_t)i * VERTEX_REC;
        map->verts[i].x = (float)ReadI16(r);
        map->verts[i].y = (float)ReadI16(r + 2);
        if (i == 0) {
            map->minX = map->maxX = map->verts[i].x;
            map->minY = map->maxY = map->verts[i].y;
        } else {
            if (map->verts[i].x < map->minX) map->minX = map->verts[i].x;
            if (map->verts[i].x > map->maxX) map->maxX = map->verts[i].x;
            if (map->verts[i].y < map->minY) map->minY = map->verts[i].y;
            if (map->verts[i].y > map->maxY) map->maxY = map->verts[i].y;
        }
    }

    for (int i = 0; i < nSectors; i++) {
        const unsigned char *r = rSectors + (size_t)i * SECTOR_REC;
        Sector *s = &map->sectors[i];
        s->floorH = (float)ReadI16(r);
        s->ceilH = (float)ReadI16(r + 2);
        CopyName8(s->floorFlat, r + 4);
        CopyName8(s->ceilFlat, r + 12);
        int light = ReadI16(r + 20);
        s->light = (light < 0) ? 0 : (light > 255) ? 255 : light;
        s->special = ReadI16(r + 22);
        s->tag = ReadI16(r + 24);
    }

    for (int i = 0; i < nSides; i++) {
        const unsigned char *r = rSides + (size_t)i * SIDEDEF_REC;
        Sidedef *sd = &map->sides[i];
        sd->xoff = (float)ReadI16(r);
        sd->yoff = (float)ReadI16(r + 2);
        CopyName8(sd->upper, r + 4);
        CopyName8(sd->lower, r + 12);
        CopyName8(sd->middle, r + 20);
        int sector = ReadU16(r + 28);
        if (sector >= nSectors) {
            TraceLog(LOG_ERROR, "MAP: %s: sidedef %d references sector %d (have %d)",
                     map->name, i, sector, nSectors);
            MapFree(map);
            return false;
        }
        sd->sector = sector;
    }

    for (int i = 0; i < nLines; i++) {
        const unsigned char *r = rLines + (size_t)i * LINEDEF_REC;
        Linedef *ld = &map->lines[i];
        int v1 = ReadU16(r);
        int v2 = ReadU16(r + 2);
        if (v1 >= nVerts || v2 >= nVerts) {
            TraceLog(LOG_ERROR, "MAP: %s: linedef %d references vertex %d/%d (have %d)",
                     map->name, i, v1, v2, nVerts);
            MapFree(map);
            return false;
        }
        ld->v1 = v1;
        ld->v2 = v2;
        ld->flags = ReadU16(r + 4);
        ld->special = ReadI16(r + 6);
        ld->tag = ReadI16(r + 8);
        uint16_t front = ReadU16(r + 10);
        uint16_t back = ReadU16(r + 12);
        ld->sideFront = (front == 0xFFFF) ? -1 : (int)front;
        ld->sideBack = (back == 0xFFFF) ? -1 : (int)back;
        if (ld->sideFront >= nSides || ld->sideBack >= nSides) {
            TraceLog(LOG_ERROR, "MAP: %s: linedef %d references sidedef %d/%d (have %d)",
                     map->name, i, ld->sideFront, ld->sideBack, nSides);
            MapFree(map);
            return false;
        }
    }

    for (int i = 0; i < nThings; i++) {
        const unsigned char *r = rThings + (size_t)i * THING_REC;
        Thing *t = &map->things[i];
        t->x = (float)ReadI16(r);
        t->y = (float)ReadI16(r + 2);
        t->angleDeg = (float)ReadI16(r + 4);
        t->type = ReadI16(r + 6);
        t->flags = ReadU16(r + 8);
    }

    return true;
}

void MapFree(DoomMap *map)
{
    if (map == NULL) return;
    free(map->verts);
    free(map->lines);
    free(map->sides);
    free(map->sectors);
    free(map->things);
    memset(map, 0, sizeof(*map));
}
