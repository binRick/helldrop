#include "wadtex.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "doomgfx.h"

static int RdI32(const unsigned char *p) { return (int)((unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24)); }
static int RdI16(const unsigned char *p) { return (int)(short)((unsigned)p[0] | ((unsigned)p[1] << 8)); }

static void Name8(char out[9], const unsigned char *src)
{
    for (int i = 0; i < 8; i++) {
        char c = (char)src[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        out[i] = c;
        if (!c) break;
    }
    out[8] = '\0';
}

static bool AddTextureLump(WadTex *wt, const char *lumpName)
{
    int idx = WadFindLump(wt->wad, lumpName, 0);
    if (idx < 0) return false;
    int size = 0;
    const unsigned char *d = WadLumpData(wt->wad, idx, &size);
    if (!d || size < 4) return false;
    int n = RdI32(d);
    if (n <= 0 || 4 + n*4 > size) return false;
    WadTexDef *grown = (WadTexDef *)realloc(wt->defs, (size_t)(wt->defCount + n) * sizeof(WadTexDef));
    if (!grown) return false;
    wt->defs = grown;
    for (int i = 0; i < n; i++) {
        int ofs = RdI32(d + 4 + i*4);
        if (ofs < 0 || ofs + 22 > size) continue;
        const unsigned char *t = d + ofs;
        WadTexDef *def = &wt->defs[wt->defCount];
        Name8(def->name, t);
        def->width = RdI16(t + 12);
        def->height = RdI16(t + 14);
        def->patchCount = RdI16(t + 20);
        if (def->width <= 0 || def->height <= 0 || def->width > 2048 || def->height > 2048) continue;
        if (def->patchCount < 0 || ofs + 22 + def->patchCount*10 > size) continue;
        def->patches = t + 22;
        wt->defCount++;
    }
    return true;
}

bool WadTexOpen(WadTex *wt, const Wad *wad)
{
    memset(wt, 0, sizeof(*wt));
    wt->wad = wad;
    wt->hasPal = DoomLoadPalette(wad, wt->pal);
    if (!wt->hasPal) return false;
    int pn = WadFindLump(wad, "PNAMES", 0);
    if (pn >= 0) {
        int size = 0;
        const unsigned char *d = WadLumpData(wad, pn, &size);
        int n = (d && size >= 4) ? RdI32(d) : 0;
        if (n > 0 && 4 + n*8 <= size) {
            wt->pnames = (int *)malloc((size_t)n * sizeof(int));
            if (wt->pnames) {
                wt->pnameCount = n;
                for (int i = 0; i < n; i++) {
                    char nm[9];
                    Name8(nm, d + 4 + i*8);
                    wt->pnames[i] = WadFindLump(wad, nm, 0);
                }
            }
        }
    }
    AddTextureLump(wt, "TEXTURE1");
    AddTextureLump(wt, "TEXTURE2");
    int fs = WadFindLump(wad, "F_START", 0);
    if (fs < 0) fs = WadFindLump(wad, "FF_START", 0);
    int fe = WadFindLump(wad, "F_END", 0);
    if (fe < 0) fe = WadFindLump(wad, "FF_END", 0);
    wt->flatStart = fs;
    wt->flatEnd = (fe > fs) ? fe : -1;
    TraceLog(LOG_INFO, "WADTEX: %d wall textures, %d patch names, flats %s",
             wt->defCount, wt->pnameCount, (wt->flatEnd > 0) ? "ok" : "missing");
    return true;
}

void WadTexClose(WadTex *wt)
{
    if (!wt) return;
    free(wt->pnames);
    free(wt->defs);
    memset(wt, 0, sizeof(*wt));
}

int WadTexFind(const WadTex *wt, const char *name)
{
    if (!name || !name[0] || name[0] == '-') return -1;
    for (int i = 0; i < wt->defCount; i++) {
        if (strncasecmp(wt->defs[i].name, name, 8) == 0) return i;
    }
    return -1;
}

bool WadTexImage(const WadTex *wt, int defIdx, Image *out)
{
    if (defIdx < 0 || defIdx >= wt->defCount) return false;
    const WadTexDef *def = &wt->defs[defIdx];
    Image img = GenImageColor(def->width, def->height, (Color){ 0, 0, 0, 255 });
    if (!img.data) return false;
    unsigned char *dst = (unsigned char *)img.data;
    for (int p = 0; p < def->patchCount; p++) {
        const unsigned char *mp = def->patches + p*10;
        int ox = RdI16(mp), oy = RdI16(mp + 2), pi = RdI16(mp + 4);
        if (pi < 0 || pi >= wt->pnameCount || wt->pnames[pi] < 0) continue;
        Image patch;
        if (!DoomPatchToImage(wt->wad, wt->pnames[pi], wt->pal, &patch)) continue;
        const unsigned char *src = (const unsigned char *)patch.data;
        for (int y = 0; y < patch.height; y++) {
            int ty = oy + y;
            if (ty < 0 || ty >= def->height) continue;
            for (int x = 0; x < patch.width; x++) {
                int tx = ox + x;
                if (tx < 0 || tx >= def->width) continue;
                const unsigned char *s = src + ((size_t)y*patch.width + x)*4;
                if (s[3] == 0) continue;   // transparent post gap
                unsigned char *o = dst + ((size_t)ty*def->width + tx)*4;
                o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = 255;
            }
        }
        UnloadImage(patch);
    }
    *out = img;
    return true;
}

bool WadFlatImage(const WadTex *wt, const char *name, Image *out)
{
    if (wt->flatStart < 0 || wt->flatEnd < 0 || !name || !name[0]) return false;
    int idx = WadFindLump(wt->wad, name, wt->flatStart);
    if (idx < 0 || idx > wt->flatEnd) return false;
    int size = 0;
    const unsigned char *d = WadLumpData(wt->wad, idx, &size);
    if (!d || size < 4096) return false;
    Image img = GenImageColor(64, 64, BLACK);
    if (!img.data) return false;
    unsigned char *o = (unsigned char *)img.data;
    for (int i = 0; i < 4096; i++) {
        unsigned ci = d[i];
        o[i*4 + 0] = wt->pal[ci*3 + 0];
        o[i*4 + 1] = wt->pal[ci*3 + 1];
        o[i*4 + 2] = wt->pal[ci*3 + 2];
        o[i*4 + 3] = 255;
    }
    *out = img;
    return true;
}
