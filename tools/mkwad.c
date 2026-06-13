// Helldrop WAD builder. Assembles assets/helldrop.wad from a base IWAD
// (Freedoom — BSD-licensed, redistributable) plus our own override lumps,
// so Helldrop ships with art we own and may legally distribute. id Software
// assets are never read or written here.
//
//   mkwad build <base.wad> <overrides-dir> <out.wad>
//       Copies every base lump through, then applies overrides:
//         <name>.txt / <name>.lmp   raw lump (replaces same-named base lump,
//                                    else appended)
//         graphics/<name>.png       encoded to Doom patch format via the base
//                                    PLAYPAL (nearest-match; alpha<128 => a
//                                    transparent gap). Optional sidecar
//                                    graphics/<name>.ofs holds "left top".
//       Lump name = uppercased filename stem, truncated to 8 chars.
//
//   mkwad bake <base.wad> <out-graphics-dir>
//       One-shot: derives Helldrop-tinted STBAR/STARMS PNGs from the base so
//       the HUD reskin lives as editable, committed source art.
//
// Built headless: only CPU-side raylib image calls are used (no GL context).

#include "raylib.h"
#include "wad.h"
#include "doomgfx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_OUT_LUMPS 8192

typedef struct OutLump {
    char name[9];
    const unsigned char *data;   // base lumps point into the Wad buffer
    int size;
    unsigned char *owned;        // non-NULL if we must free it
} OutLump;

// ---- little-endian writers ------------------------------------------------

static void WriteU32(FILE *f, unsigned v)
{
    fputc((int)(v & 0xff), f);
    fputc((int)((v >> 8) & 0xff), f);
    fputc((int)((v >> 16) & 0xff), f);
    fputc((int)((v >> 24) & 0xff), f);
}

// Uppercase filename stem -> 8-char lump name.
static void LumpNameFromPath(const char *path, char out[9])
{
    const char *stem = GetFileNameWithoutExt(path);
    int n = 0;
    for (; stem[n] != '\0' && n < 8; n++) out[n] = (char)toupper((unsigned char)stem[n]);
    out[n] = '\0';
}

// ---- patch encoding -------------------------------------------------------

static int NearestPaletteIndex(const unsigned char pal[768], int r, int g, int b)
{
    int best = 0;
    long bestDist = 1L << 60;
    for (int i = 0; i < 256; i++) {
        int dr = r - pal[i*3 + 0];
        int dg = g - pal[i*3 + 1];
        int db = b - pal[i*3 + 2];
        long d = (long)dr*dr + (long)dg*dg + (long)db*db;
        if (d < bestDist) { bestDist = d; best = i; }
    }
    return best;
}

// Encode RGBA pixels (w*h, row-major) as a Doom picture-format patch.
// alpha < 128 reads as a transparent gap. Returns malloc'd bytes; NULL on
// error (e.g. height needing the tall-patch trick, which the HUD never does).
static unsigned char *EncodePatch(const Color *px, int w, int h,
                                  int leftOfs, int topOfs,
                                  const unsigned char pal[768], int *outSize)
{
    if (w <= 0 || h <= 0 || w > 4096 || h > 254) return NULL;

    // Worst case (alternating posts): each column <= h*(4+1) bytes + 1.
    size_t headerLen = 8 + (size_t)w * 4;
    size_t colsCap = (size_t)w * ((size_t)h * 5 + 2) + 16;
    unsigned char *cols = (unsigned char *)malloc(colsCap);
    uint32_t *colOfs = (uint32_t *)malloc((size_t)w * sizeof(uint32_t));
    if (!cols || !colOfs) { free(cols); free(colOfs); return NULL; }

    size_t cw = 0;   // write cursor into cols
    for (int x = 0; x < w; x++) {
        colOfs[x] = (uint32_t)cw;
        int y = 0;
        while (y < h) {
            const Color *c = &px[y*w + x];
            if (c->a < 128) { y++; continue; }      // transparent: skip
            int top = y;
            int runStart = y;
            while (y < h && px[y*w + x].a >= 128 && (y - runStart) < 254) y++;
            int len = y - runStart;
            cols[cw++] = (unsigned char)top;        // topdelta
            cols[cw++] = (unsigned char)len;        // length
            cols[cw++] = 0;                         // unused pad
            for (int i = 0; i < len; i++) {
                const Color *p = &px[(runStart + i)*w + x];
                cols[cw++] = (unsigned char)NearestPaletteIndex(pal, p->r, p->g, p->b);
            }
            cols[cw++] = 0;                         // unused pad
        }
        cols[cw++] = 0xFF;                          // column terminator
    }

    int total = (int)(headerLen + cw);
    unsigned char *out = (unsigned char *)malloc((size_t)total);
    if (!out) { free(cols); free(colOfs); return NULL; }

    out[0] = (unsigned char)(w & 0xff);   out[1] = (unsigned char)((w >> 8) & 0xff);
    out[2] = (unsigned char)(h & 0xff);   out[3] = (unsigned char)((h >> 8) & 0xff);
    out[4] = (unsigned char)(leftOfs & 0xff); out[5] = (unsigned char)((leftOfs >> 8) & 0xff);
    out[6] = (unsigned char)(topOfs & 0xff);  out[7] = (unsigned char)((topOfs >> 8) & 0xff);
    for (int x = 0; x < w; x++) {
        uint32_t abs = (uint32_t)headerLen + colOfs[x];
        unsigned char *e = out + 8 + x*4;
        e[0] = abs & 0xff; e[1] = (abs >> 8) & 0xff;
        e[2] = (abs >> 16) & 0xff; e[3] = (abs >> 24) & 0xff;
    }
    memcpy(out + headerLen, cols, cw);

    free(cols);
    free(colOfs);
    if (outSize) *outSize = total;
    return out;
}

// ---- WAD writing ----------------------------------------------------------

static bool WriteWad(const char *path, OutLump *lumps, int count)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "mkwad: cannot write %s\n", path); return false; }

    // Header placeholder; back-patched once offsets are known.
    fwrite("IWAD", 1, 4, f);
    WriteU32(f, 0);
    WriteU32(f, 0);

    uint32_t *filepos = (uint32_t *)malloc((size_t)count * sizeof(uint32_t));
    for (int i = 0; i < count; i++) {
        filepos[i] = (uint32_t)ftell(f);
        if (lumps[i].size > 0 && lumps[i].data)
            fwrite(lumps[i].data, 1, (size_t)lumps[i].size, f);
    }

    uint32_t dirOfs = (uint32_t)ftell(f);
    for (int i = 0; i < count; i++) {
        WriteU32(f, filepos[i]);
        WriteU32(f, (uint32_t)lumps[i].size);
        char name8[8] = { 0 };
        memcpy(name8, lumps[i].name, strnlen(lumps[i].name, 8));
        fwrite(name8, 1, 8, f);
    }

    fseek(f, 4, SEEK_SET);
    WriteU32(f, (uint32_t)count);
    WriteU32(f, dirOfs);

    free(filepos);
    fclose(f);
    return true;
}

// ---- override application -------------------------------------------------

static void AddOrReplace(OutLump *out, int *count, const char *name,
                         unsigned char *data, int size)
{
    for (int i = 0; i < *count; i++) {
        if (strcasecmp(out[i].name, name) == 0) {
            free(out[i].owned);
            out[i].owned = data;
            out[i].data = data;
            out[i].size = size;
            printf("  replace %-8s (%d bytes)\n", name, size);
            return;
        }
    }
    if (*count >= MAX_OUT_LUMPS) { fprintf(stderr, "mkwad: too many lumps\n"); return; }
    OutLump *l = &out[(*count)++];
    snprintf(l->name, sizeof(l->name), "%s", name);
    l->owned = data;
    l->data = data;
    l->size = size;
    printf("  append  %-8s (%d bytes)\n", name, size);
}

static void ApplyRawFile(OutLump *out, int *count, const char *path)
{
    int size = 0;
    unsigned char *bytes = LoadFileData(path, &size);
    if (!bytes) { fprintf(stderr, "mkwad: cannot read %s\n", path); return; }
    char name[9];
    LumpNameFromPath(path, name);
    AddOrReplace(out, count, name, bytes, size);   // bytes owned by LoadFileData; freed via free()
}

static void ApplyGraphic(OutLump *out, int *count, const char *path,
                         const unsigned char pal[768])
{
    Image img = LoadImage(path);
    if (img.data == NULL) { fprintf(stderr, "mkwad: cannot load %s\n", path); return; }
    Color *px = LoadImageColors(img);

    int leftOfs = 0, topOfs = 0;
    char ofsPath[1024];
    snprintf(ofsPath, sizeof(ofsPath), "%s.ofs", path);
    if (FileExists(ofsPath)) {
        char *t = LoadFileText(ofsPath);
        if (t) { sscanf(t, "%d %d", &leftOfs, &topOfs); UnloadFileText(t); }
    }

    int size = 0;
    unsigned char *patch = EncodePatch(px, img.width, img.height,
                                       leftOfs, topOfs, pal, &size);
    UnloadImageColors(px);
    UnloadImage(img);
    if (!patch) { fprintf(stderr, "mkwad: failed to encode %s\n", path); return; }

    char name[9];
    LumpNameFromPath(path, name);
    AddOrReplace(out, count, name, patch, size);
}

// ---- modes ----------------------------------------------------------------

static int BuildMode(const char *basePath, const char *overridesDir, const char *outPath)
{
    Wad base = { 0 };
    if (!WadLoad(&base, basePath)) { fprintf(stderr, "mkwad: cannot load base %s\n", basePath); return 1; }
    unsigned char pal[768];
    bool havePal = DoomLoadPalette(&base, pal);

    OutLump *out = (OutLump *)calloc(MAX_OUT_LUMPS, sizeof(OutLump));
    int count = 0;
    for (int i = 0; i < base.lumpCount && count < MAX_OUT_LUMPS; i++) {
        int sz = 0;
        const unsigned char *d = WadLumpData(&base, i, &sz);
        OutLump *l = &out[count++];
        snprintf(l->name, sizeof(l->name), "%s", base.lumps[i].name);
        l->data = d;
        l->size = sz;
        l->owned = NULL;
    }
    printf("mkwad: %d base lumps from %s\n", count, basePath);

    // Raw lumps at the overrides root (*.txt, *.lmp).
    FilePathList root = LoadDirectoryFiles(overridesDir);
    for (unsigned i = 0; i < root.count; i++) {
        if (IsFileExtension(root.paths[i], ".txt") || IsFileExtension(root.paths[i], ".lmp"))
            ApplyRawFile(out, &count, root.paths[i]);
    }
    UnloadDirectoryFiles(root);

    // Graphics (*.png) under <overrides>/graphics.
    char gfxDir[1024];
    snprintf(gfxDir, sizeof(gfxDir), "%s/graphics", overridesDir);
    if (havePal && DirectoryExists(gfxDir)) {
        FilePathList gfx = LoadDirectoryFiles(gfxDir);
        for (unsigned i = 0; i < gfx.count; i++) {
            if (IsFileExtension(gfx.paths[i], ".png"))
                ApplyGraphic(out, &count, gfx.paths[i], pal);
        }
        UnloadDirectoryFiles(gfx);
    }

    bool ok = WriteWad(outPath, out, count);
    if (ok) printf("mkwad: wrote %s (%d lumps)\n", outPath, count);

    for (int i = 0; i < count; i++) free(out[i].owned);
    free(out);
    WadFree(&base);
    return ok ? 0 : 1;
}

// Multiply tint that reskins Freedoom's grey bar toward hell-forged metal:
// warm the reds, pull down green/blue. Stays paletted once re-encoded.
static void TintHud(unsigned char *rgba, int n)
{
    for (int i = 0; i < n; i++) {
        unsigned char *p = rgba + i*4;
        if (p[3] == 0) continue;
        int r = (int)(p[0] * 1.14f) + 12;
        int g = (int)(p[1] * 0.66f);
        int b = (int)(p[2] * 0.52f);
        p[0] = (unsigned char)(r > 255 ? 255 : r);
        p[1] = (unsigned char)(g > 255 ? 255 : g);
        p[2] = (unsigned char)(b > 255 ? 255 : b);
    }
}

static bool BakeOne(Wad *base, const unsigned char pal[768],
                    const char *lump, const char *outDir)
{
    int idx = WadFindLump(base, lump, 0);
    if (idx < 0) { fprintf(stderr, "mkwad: base has no %s\n", lump); return false; }
    Image img;
    if (!DoomPatchToImage(base, idx, pal, &img)) { fprintf(stderr, "mkwad: decode %s failed\n", lump); return false; }
    TintHud((unsigned char *)img.data, img.width * img.height);
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.png", outDir, lump);
    bool ok = ExportImage(img, path);
    UnloadImage(img);
    printf("  baked %s -> %s\n", lump, path);
    return ok;
}

static int BakeMode(const char *basePath, const char *outDir)
{
    Wad base = { 0 };
    if (!WadLoad(&base, basePath)) { fprintf(stderr, "mkwad: cannot load base %s\n", basePath); return 1; }
    unsigned char pal[768];
    if (!DoomLoadPalette(&base, pal)) { fprintf(stderr, "mkwad: base has no PLAYPAL\n"); WadFree(&base); return 1; }
    bool ok = true;
    ok &= BakeOne(&base, pal, "STBAR", outDir);
    ok &= BakeOne(&base, pal, "STARMS", outDir);
    WadFree(&base);
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    SetTraceLogLevel(LOG_WARNING);
    if (argc >= 5 && strcmp(argv[1], "build") == 0)
        return BuildMode(argv[2], argv[3], argv[4]);
    if (argc >= 4 && strcmp(argv[1], "bake") == 0)
        return BakeMode(argv[2], argv[3]);
    fprintf(stderr,
        "usage:\n"
        "  mkwad build <base.wad> <overrides-dir> <out.wad>\n"
        "  mkwad bake  <base.wad> <out-graphics-dir>\n");
    return 2;
}
