#include "wad.h"
#include "raylib.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAD_HEADER_SIZE 12
#define WAD_DIRENT_SIZE 16

static uint32_t ReadU32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t ReadI32(const unsigned char *p)
{
    return (int32_t)ReadU32(p);
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

static bool NameEq(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return *a == *b;
}

bool WadLoad(Wad *wad, const char *path)
{
    memset(wad, 0, sizeof(*wad));

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        TraceLog(LOG_ERROR, "WAD: cannot open file: %s", path);
        return false;
    }

    long fileLen = -1;
    if (fseek(f, 0, SEEK_END) == 0) fileLen = ftell(f);
    if (fileLen < WAD_HEADER_SIZE || fseek(f, 0, SEEK_SET) != 0) {
        TraceLog(LOG_ERROR, "WAD: %s is not a WAD (too small or unreadable)", path);
        fclose(f);
        return false;
    }

    wad->size = (size_t)fileLen;
    wad->data = (unsigned char *)malloc(wad->size);
    if (wad->data == NULL || fread(wad->data, 1, wad->size, f) != wad->size) {
        TraceLog(LOG_ERROR, "WAD: failed to read %zu bytes from %s", wad->size, path);
        fclose(f);
        WadFree(wad);
        return false;
    }
    fclose(f);

    if (memcmp(wad->data, "IWAD", 4) != 0 && memcmp(wad->data, "PWAD", 4) != 0) {
        TraceLog(LOG_ERROR, "WAD: %s has bad magic (not IWAD/PWAD)", path);
        WadFree(wad);
        return false;
    }
    memcpy(wad->type, wad->data, 4);
    wad->type[4] = '\0';

    int32_t lumpCount = ReadI32(wad->data + 4);
    int32_t dirOfs = ReadI32(wad->data + 8);
    if (lumpCount < 0 || dirOfs < 0 ||
        (uint64_t)dirOfs + (uint64_t)lumpCount * WAD_DIRENT_SIZE > (uint64_t)wad->size) {
        TraceLog(LOG_ERROR, "WAD: %s directory out of bounds (count=%d ofs=%d size=%zu)",
                 path, lumpCount, dirOfs, wad->size);
        WadFree(wad);
        return false;
    }

    wad->lumpCount = lumpCount;
    wad->lumps = (WadLump *)calloc((size_t)lumpCount > 0 ? (size_t)lumpCount : 1,
                                   sizeof(WadLump));
    if (wad->lumps == NULL) {
        TraceLog(LOG_ERROR, "WAD: out of memory for %d lumps", lumpCount);
        WadFree(wad);
        return false;
    }

    for (int i = 0; i < lumpCount; i++) {
        const unsigned char *ent = wad->data + dirOfs + (size_t)i * WAD_DIRENT_SIZE;
        WadLump *lump = &wad->lumps[i];
        lump->filepos = ReadU32(ent);
        lump->size = ReadU32(ent + 4);
        CopyName8(lump->name, ent + 8);
        if ((uint64_t)lump->filepos + (uint64_t)lump->size > (uint64_t)wad->size) {
            TraceLog(LOG_ERROR, "WAD: lump %d (%s) out of bounds (pos=%u size=%u file=%zu)",
                     i, lump->name, lump->filepos, lump->size, wad->size);
            WadFree(wad);
            return false;
        }
    }

    return true;
}

void WadFree(Wad *wad)
{
    if (wad == NULL) return;
    free(wad->data);
    free(wad->lumps);
    memset(wad, 0, sizeof(*wad));
}

int WadFindLump(const Wad *wad, const char *name, int from)
{
    if (wad == NULL || wad->lumps == NULL || name == NULL) return -1;
    if (from < 0) from = 0;
    for (int i = from; i < wad->lumpCount; i++) {
        if (NameEq(wad->lumps[i].name, name)) return i;
    }
    return -1;
}

const unsigned char *WadLumpData(const Wad *wad, int index, int *size)
{
    if (size != NULL) *size = 0;
    if (wad == NULL || wad->data == NULL || index < 0 || index >= wad->lumpCount) {
        return NULL;
    }
    const WadLump *lump = &wad->lumps[index];
    if (size != NULL) *size = (int)lump->size;
    return wad->data + lump->filepos;
}
