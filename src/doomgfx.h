#ifndef HELLDROP_DOOMGFX_H
#define HELLDROP_DOOMGFX_H

#include <stdbool.h>
#include "raylib.h"
#include "wad.h"

// Classic Doom status-bar graphics decoded from the WAD (patch format
// through PLAYPAL). Every piece is optional: a missing lump just clears its
// has* flag and the HUD falls back to plain rectangles and text, so any
// Doom-compatible IWAD (Freedoom included) works.
typedef struct DoomHud {
    Texture2D stbar;        bool hasStbar;     // the 320x32 bar itself
    Texture2D face[5][3];   bool hasFaces;     // mugshot: pain tier x look
    Texture2D faceDead;     bool hasDead;      // STFDEAD0
    Texture2D faceGrin[5];  bool hasGrin;      // STFEVL*: new-toy sneer
    Texture2D faceOuch[5];  bool hasOuch;      // STFOUCH*: 20+ damage at once
    Texture2D digit[10];    bool hasDigits;    // STTNUM*: big red numbers
    Texture2D percent;      bool hasPercent;   // STTPRCNT
} DoomHud;

bool DoomHudLoad(DoomHud *hud, const Wad *wad);   // false = nothing usable
void DoomHudUnload(DoomHud *hud);

#endif
