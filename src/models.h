#ifndef HELLDROP_MODELS_H
#define HELLDROP_MODELS_H

#include <stdbool.h>
#include "raylib.h"
#include "sim.h"

// Animated 3D models (Blender-built GLBs in assets/models/) for the marine,
// every demon, barrels and pickups. Optional per model: anything missing
// falls back to the WAD sprite. Contract with tools/models (see its
// README): feet at the origin, facing glTF +Z, clips named idle / walk /
// attack / pain / death (death's last frame is the corpse).
typedef enum ModelClip { CLIP_IDLE, CLIP_WALK, CLIP_ATTACK, CLIP_PAIN, CLIP_DEATH, CLIP_COUNT } ModelClip;

typedef struct GameModel {
    Model model;
    ModelAnimation *anims;
    int animCount;
    int clip[CLIP_COUNT];      // index into anims, -1 missing
    float scale;               // model units -> map units
    float yOffset;             // lifts the bind-pose bottom onto the floor
    bool skinned;
    bool loaded;
} GameModel;

typedef struct GameModels {
    GameModel mob[MK_COUNT];
    GameModel marine;
    GameModel barrel;
    GameModel pickup[PK_COUNT];
    Shader shader;
    int locSkinned, locLightDir, locBones, locAmbient, locRim;
    RenderTexture2D portrait[MK_COUNT];   // idle-pose thumbnails for the HUD
    bool hasPortrait[MK_COUNT];
    bool any;
    bool enabled;              // M toggles models <-> authentic sprites
} GameModels;

bool GameModelsLoad(GameModels *gm, const char *dir);
void GameModelsUnload(GameModels *gm);
void GameModelsRenderPortraits(GameModels *gm);

// Draw one entity. clipTime is seconds into the clip; loop wraps it, else
// it clamps on the last frame. yawDeg is the Doom angle (CCW from east).
void GameModelDraw(const GameModels *gm, const GameModel *m, Vector3 feet,
                   float yawDeg, ModelClip clip, float clipTime, bool loop,
                   Color tint, float light);

// Sim state -> clip choice for monsters and the marine (render-only).
void MobModelPose(const Mob *mob, int idx, long tic, float alpha,
                  ModelClip *clip, float *t, bool *loop);
void DgModelPose(const DoomGuy *dg, long tic, float alpha, ModelClip *clip,
                 float *t, bool *loop);

#endif
