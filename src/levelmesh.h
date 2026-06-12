#ifndef HELLDROP_LEVELMESH_H
#define HELLDROP_LEVELMESH_H

#include <stdbool.h>
#include "raylib.h"
#include "map.h"

// Flat-shaded extruded level geometry for the isometric view ("2.5D cheat"):
// floors are sector polygons at floor height, walls are quads spanning the
// height differences along linedefs. No ceilings — the iso camera looks in
// from above. All shading is baked into vertex colors (sector light level,
// Doom-style fake contrast on walls, color-keyed special flats like nukage),
// so the default raylib material renders everything.
//
// Meshes are non-indexed triangle soup: raylib Mesh indices are 16-bit, and
// large maps blow past 65k vertices; unindexed DrawMesh has no such limit.
typedef struct LevelMesh {
    Mesh floors;        // all sector floor polygons
    Mesh walls;         // all wall quads (two-sided: both windings emitted)
    Material mat;       // default material; vertex colors do the work
    int *floorTriSector;   // sector index per floors triangle (CPU-side);
                           // the sim's point->sector lookup depends on it
    bool valid;
} LevelMesh;

// Tessellates sector boundary loops with libtess2 and uploads the meshes.
// Sectors whose boundary cannot be assembled into closed loops are skipped
// with a warning log (malformed map data), never a hard failure.
bool LevelMeshBuild(LevelMesh *lm, const DoomMap *map);
void LevelMeshFree(LevelMesh *lm);
void LevelMeshDraw(const LevelMesh *lm);

#endif
