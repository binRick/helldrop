#ifndef HELLDROP_DOOMGFX_H
#define HELLDROP_DOOMGFX_H

#include <stdbool.h>
#include "raylib.h"
#include "wad.h"

// Classic Doom status-bar graphics decoded from the WAD (patch format
// through PLAYPAL). Every piece is optional: a missing lump just clears its
// has* flag and the HUD falls back to plain rectangles and text, so any
// Doom-compatible IWAD (Freedoom included) works.

// A patch plus its header draw offsets. V_DrawPatch blits at (x - ox, y - oy);
// the mugshot faces rely on this (they carry -5,-2) to sit centered in the
// bar's face slot.
typedef struct HudPatch {
    Texture2D tex;
    int ox, oy;
} HudPatch;

typedef struct DoomHud {
    Texture2D stbar;        bool hasStbar;     // the 320x32 bar itself
    Texture2D arms;         bool hasArms;      // STARMS: covers the DM frag box
    Texture2D gnum[6];      bool hasGnum;      // STGNUM2..7: unowned arms slots
    Texture2D ynum[6];      bool hasYnum;      // STYSNUM2..7: owned arms slots
    HudPatch face[5][3];    bool hasFaces;     // mugshot: pain tier x look
    HudPatch faceDead;      bool hasDead;      // STFDEAD0
    HudPatch faceGrin[5];   bool hasGrin;      // STFEVL*: new-toy sneer
    HudPatch faceOuch[5];   bool hasOuch;      // STFOUCH*: 20+ damage at once
    Texture2D digit[10];    bool hasDigits;    // STTNUM*: big red numbers
    Texture2D percent;      bool hasPercent;   // STTPRCNT
} DoomHud;

bool DoomHudLoad(DoomHud *hud, const Wad *wad);   // false = nothing usable
void DoomHudUnload(DoomHud *hud);

// Shared decoder pieces (sprites use the same picture format).
bool DoomLoadPalette(const Wad *wad, unsigned char pal[768]);
bool DoomPatchToTexture(const Wad *wad, int lumpIndex,
                        const unsigned char *pal, Texture2D *out);
// CPU-side decode (RGBA8 Image); caller owns/unloads the Image.
bool DoomPatchToImage(const Wad *wad, int lumpIndex,
                      const unsigned char *pal, Image *out);
// Header only: dimensions and the sprite's drawing offsets.
bool DoomPatchInfo(const Wad *wad, int lumpIndex,
                   int *w, int *h, int *leftOfs, int *topOfs);

#endif
