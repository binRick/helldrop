#ifndef HELLDROP_SPRITES_H
#define HELLDROP_SPRITES_H

#include <stdbool.h>
#include "raylib.h"
#include "wad.h"
#include "sim.h"

// Doom sprites as billboards with the authentic 8-rotation system: lump
// names are NAME + frame letter + rotation digit, with mirrored pairs
// packed as e.g. TROOA2A8 (one patch serving rotation 2 and, flipped, 8).
// Rotation 0 lumps face every direction. All sets are optional — a missing
// monster falls back to the capsule renderer.

#define SPR_FRAMES 29   // frame letters A..Z plus [ \ ] (arch-vile heal)

typedef struct SpriteFrame {
    Texture2D tex[8];   // by rotation, 0 = facing the viewer
    bool mirror[8];
    bool present;
} SpriteFrame;

typedef struct SpriteSet {
    SpriteFrame frames[SPR_FRAMES];
    Texture2D owned[SPR_FRAMES*8];   // unique textures for unload
    int ownedCount;
    bool any;
} SpriteSet;

typedef struct GameSprites {
    SpriteSet mobs[MK_COUNT];
    SpriteSet player;
    SpriteSet ball[2];          // BAL1 (imp), BAL2 (caco)
    SpriteSet baronBall;        // BAL7
    SpriteSet tracer;           // FATB: revenant homing missile (FBXP boom)
    SpriteSet tracerBoom;
    SpriteSet missile;          // MISL: his rocket + its explosion frames
    SpriteSet barrel;           // BAR1: exploding barrels
    SpriteSet fire;             // FIRE: arch-vile flame on his target
    SpriteSet pickups[PK_COUNT];
    Shader alphaTest;           // discard shader: depth-correct billboards
    bool valid;
} GameSprites;

bool GameSpritesLoad(GameSprites *gs, const Wad *wad);
void GameSpritesUnload(GameSprites *gs);

// Frame selection: returns NULL when the needed frame is missing (caller
// falls back to shapes). `tic` drives walk cycles and death sequences.
const SpriteFrame *MobSpriteFrame(const GameSprites *gs, const Mob *m, long tic);
const SpriteFrame *DGSpriteFrame(const GameSprites *gs, const DoomGuy *dg, long tic);
const SpriteFrame *PickupSpriteFrame(const GameSprites *gs, PickupType t, long tic);
const SpriteFrame *ProjSpriteFrame(const GameSprites *gs, const Proj *p, long tic);
const SpriteFrame *BoomSpriteFrame(const GameSprites *gs, BoomKind kind, long age);
const SpriteFrame *FireSpriteFrame(const GameSprites *gs, long tic);
const SpriteFrame *BarrelSpriteFrame(const GameSprites *gs, long tic);

// Doom rotation index for an entity facing entAngle (degrees, doom coords)
// seen by a viewer at viewerDoom. Works for the iso camera (pass its
// world position converted to doom coords) and the first-person doomcam.
int SpriteRotationFrom(float entAngleDeg, Vector2 viewerDoom, Vector2 entDoom);

// Billboard with feet anchored at base (world coords), 1 sprite px = 1 unit.
void DrawSpriteBillboard(Camera3D cam, const SpriteFrame *f, int rot,
                         Vector3 base, Color tint);

#endif
