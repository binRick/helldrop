#ifndef HELLDROP_WADTEX_H
#define HELLDROP_WADTEX_H

#include <stdbool.h>
#include "raylib.h"
#include "wad.h"

// Doom wall textures (PNAMES + TEXTURE1/TEXTURE2 composites) and flats
// (64x64 raw palette indices between F_START/F_END), decoded through
// PLAYPAL. Like every other visual, the level's look comes straight from
// the loaded WAD: point it at id's IWAD and the walls are id's walls.
typedef struct WadTexDef {
    char name[9];
    int width, height;
    int patchCount;
    const unsigned char *patches;   // raw mappatch_t records (10 bytes each)
} WadTexDef;

typedef struct WadTex {
    const Wad *wad;
    unsigned char pal[768];
    bool hasPal;
    int *pnames;              // PNAMES index -> lump index (-1 missing)
    int pnameCount;
    WadTexDef *defs;
    int defCount;
    int flatStart, flatEnd;   // lump index range of flats (exclusive markers)
} WadTex;

bool WadTexOpen(WadTex *wt, const Wad *wad);
void WadTexClose(WadTex *wt);
// -1 when the WAD has no such texture ("-" is always -1).
int  WadTexFind(const WadTex *wt, const char *name);
bool WadTexImage(const WadTex *wt, int def, Image *out);     // RGBA8, caller unloads
bool WadFlatImage(const WadTex *wt, const char *name, Image *out);

#endif
