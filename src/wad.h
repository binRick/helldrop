#ifndef HELLDROP_WAD_H
#define HELLDROP_WAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Doom WAD container. The whole file is held in memory; lump accessors
// return pointers into that buffer, valid until WadFree.
typedef struct WadLump {
    char name[9];     // uppercase, NUL-terminated (WAD stores 8 bytes, padded)
    uint32_t filepos;
    uint32_t size;
} WadLump;

typedef struct Wad {
    unsigned char *data;
    size_t size;
    char type[5];     // "IWAD" or "PWAD"
    int lumpCount;
    WadLump *lumps;
} Wad;

bool WadLoad(Wad *wad, const char *path);
void WadFree(Wad *wad);

// Linear scan starting at index `from` (pass 0 for whole-file search).
// Returns lump index or -1. Names compare case-insensitively.
int WadFindLump(const Wad *wad, const char *name, int from);

// Pointer into wad->data for lump `index`; *size receives byte length.
// Returns NULL (and *size 0) for out-of-range indices.
const unsigned char *WadLumpData(const Wad *wad, int index, int *size);

#endif
