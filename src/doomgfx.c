#include "doomgfx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t ReadLE32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t ReadLE16(const unsigned char *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

bool DoomLoadPalette(const Wad *wad, unsigned char pal[768])
{
    int idx = WadFindLump(wad, "PLAYPAL", 0);
    if (idx < 0) return false;
    int size = 0;
    const unsigned char *d = WadLumpData(wad, idx, &size);
    if (!d || size < 768) return false;
    memcpy(pal, d, 768);
    return true;
}

// Decode one Doom picture-format ("patch") lump into RGBA. Hostile data is
// only ever skipped, never trusted: every offset is bounds-checked.
bool DoomPatchToImage(const Wad *wad, int lumpIndex,
                      const unsigned char *pal, Image *out)
{
    int size = 0;
    const unsigned char *d = WadLumpData(wad, lumpIndex, &size);
    if (!d || size < 8) return false;

    int w = ReadLE16(d);
    int h = ReadLE16(d + 2);
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return false;
    if (size < 8 + w*4) return false;

    unsigned char *rgba = (unsigned char *)calloc((size_t)w*h, 4);
    if (!rgba) return false;

    for (int x = 0; x < w; x++) {
        uint32_t ofs = ReadLE32(d + 8 + x*4);
        while (ofs + 2 <= (uint32_t)size) {
            unsigned char topdelta = d[ofs];
            if (topdelta == 0xFF) break;
            unsigned char len = d[ofs + 1];
            if (ofs + 3 + (uint32_t)len + 1 > (uint32_t)size) break;
            const unsigned char *src = d + ofs + 3;   // skip pad byte
            for (int i = 0; i < len; i++) {
                int y = topdelta + i;
                if (y >= h) break;
                unsigned char ci = src[i];
                unsigned char *px = rgba + ((size_t)y*w + x)*4;
                px[0] = pal[ci*3 + 0];
                px[1] = pal[ci*3 + 1];
                px[2] = pal[ci*3 + 2];
                px[3] = 255;
            }
            ofs += 4u + len;
        }
    }

    out->data = rgba;
    out->width = w;
    out->height = h;
    out->mipmaps = 1;
    out->format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    return true;
}

bool DoomPatchToTexture(const Wad *wad, int lumpIndex,
                        const unsigned char *pal, Texture2D *out)
{
    Image img;
    if (!DoomPatchToImage(wad, lumpIndex, pal, &img)) return false;
    *out = LoadTextureFromImage(img);
    SetTextureFilter(*out, TEXTURE_FILTER_POINT);
    UnloadImage(img);
    return out->id != 0;
}

bool DoomPatchInfo(const Wad *wad, int lumpIndex,
                   int *w, int *h, int *leftOfs, int *topOfs)
{
    int size = 0;
    const unsigned char *d = WadLumpData(wad, lumpIndex, &size);
    if (!d || size < 8) return false;
    if (w) *w = ReadLE16(d);
    if (h) *h = ReadLE16(d + 2);
    if (leftOfs) *leftOfs = (int16_t)ReadLE16(d + 4);
    if (topOfs) *topOfs = (int16_t)ReadLE16(d + 6);
    return true;
}

static bool PatchToTexture(const Wad *wad, const char *name,
                           const unsigned char *pal, Texture2D *out)
{
    int idx = WadFindLump(wad, name, 0);
    if (idx < 0) return false;
    return DoomPatchToTexture(wad, idx, pal, out);
}

static bool PatchToHud(const Wad *wad, const char *name,
                       const unsigned char *pal, HudPatch *out)
{
    int idx = WadFindLump(wad, name, 0);
    if (idx < 0) return false;
    out->ox = 0;
    out->oy = 0;
    DoomPatchInfo(wad, idx, NULL, NULL, &out->ox, &out->oy);
    return DoomPatchToTexture(wad, idx, pal, &out->tex);
}

bool DoomHudLoad(DoomHud *hud, const Wad *wad)
{
    memset(hud, 0, sizeof(*hud));
    unsigned char pal[768];
    if (!DoomLoadPalette(wad, pal)) return false;

    hud->hasStbar = PatchToTexture(wad, "STBAR", pal, &hud->stbar);
    hud->hasArms = PatchToTexture(wad, "STARMS", pal, &hud->arms);

    hud->hasGnum = true;
    hud->hasYnum = true;
    for (int i = 0; i < 6; i++) {
        char name[9];
        snprintf(name, sizeof(name), "STGNUM%d", i + 2);
        if (!PatchToTexture(wad, name, pal, &hud->gnum[i])) hud->hasGnum = false;
        snprintf(name, sizeof(name), "STYSNUM%d", i + 2);
        if (!PatchToTexture(wad, name, pal, &hud->ynum[i])) hud->hasYnum = false;
    }

    hud->hasFaces = true;
    for (int t = 0; t < 5 && hud->hasFaces; t++) {
        for (int l = 0; l < 3 && hud->hasFaces; l++) {
            char name[9];
            snprintf(name, sizeof(name), "STFST%d%d", t, l);
            hud->hasFaces = PatchToHud(wad, name, pal, &hud->face[t][l]);
        }
    }
    hud->hasDead = PatchToHud(wad, "STFDEAD0", pal, &hud->faceDead);

    hud->hasGrin = true;
    hud->hasOuch = true;
    for (int t = 0; t < 5; t++) {
        char name[9];
        snprintf(name, sizeof(name), "STFEVL%d", t);
        if (!PatchToHud(wad, name, pal, &hud->faceGrin[t])) hud->hasGrin = false;
        snprintf(name, sizeof(name), "STFOUCH%d", t);
        if (!PatchToHud(wad, name, pal, &hud->faceOuch[t])) hud->hasOuch = false;
    }

    hud->hasDigits = true;
    for (int i = 0; i < 10; i++) {
        char name[9];
        snprintf(name, sizeof(name), "STTNUM%d", i);
        if (!PatchToTexture(wad, name, pal, &hud->digit[i])) hud->hasDigits = false;
    }
    hud->hasPercent = PatchToTexture(wad, "STTPRCNT", pal, &hud->percent);

    return hud->hasStbar || hud->hasFaces || hud->hasDigits;
}

void DoomHudUnload(DoomHud *hud)
{
    if (!hud) return;
    if (hud->hasStbar) UnloadTexture(hud->stbar);
    if (hud->hasArms) UnloadTexture(hud->arms);
    if (hud->hasGnum) {
        for (int i = 0; i < 6; i++) UnloadTexture(hud->gnum[i]);
    }
    if (hud->hasYnum) {
        for (int i = 0; i < 6; i++) UnloadTexture(hud->ynum[i]);
    }
    if (hud->hasFaces) {
        for (int t = 0; t < 5; t++) {
            for (int l = 0; l < 3; l++) UnloadTexture(hud->face[t][l].tex);
        }
    }
    if (hud->hasDead) UnloadTexture(hud->faceDead.tex);
    if (hud->hasGrin) {
        for (int t = 0; t < 5; t++) UnloadTexture(hud->faceGrin[t].tex);
    }
    if (hud->hasOuch) {
        for (int t = 0; t < 5; t++) UnloadTexture(hud->faceOuch[t].tex);
    }
    if (hud->hasDigits) {
        for (int i = 0; i < 10; i++) UnloadTexture(hud->digit[i]);
    }
    if (hud->hasPercent) UnloadTexture(hud->percent);
    memset(hud, 0, sizeof(*hud));
}
