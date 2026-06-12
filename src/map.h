#ifndef HELLDROP_MAP_H
#define HELLDROP_MAP_H

#include <stdbool.h>
#include "raylib.h"
#include "wad.h"

// Runtime Doom map, decoded from the vanilla 16-bit binary lump formats
// (THINGS/LINEDEFS/SIDEDEFS/VERTEXES/SECTORS). We deliberately do not read
// NODES/SEGS/SSECTORS: floor geometry is tessellated from sector boundary
// loops instead, so extended node formats (ZDBSP etc.) never break us.
//
// Coordinates stay in Doom map units. Doom is x-east / y-north; the
// renderer maps (x, y, height) -> raylib (x, height, -y).

// Linedef flags (vanilla)
#define ML_BLOCKING  0x0001
#define ML_TWOSIDED  0x0004
#define ML_SECRET    0x0020
#define ML_DONTDRAW  0x0080

typedef struct Thing {
    float x, y;
    float angleDeg;
    int type;          // doomednum (1 = player 1 start, 3001 = imp, ...)
    unsigned flags;
} Thing;

typedef struct Sector {
    float floorH, ceilH;
    int light;          // 0..255
    int special, tag;
    char floorFlat[9], ceilFlat[9];
} Sector;

typedef struct Sidedef {
    float xoff, yoff;
    int sector;         // index into DoomMap.sectors
    char upper[9], lower[9], middle[9];
} Sidedef;

typedef struct Linedef {
    int v1, v2;         // indices into DoomMap.verts
    unsigned flags;
    int special, tag;
    int sideFront, sideBack;  // indices into DoomMap.sides, -1 = none.
                              // Front sidedef lies on the RIGHT of v1->v2.
} Linedef;

typedef struct DoomMap {
    char name[9];
    Vector2 *verts;   int vertCount;    // Doom x,y as floats
    Linedef *lines;   int lineCount;
    Sidedef *sides;   int sideCount;
    Sector  *sectors; int sectorCount;
    Thing   *things;  int thingCount;
    float minX, minY, maxX, maxY;       // bounding box over verts
} DoomMap;

// Locates the `mapName` marker lump (e.g. "E1M1") in `wad` and decodes the
// map lumps that follow it. Logs and returns false on any malformed lump.
bool MapLoadFromWad(DoomMap *map, const Wad *wad, const char *mapName);
void MapFree(DoomMap *map);

#endif
