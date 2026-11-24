#include "models.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raymath.h"
#include "rlgl.h"

// Present in raylib 6.2's rmodels.c, not exported by raylib.h: computes the
// bone matrices only (GPU skinning), skipping the CPU vertex rewrite.
void UpdateModelAnimationBones(Model model, ModelAnimation anim, float frame);

#define MAX_BONES 100
#define CLIP_FPS 60.0f   // raylib resamples glTF clips at 60 fps

#if defined(GRAPHICS_API_OPENGL_ES3)
#define GLSL_HEAD "#version 300 es\nprecision highp float;\n"
#else
#define GLSL_HEAD "#version 330\n"
#endif

static const char *MODEL_VS = GLSL_HEAD
    "in vec3 vertexPosition; in vec2 vertexTexCoord; in vec3 vertexNormal; in vec4 vertexColor;\n"
    "in vec4 vertexBoneIndices; in vec4 vertexBoneWeights;\n"
    "uniform mat4 mvp; uniform mat4 matModel; uniform mat4 matNormal;\n"
    "uniform mat4 boneMatrices[100]; uniform int skinned;\n"
    "out vec2 fragTexCoord; out vec3 fragNormal; out vec4 fragColor; out vec3 fragPos;\n"
    "void main() {\n"
    "    vec4 p = vec4(vertexPosition, 1.0); vec4 n = vec4(vertexNormal, 0.0);\n"
    "    if (skinned == 1) {\n"
    "        mat4 sk = boneMatrices[int(vertexBoneIndices.x)]*vertexBoneWeights.x\n"
    "                + boneMatrices[int(vertexBoneIndices.y)]*vertexBoneWeights.y\n"
    "                + boneMatrices[int(vertexBoneIndices.z)]*vertexBoneWeights.z\n"
    "                + boneMatrices[int(vertexBoneIndices.w)]*vertexBoneWeights.w;\n"
    "        p = sk*p; n = sk*n;\n"
    "    }\n"
    "    fragTexCoord = vertexTexCoord; fragColor = vertexColor;\n"
    "    fragNormal = normalize((matNormal*vec4(n.xyz, 0.0)).xyz);\n"
    "    fragPos = (matModel*p).xyz;\n"
    "    gl_Position = mvp*p;\n"
    "}\n";

// Key light from the upper-left like the iso view's implied sun, a soft
// fill from below, and a warm rim so silhouettes pop off dark floors.
static const char *MODEL_FS = GLSL_HEAD
    "in vec2 fragTexCoord; in vec3 fragNormal; in vec4 fragColor; in vec3 fragPos;\n"
    "uniform sampler2D texture0; uniform vec4 colDiffuse;\n"
    "uniform vec3 lightDir; uniform float ambient; uniform vec3 rimColor;\n"
    "uniform int lightCount; uniform vec4 lightPos[24]; uniform vec3 lightCol[24];\n"
    "uniform vec4 silhouette;\n"
    "out vec4 finalColor;\n"
    "void main() {\n"
    "    if (silhouette.a > 0.0) { finalColor = silhouette; return; }\n"
    "    vec4 albedo = texture(texture0, fragTexCoord)*colDiffuse;\n"
    "    vec3 n = normalize(fragNormal);\n"
    "    float key = max(dot(n, -lightDir), 0.0);\n"
    "    float fill = max(dot(n, vec3(0.3, -0.2, 0.6)), 0.0)*0.25;\n"
    "    float rim = pow(1.0 - max(n.y*0.4 + 0.6*abs(dot(n, normalize(vec3(-lightDir.x, 0.0, -lightDir.z)))), 0.0), 3.0);\n"
    "    vec3 dyn = vec3(0.0);\n"
    "    for (int i = 0; i < 24; i++) {\n"
    "        if (i >= lightCount) break;\n"
    "        vec3 d = lightPos[i].xyz - fragPos;\n"
    "        float f = max(0.0, 1.0 - length(d)/lightPos[i].w);\n"
    "        dyn += lightCol[i]*f*f*max(dot(n, normalize(d)), 0.2);\n"
    "    }\n"
    "    vec3 c = albedo.rgb*(ambient + key*0.95 + fill + dyn) + rimColor*rim*0.35;\n"
    // Opaque models: baked PNGs may carry junk alpha (0 made the agent's\n"
    "    // skinned models invisible); only the tint's alpha (spectre) counts.\n"
    "    finalColor = vec4(c, colDiffuse.a);\n"
    "}\n";

static const char *MOB_FILES[MK_COUNT] = {
    [MK_ZOMBIE] = "zombie", [MK_SHOTGUNGUY] = "shotgunner", [MK_IMP] = "imp",
    [MK_PINKY] = "pinky", [MK_SPECTRE] = "pinky", [MK_CACO] = "caco",
    [MK_LOSTSOUL] = "lostsoul", [MK_REVENANT] = "revenant", [MK_BARON] = "baron",
    [MK_ARCHVILE] = "archvile", [MK_CHAINGUNNER] = "chaingunner",
    [MK_HELLKNIGHT] = "hellknight", [MK_MANCUBUS] = "mancubus",
    [MK_PAINELEMENTAL] = "painelemental",
};
// In-game standing heights (map units): the sim's bodies, vanilla-ish.
static const float MOB_HEIGHT[MK_COUNT] = {
    [MK_ZOMBIE] = 56, [MK_SHOTGUNGUY] = 56, [MK_IMP] = 56, [MK_PINKY] = 56,
    [MK_SPECTRE] = 56, [MK_CACO] = 56, [MK_LOSTSOUL] = 32, [MK_REVENANT] = 64,
    [MK_BARON] = 64, [MK_ARCHVILE] = 60, [MK_CHAINGUNNER] = 56,
    [MK_HELLKNIGHT] = 64, [MK_MANCUBUS] = 64, [MK_PAINELEMENTAL] = 56,
};
static const char *PICKUP_FILES[PK_COUNT] = {
    [PK_STIM] = "pk_stimpack", [PK_MEDIKIT] = "pk_medikit", [PK_ARMOR] = "pk_armor",
    [PK_SHOTGUN] = "pk_shotgun", [PK_CLIP] = "pk_clip", [PK_CLIPBOX] = "pk_clipbox",
    [PK_SHELLS] = "pk_shells", [PK_SHELLBOX] = "pk_shellbox", [PK_CHAINGUN] = "pk_chaingun",
    [PK_RLAUNCH] = "pk_rlaunch", [PK_ROCKET] = "pk_rocket", [PK_ROCKETBOX] = "pk_rocketbox",
};
static const float PICKUP_HEIGHT[PK_COUNT] = {
    [PK_STIM] = 12, [PK_MEDIKIT] = 16, [PK_ARMOR] = 20, [PK_SHOTGUN] = 12,
    [PK_CLIP] = 8, [PK_CLIPBOX] = 14, [PK_SHELLS] = 8, [PK_SHELLBOX] = 14,
    [PK_CHAINGUN] = 16, [PK_RLAUNCH] = 16, [PK_ROCKET] = 20, [PK_ROCKETBOX] = 16,
};

static int FindClip(const ModelAnimation *anims, int n, const char *name)
{
    for (int i = 0; i < n; i++) if (strcmp(anims[i].name, name) == 0) return i;
    for (int i = 0; i < n; i++) if (strstr(anims[i].name, name)) return i;   // "Armature|walk"
    return -1;
}

static bool LoadOne(GameModels *gm, GameModel *out, const char *dir, const char *name, float height)
{
    memset(out, 0, sizeof(*out));
    for (int c = 0; c < CLIP_COUNT; c++) out->clip[c] = -1;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.glb", dir, name);
    if (!FileExists(path)) return false;
    out->model = LoadModel(path);
    if (out->model.meshCount <= 0) { UnloadModel(out->model); return false; }
    BoundingBox bb = GetModelBoundingBox(out->model);
    float h = bb.max.y - bb.min.y;
    if (h <= 0.0001f) h = 1.0f;
    out->scale = height/h;
    out->yOffset = -bb.min.y*out->scale;
    for (int i = 0; i < out->model.materialCount; i++) {
        out->model.materials[i].shader = gm->shader;
        Texture2D *t = &out->model.materials[i].maps[MATERIAL_MAP_DIFFUSE].texture;
        if (t->id > 1 && t->mipmaps <= 1) {
            GenTextureMipmaps(t);
            if (t->mipmaps > 1) SetTextureFilter(*t, TEXTURE_FILTER_TRILINEAR);
            else SetTextureFilter(*t, TEXTURE_FILTER_BILINEAR);
        }
    }
    out->skinned = out->model.skeleton.boneCount > 0 && out->model.boneMatrices != NULL;
    if (out->skinned) {
        out->anims = LoadModelAnimations(path, &out->animCount);
        static const char *NAMES[CLIP_COUNT] = { "idle", "walk", "attack", "pain", "death" };
        for (int c = 0; c < CLIP_COUNT; c++) out->clip[c] = FindClip(out->anims, out->animCount, NAMES[c]);
        if (out->clip[CLIP_IDLE] < 0) out->clip[CLIP_IDLE] = out->clip[CLIP_WALK];
        if (out->clip[CLIP_WALK] < 0) out->clip[CLIP_WALK] = out->clip[CLIP_IDLE];
    }
    out->loaded = true;
    TraceLog(LOG_INFO, "MODELS: %s: %d meshes, %u bones, %d clips, scale %.3f",
             name, out->model.meshCount, out->model.skeleton.boneCount, out->animCount, out->scale);
    return true;
}

bool GameModelsLoad(GameModels *gm, const char *dir)
{
    memset(gm, 0, sizeof(*gm));
    if (!DirectoryExists(dir)) return false;
    gm->shader = LoadShaderFromMemory(MODEL_VS, MODEL_FS);
    if (gm->shader.id == 0) return false;
    gm->cpuSkin = getenv("HELLDROP_CPU_SKIN") != NULL;
#ifdef PLATFORM_WEB
    // WebGL 2: GPU skinning produced stretched "spike" vertices in Chrome;
    // CPU skinning renders correctly and culling keeps it cheap.
    gm->cpuSkin = true;
#endif
    if (gm->cpuSkin) {
        // Fallback: raylib's CPU skinning (rewrites the vertex buffer per
        // draw). GPU skinning is the default: same pixels, far cheaper.
        gm->shader.locs[SHADER_LOC_VERTEX_BONEIDS] = -1;
        gm->shader.locs[SHADER_LOC_VERTEX_BONEWEIGHTS] = -1;
        gm->shader.locs[SHADER_LOC_MATRIX_BONETRANSFORMS] = -1;
    } else {
        gm->shader.locs[SHADER_LOC_VERTEX_BONEIDS] = GetShaderLocationAttrib(gm->shader, "vertexBoneIndices");
        gm->shader.locs[SHADER_LOC_VERTEX_BONEWEIGHTS] = GetShaderLocationAttrib(gm->shader, "vertexBoneWeights");
        gm->shader.locs[SHADER_LOC_MATRIX_BONETRANSFORMS] = GetShaderLocation(gm->shader, "boneMatrices");
    }
    gm->shader.locs[SHADER_LOC_MATRIX_NORMAL] = GetShaderLocation(gm->shader, "matNormal");
    gm->shader.locs[SHADER_LOC_MATRIX_MODEL] = GetShaderLocation(gm->shader, "matModel");
    gm->locSkinned = GetShaderLocation(gm->shader, "skinned");
    gm->locLightDir = GetShaderLocation(gm->shader, "lightDir");
    gm->locAmbient = GetShaderLocation(gm->shader, "ambient");
    gm->locRim = GetShaderLocation(gm->shader, "rimColor");
    gm->locSil = GetShaderLocation(gm->shader, "silhouette");
    Vector3 ld = Vector3Normalize((Vector3){ 0.45f, -0.8f, -0.35f });
    SetShaderValue(gm->shader, gm->locLightDir, &ld, SHADER_UNIFORM_VEC3);
    Vector3 rim = { 1.0f, 0.55f, 0.35f };
    SetShaderValue(gm->shader, gm->locRim, &rim, SHADER_UNIFORM_VEC3);

    for (int k = 0; k < MK_COUNT; k++) {
        // Spectre shares the pinky's model (drawn translucent): load once.
        if (k == MK_SPECTRE) continue;
        if (LoadOne(gm, &gm->mob[k], dir, MOB_FILES[k], MOB_HEIGHT[k])) gm->any = true;
    }
    if (gm->mob[MK_PINKY].loaded) { gm->mob[MK_SPECTRE] = gm->mob[MK_PINKY]; }
    if (LoadOne(gm, &gm->marine, dir, "marine", 56.0f)) gm->any = true;
    if (LoadOne(gm, &gm->barrel, dir, "barrel", 32.0f)) gm->any = true;
    for (int p = 0; p < PK_COUNT; p++) {
        if (LoadOne(gm, &gm->pickup[p], dir, PICKUP_FILES[p], PICKUP_HEIGHT[p])) gm->any = true;
    }
    gm->enabled = gm->any;
    if (!gm->any) { UnloadShader(gm->shader); gm->shader = (Shader){ 0 }; }
    return gm->any;
}

static void UnloadOne(GameModel *m)
{
    if (!m->loaded) return;
    if (m->anims) UnloadModelAnimations(m->anims, m->animCount);
    for (int i = 0; i < m->model.materialCount; i++) m->model.materials[i].shader = (Shader){ 0 };
    UnloadModel(m->model);
    memset(m, 0, sizeof(*m));
}

void GameModelsUnload(GameModels *gm)
{
    if (!gm) return;
    for (int k = 0; k < MK_COUNT; k++) {
        if (gm->hasPortrait[k]) UnloadRenderTexture(gm->portrait[k]);
        if (k == MK_SPECTRE) { memset(&gm->mob[k], 0, sizeof(GameModel)); continue; }
        UnloadOne(&gm->mob[k]);
    }
    UnloadOne(&gm->marine);
    UnloadOne(&gm->barrel);
    for (int p = 0; p < PK_COUNT; p++) UnloadOne(&gm->pickup[p]);
    if (gm->shader.id) UnloadShader(gm->shader);
    memset(gm, 0, sizeof(*gm));
}

void GameModelDraw(const GameModels *gm, const GameModel *m, Vector3 feet,
                   float yawDeg, ModelClip clip, float clipTime, bool loop,
                   Color tint, float light)
{
    GameModelDrawEx(gm, m, feet, yawDeg, clip, clipTime, loop, tint, light, (Color){ 0 });
}

void GameModelDrawEx(const GameModels *gm, const GameModel *m, Vector3 feet,
                     float yawDeg, ModelClip clip, float clipTime, bool loop,
                     Color tint, float light, Color silhouette)
{
    if (!m->loaded) return;
    int skinned = 0;
    if (m->skinned) {
        int ci = m->clip[clip];
        if (ci < 0) ci = m->clip[CLIP_IDLE];
        if (ci >= 0) {
            const ModelAnimation *a = &m->anims[ci];
            float f = clipTime*CLIP_FPS;
            float last = (float)(a->keyframeCount - 1);
            if (loop && a->keyframeCount > 1) f = fmodf(f, last);
            else if (f > last) f = last;
            if (f < 0.0f) f = 0.0f;
            if (gm->cpuSkin) UpdateModelAnimation(m->model, *a, f);
            else { UpdateModelAnimationBones(m->model, *a, f); skinned = 1; }
        }
    }
    SetShaderValue(gm->shader, gm->locSkinned, &skinned, SHADER_UNIFORM_INT);
    float amb = 0.30f + 0.35f*light;
    SetShaderValue(gm->shader, gm->locAmbient, &amb, SHADER_UNIFORM_FLOAT);
    Vector3 pos = { feet.x, feet.y + m->yOffset, feet.z };
    float s = m->scale;
    // Doom angle θ (CCW from east) faces world (cos θ, 0, -sin θ); the
    // model faces +Z, so it turns θ + 90° about Y.
    if (silhouette.a > 0) {
        Vector4 sc = ColorNormalize(silhouette);
        SetShaderValue(gm->shader, gm->locSil, &sc, SHADER_UNIFORM_VEC4);
        rlDrawRenderBatchActive();
        rlDisableDepthTest();
        rlDisableDepthMask();
        DrawModelEx(m->model, pos, (Vector3){ 0, 1, 0 }, yawDeg + 90.0f,
                    (Vector3){ s, s, s }, WHITE);
        rlDrawRenderBatchActive();
        rlEnableDepthMask();
        rlEnableDepthTest();
        Vector4 off = { 0, 0, 0, 0 };
        SetShaderValue(gm->shader, gm->locSil, &off, SHADER_UNIFORM_VEC4);
    }
    DrawModelEx(m->model, pos, (Vector3){ 0, 1, 0 }, yawDeg + 90.0f,
                (Vector3){ s, s, s }, tint);
}

void MobModelPose(const Mob *mob, int idx, long tic, float alpha,
                  ModelClip *clip, float *t, bool *loop)
{
    float now = ((float)tic + alpha)/35.0f;
    *loop = false;
    switch (mob->state) {
    case MOB_DEAD:
        *clip = CLIP_DEATH;
        *t = ((float)(tic - mob->deathTic) + alpha)/35.0f;
        return;
    case MOB_PAIN:
        *clip = CLIP_PAIN;
        *t = ((float)(6 - mob->stateTics) + alpha)/35.0f;
        if (*t < 0.0f) *t = 0.0f;
        return;
    case MOB_ATTACK:
    case MOB_RAISE: {
        int total = (mob->kind == MK_ARCHVILE) ? 24 : 10;
        *clip = CLIP_ATTACK;
        float p = ((float)(total - mob->stateTics) + alpha)/(float)total;
        if (p < 0.0f) p = 0.0f;
        // Attack clips are ~0.6s with the hit at ~60%: map windup onto that.
        *t = p*0.6f;
        if (mob->state == MOB_RAISE) { *t = fmodf(now, 0.6f); *loop = true; }
        return;
    }
    case MOB_CHASE: {
        float dx = mob->pos.x - mob->prevPos.x, dy = mob->pos.y - mob->prevPos.y;
        bool moving = (dx*dx + dy*dy) > 0.04f;
        *clip = moving ? CLIP_WALK : CLIP_IDLE;
        *t = now + (float)idx*0.37f;
        *loop = true;
        return;
    }
    default:
        *clip = CLIP_IDLE;
        *t = now + (float)idx*0.53f;
        *loop = true;
        return;
    }
}

void DgModelPose(const DoomGuy *dg, long tic, float alpha, ModelClip *clip,
                 float *t, bool *loop)
{
    float now = ((float)tic + alpha)/35.0f;
    *loop = true;
    if (dg->state == DG_DEAD) {
        *clip = CLIP_DEATH; *loop = false;
        *t = ((float)(tic - dg->deathTic) + alpha)/35.0f;
        return;
    }
    int cdMax = (dg->weapon == WPN_SHOTGUN) ? 37 : (dg->weapon == WPN_ROCKET) ? 30
              : (dg->weapon == WPN_CHAINGUN) ? 4 : 10;
    if (dg->state == DG_FIGHT && dg->fireCooldown > cdMax - 8) {
        *clip = CLIP_ATTACK; *loop = false;
        *t = ((float)(cdMax - dg->fireCooldown) + alpha)/35.0f + 0.25f;
        return;
    }
    float dx = dg->pos.x - dg->prevPos.x, dy = dg->pos.y - dg->prevPos.y;
    bool moving = (dx*dx + dy*dy) > 0.25f;
    *clip = moving ? CLIP_WALK : CLIP_IDLE;
    // He runs (8 u/tic): play the walk cycle fast.
    *t = moving ? now*1.6f : now;
}

void GameModelsRenderPortraits(GameModels *gm)
{
    for (int k = 0; k < MK_COUNT; k++) {
        const GameModel *m = &gm->mob[k];
        if (!m->loaded) continue;
        gm->portrait[k] = LoadRenderTexture(96, 96);
        if (gm->portrait[k].id == 0) continue;
        gm->hasPortrait[k] = true;
        float h = MOB_HEIGHT[k];
        Camera3D cam = { 0 };
        cam.target = (Vector3){ 0, h*0.55f, 0 };
        cam.position = (Vector3){ h*1.1f, h*1.1f, h*1.6f };
        cam.up = (Vector3){ 0, 1, 0 };
        cam.fovy = h*1.35f;
        cam.projection = CAMERA_ORTHOGRAPHIC;
        BeginTextureMode(gm->portrait[k]);
        ClearBackground((Color){ 0, 0, 0, 0 });
        BeginMode3D(cam);
        GameModelDraw(gm, m, (Vector3){ 0, 0, 0 }, -60.0f, CLIP_IDLE, 0.0f, true,
                      (k == MK_SPECTRE) ? (Color){ 120, 120, 140, 255 } : WHITE, 1.0f);
        EndMode3D();
        EndTextureMode();
    }
}
