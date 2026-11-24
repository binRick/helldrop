#include "sprites.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "doomgfx.h"
#include "raymath.h"

// Per-kind animation tables (vanilla frame letters).
typedef struct MobAnim {
    const char *lump;     // 4-char sprite name
    const char *walk;     // cycled while idle/chasing
    const char *attack;   // played across the ATTACK windup
    char pain;
    const char *death;    // played once from deathTic, holds last frame
    const char *heal;     // arch-vile resurrection pose, NULL otherwise
} MobAnim;

static const MobAnim MOB_ANIM[MK_COUNT] = {
    [MK_ZOMBIE]     = { "POSS", "ABCD",   "EF",  'G', "HIJKL", NULL },
    [MK_SHOTGUNGUY] = { "SPOS", "ABCD",   "EF",  'G', "HIJKL", NULL },
    [MK_IMP]        = { "TROO", "ABCD",   "EFG", 'H', "IJKLM", NULL },
    [MK_PINKY]      = { "SARG", "ABCD",   "EFG", 'H', "IJKLMN", NULL },
    [MK_SPECTRE]    = { "SARG", "ABCD",   "EFG", 'H', "IJKLMN", NULL },
    [MK_CACO]       = { "HEAD", "A",      "BCD", 'E', "FGHIJK", NULL },
    [MK_LOSTSOUL]   = { "SKUL", "AB",     "CD",  'E', "FGHIJK", NULL },
    [MK_REVENANT]   = { "SKEL", "ABCDEF", "JK",  'L', "LMNOPQ", NULL },
    [MK_BARON]      = { "BOSS", "ABCD",   "EFG", 'H', "IJKLMNO", NULL },
    [MK_ARCHVILE]   = { "VILE", "ABCDEF", "GHIJKLMNOP", 'Q', "RSTUVWXYZ", "[\\]" },
    [MK_CHAINGUNNER]   = { "CPOS", "ABCD",   "EF",  'G', "HIJKLM", NULL },
    [MK_HELLKNIGHT]    = { "BOS2", "ABCD",   "EFG", 'H', "IJKLMNO", NULL },
    [MK_MANCUBUS]      = { "FATT", "ABCDEF", "GHI", 'J', "KLMNOPQRST", NULL },
    [MK_PAINELEMENTAL] = { "PAIN", "ABC",    "DEF", 'G', "HIJKLM", NULL },
};

// Static map decorations (and pickups the sim doesn't simulate yet), by
// doomednum: sprite name + vanilla animation frame sequence. Hanging
// ceiling gore is omitted: there are no ceilings in the iso view.
typedef struct DecoDef { int ednum; const char *lump; const char *seq; } DecoDef;
static const DecoDef DECO[] = {
    { 2028, "COLU", "A" }, { 30, "COL1", "A" }, { 31, "COL2", "A" }, { 32, "COL3", "A" },
    { 33, "COL4", "A" }, { 36, "COL5", "AB" }, { 37, "COL6", "A" }, { 34, "CAND", "A" },
    { 35, "CBRA", "A" }, { 41, "CEYE", "ABCB" }, { 42, "FSKU", "ABC" }, { 43, "TRE1", "A" },
    { 44, "TBLU", "ABCD" }, { 45, "TGRN", "ABCD" }, { 46, "TRED", "ABCD" }, { 47, "SMIT", "A" },
    { 48, "ELEC", "A" }, { 54, "TRE2", "A" }, { 55, "SMBT", "ABCD" }, { 56, "SMGT", "ABCD" },
    { 57, "SMRT", "ABCD" }, { 70, "FCAN", "ABC" }, { 85, "TLMP", "ABCD" }, { 86, "TLP2", "ABCD" },
    { 10, "PLAY", "W" }, { 12, "PLAY", "W" }, { 15, "PLAY", "N" }, { 24, "POL5", "A" },
    { 18, "POSS", "L" }, { 19, "SPOS", "L" }, { 20, "TROO", "M" }, { 21, "SARG", "N" },
    { 22, "HEAD", "L" }, { 23, "SKUL", "K" }, { 25, "POL1", "A" }, { 26, "POL6", "AB" },
    { 27, "POL4", "A" }, { 28, "POL2", "A" }, { 29, "POL3", "AB" }, { 79, "POB1", "A" },
    { 80, "POB2", "A" }, { 81, "BRS1", "A" },
    { 5, "BKEY", "AB" }, { 6, "YKEY", "AB" }, { 13, "RKEY", "AB" },
    { 38, "RSKU", "AB" }, { 39, "YSKU", "AB" }, { 40, "BSKU", "AB" },
    { 2013, "SOUL", "ABCDCB" }, { 2014, "BON1", "ABCDCB" }, { 2015, "BON2", "ABCDCB" },
    { 2019, "ARM2", "AB" }, { 2022, "PINV", "ABCD" }, { 2023, "PSTR", "A" },
    { 2024, "PINS", "ABCD" }, { 2025, "SUIT", "A" }, { 2026, "PMAP", "ABCDCB" },
    { 2045, "PVIS", "AB" }, { 8, "BPAK", "A" }, { 2004, "PLAS", "A" }, { 2005, "CSAW", "A" },
    { 2006, "BFUG", "A" }, { 17, "CELP", "A" }, { 2047, "CELL", "A" }, { 83, "MEGA", "ABCD" },
};
#define DECO_COUNT ((int)(sizeof(DECO)/sizeof(DECO[0])))

static const MobAnim PLAYER_ANIM = { "PLAY", "ABCD", "EF", 'G', "HIJKLMN", NULL };

static const char *PICKUP_LUMPS[PK_COUNT] = {
    [PK_STIM] = "STIM", [PK_MEDIKIT] = "MEDI", [PK_ARMOR] = "ARM1",
    [PK_SHOTGUN] = "SHOT", [PK_CLIP] = "CLIP", [PK_CLIPBOX] = "AMMO",
    [PK_SHELLS] = "SHEL", [PK_SHELLBOX] = "SBOX",
    [PK_CHAINGUN] = "MGUN", [PK_RLAUNCH] = "LAUN",
    [PK_ROCKET] = "ROCK", [PK_ROCKETBOX] = "BROK",
};

// Same alpha-test discard shader, two dialects: desktop GL 3.3 core, and
// GLSL ES 3.00 for WebGL 2 (PLATFORM_WEB) — which additionally requires an
// explicit float precision qualifier. Bodies are otherwise identical.
#if defined(GRAPHICS_API_OPENGL_ES3)
static const char *ALPHA_TEST_FS =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 fragTexCoord; in vec4 fragColor;\n"
    "uniform sampler2D texture0; uniform vec4 colDiffuse;\n"
    "out vec4 finalColor;\n"
    "void main() {\n"
    "    vec4 t = texture(texture0, fragTexCoord);\n"
    "    if (t.a < 0.5) discard;\n"
    "    finalColor = t*colDiffuse*fragColor;\n"
    "}\n";
#else
static const char *ALPHA_TEST_FS =
    "#version 330\n"
    "in vec2 fragTexCoord; in vec4 fragColor;\n"
    "uniform sampler2D texture0; uniform vec4 colDiffuse;\n"
    "out vec4 finalColor;\n"
    "void main() {\n"
    "    vec4 t = texture(texture0, fragTexCoord);\n"
    "    if (t.a < 0.5) discard;\n"
    "    finalColor = t*colDiffuse*fragColor;\n"
    "}\n";
#endif

static void SetRegister(SpriteSet *set, int frame, int rot, Texture2D tex,
                        bool mirror, short botOfs, unsigned char scale)
{
    if (frame < 0 || frame >= SPR_FRAMES) return;
    SpriteFrame *f = &set->frames[frame];
    if (rot == 0) {
        for (int r = 0; r < 8; r++) {
            f->tex[r] = tex;
            f->mirror[r] = false;
            f->botOfs[r] = botOfs;
            f->texelsPerUnit[r] = scale;
        }
    } else if (rot >= 1 && rot <= 8) {
        f->tex[rot - 1] = tex;
        f->mirror[rot - 1] = mirror;
        f->botOfs[rot - 1] = botOfs;
        f->texelsPerUnit[rot - 1] = scale;
    } else {
        return;
    }
    f->present = true;
    set->any = true;
}

// Refine the coverage of a sprite's outline at half-texel spacing while
// retaining high-contrast features such as the visor slit and weapon edges.
// Ignore RGB from clear source pixels to keep cutout edges free of dark halos.
static bool LoadDetailedPatch(const Wad *wad, int lump, const unsigned char *pal,
                              Texture2D *texture)
{
    Image source;
    if (!DoomPatchToImage(wad, lump, pal, &source)) return false;
    int width = source.width*2;
    int height = source.height*2;
    Color *pixels = malloc((size_t)width*height*sizeof(*pixels));
    if (!pixels) { UnloadImage(source); return false; }

    const Color *input = source.data;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int sx = x/2, sy = y/2;
            int nx = sx + (x%2 ? 1 : -1), ny = sy + (y%2 ? 1 : -1);
            Color center = input[sy*source.width + sx];
            Color samples[] = {
                center,
                nx >= 0 && nx < source.width ? input[sy*source.width + nx] : (Color){0},
                ny >= 0 && ny < source.height ? input[ny*source.width + sx] : (Color){0},
                nx >= 0 && nx < source.width && ny >= 0 && ny < source.height
                    ? input[ny*source.width + nx] : (Color){0},
            };
            const int weights[] = { 9, 3, 3, 1 };
            int coverage = 0, sumR = 0, sumG = 0, sumB = 0, total = 0;
            for (int n = 0; n < 4; n++) {
                Color sample = samples[n];
                if (!sample.a) continue;
                coverage += weights[n];
                int difference = abs(center.r - sample.r) + abs(center.g - sample.g) + abs(center.b - sample.b);
                if (n && center.a && difference > 100) continue;
                total += weights[n];
                sumR += sample.r*weights[n];
                sumG += sample.g*weights[n];
                sumB += sample.b*weights[n];
            }
            Color result = { 0 };
            if (total) {
                // A restrained highlight along the top edge and a small
                // contact shadow along the bottom give the cutout depth.
                float light = !samples[2].a ? (y%2 ? 0.92f : 1.10f) : 1.0f;
                result = (Color){ (unsigned char)fminf(255, light*sumR/total),
                                  (unsigned char)fminf(255, light*sumG/total),
                                  (unsigned char)fminf(255, light*sumB/total),
                                  (unsigned char)(coverage*255/16) };
            }
            pixels[y*width + x] = result;
        }
    }
    Image enhanced = { pixels, width, height, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    *texture = LoadTextureFromImage(enhanced);
    if (texture->id) SetTextureFilter(*texture, TEXTURE_FILTER_BILINEAR);
    free(pixels);
    UnloadImage(source);
    return texture->id != 0;
}

static void SpriteSetLoad(SpriteSet *set, const Wad *wad, const char *name4,
                          const unsigned char *pal, bool detailed)
{
    memset(set, 0, sizeof(*set));
    for (int i = 0; i < wad->lumpCount; i++) {
        const char *n = wad->lumps[i].name;
        size_t len = strlen(n);
        if ((len != 6 && len != 8) || strncmp(n, name4, 4) != 0) continue;
        Texture2D tex;
        unsigned char scale = detailed && LoadDetailedPatch(wad, i, pal, &tex) ? 2 : 1;
        if (scale == 1 && !DoomPatchToTexture(wad, i, pal, &tex)) continue;
        int w = 0, h = 0, lo = 0, to = 0;
        DoomPatchInfo(wad, i, &w, &h, &lo, &to);
        short botOfs = (short)(to - h);
        if (set->ownedCount < SPR_FRAMES*8) set->owned[set->ownedCount++] = tex;
        SetRegister(set, n[4] - 'A', n[5] - '0', tex, false, botOfs, scale);
        if (len == 8) SetRegister(set, n[6] - 'A', n[7] - '0', tex, true, botOfs, scale);
    }
}

static void SpriteSetUnload(SpriteSet *set)
{
    for (int i = 0; i < set->ownedCount; i++) UnloadTexture(set->owned[i]);
    memset(set, 0, sizeof(*set));
}

bool GameSpritesLoad(GameSprites *gs, const Wad *wad)
{
    memset(gs, 0, sizeof(*gs));
    unsigned char pal[768];
    if (!DoomLoadPalette(wad, pal)) return false;

    for (int k = 0; k < MK_COUNT; k++) {
        SpriteSetLoad(&gs->mobs[k], wad, MOB_ANIM[k].lump, pal, true);
    }
    SpriteSetLoad(&gs->player, wad, PLAYER_ANIM.lump, pal, true);
    SpriteSetLoad(&gs->ball[0], wad, "BAL1", pal, false);
    SpriteSetLoad(&gs->ball[1], wad, "BAL2", pal, false);
    SpriteSetLoad(&gs->baronBall, wad, "BAL7", pal, false);
    SpriteSetLoad(&gs->fatShot, wad, "MANF", pal, false);
    SpriteSetLoad(&gs->tracer, wad, "FATB", pal, false);
    SpriteSetLoad(&gs->tracerBoom, wad, "FBXP", pal, false);
    SpriteSetLoad(&gs->missile, wad, "MISL", pal, false);
    SpriteSetLoad(&gs->barrel, wad, "BAR1", pal, true);
    SpriteSetLoad(&gs->fire, wad, "FIRE", pal, false);
    for (int p = 0; p < PK_COUNT; p++) {
        SpriteSetLoad(&gs->pickups[p], wad, PICKUP_LUMPS[p], pal, true);
    }

    // Decorations share sprite sets by name (PLAY/POSS corpses reuse the
    // live sets' lumps but load their own copy: simple, and small).
    gs->deco = (SpriteSet *)calloc((size_t)DECO_COUNT, sizeof(SpriteSet));
    if (gs->deco) {
        for (int d = 0; d < DECO_COUNT; d++) {
            int same = -1;
            for (int e = 0; e < d && same < 0; e++) if (strcmp(DECO[e].lump, DECO[d].lump) == 0) same = e;
            if (same >= 0) { gs->decoAlias[d] = (short)same; continue; }
            gs->decoAlias[d] = (short)d;
            SpriteSetLoad(&gs->deco[d], wad, DECO[d].lump, pal, false);
        }
    }

    gs->alphaTest = LoadShaderFromMemory(NULL, ALPHA_TEST_FS);
    gs->valid = gs->player.any;
    for (int k = 0; k < MK_COUNT; k++) gs->valid = gs->valid || gs->mobs[k].any;
    return gs->valid;
}

void GameSpritesUnload(GameSprites *gs)
{
    if (!gs) return;
    for (int k = 0; k < MK_COUNT; k++) SpriteSetUnload(&gs->mobs[k]);
    SpriteSetUnload(&gs->player);
    SpriteSetUnload(&gs->ball[0]);
    SpriteSetUnload(&gs->ball[1]);
    SpriteSetUnload(&gs->baronBall);
    SpriteSetUnload(&gs->fatShot);
    SpriteSetUnload(&gs->tracer);
    SpriteSetUnload(&gs->tracerBoom);
    SpriteSetUnload(&gs->missile);
    SpriteSetUnload(&gs->barrel);
    SpriteSetUnload(&gs->fire);
    for (int p = 0; p < PK_COUNT; p++) SpriteSetUnload(&gs->pickups[p]);
    if (gs->deco) {
        for (int d = 0; d < DECO_COUNT; d++) if (gs->decoAlias[d] == d) SpriteSetUnload(&gs->deco[d]);
        free(gs->deco);
    }
    if (gs->alphaTest.id != 0) UnloadShader(gs->alphaTest);
    memset(gs, 0, sizeof(*gs));
}

static const SpriteFrame *Pick(const SpriteSet *set, char letter)
{
    int idx = letter - 'A';
    if (idx < 0 || idx >= SPR_FRAMES || !set->frames[idx].present) return NULL;
    return &set->frames[idx];
}

// Death sequences run at 5 tics per frame and hold the final corpse frame.
static const SpriteFrame *PickDeath(const SpriteSet *set, const char *seq,
                                    long ticsDead)
{
    int n = (int)strlen(seq);
    int i = (int)(ticsDead/5);
    if (i >= n) i = n - 1;
    return Pick(set, seq[i]);
}

const SpriteFrame *MobSpriteFrame(const GameSprites *gs, const Mob *m, long tic, float stride)
{
    const SpriteSet *set = &gs->mobs[m->kind];
    if (!set->any) return NULL;
    const MobAnim *an = &MOB_ANIM[m->kind];
    switch (m->state) {
    case MOB_DEAD:
        return PickDeath(set, an->death, tic - m->deathTic);
    case MOB_PAIN:
        return Pick(set, an->pain);
    case MOB_RAISE:
        if (an->heal) {
            int n = (int)strlen(an->heal);
            return Pick(set, an->heal[(tic/4) % n]);
        }
        return Pick(set, an->walk[0]);
    case MOB_ATTACK: {
        // stateTics counts windup -> 0; spread the attack frames across it.
        int total = (m->kind == MK_ARCHVILE) ? 24 : 10;
        int n = (int)strlen(an->attack);
        int i = (total - m->stateTics)*n/(total + 1);
        if (i < 0) i = 0;
        if (i >= n) i = n - 1;
        return Pick(set, an->attack[i]);
    }
    default: {
        int n = (int)strlen(an->walk);
        int frame = m->state == MOB_CHASE ? (int)stride % n : 0;
        if (MobIsFlyer(m->kind)) frame = (int)((tic/6) % n);
        return Pick(set, an->walk[frame]);
    }
    }
}

const SpriteFrame *DGSpriteFrame(const GameSprites *gs, const DoomGuy *dg, long tic, float stride)
{
    const SpriteSet *set = &gs->player;
    if (!set->any) return NULL;
    if (dg->state == DG_DEAD) return PickDeath(set, PLAYER_ANIM.death, tic - dg->deathTic);
    int cdMax = (dg->weapon == WPN_SHOTGUN) ? 37 : 10;
    if (dg->state == DG_FIGHT && dg->fireCooldown > cdMax - 6) {
        return Pick(set, PLAYER_ANIM.attack[(dg->fireCooldown > cdMax - 3) ? 1 : 0]);
    }
    int n = (int)strlen(PLAYER_ANIM.walk);
    return Pick(set, PLAYER_ANIM.walk[(int)stride % n]);
}

const SpriteFrame *PickupSpriteFrame(const GameSprites *gs, PickupType t, long tic)
{
    if (t < 0 || t >= PK_COUNT || !gs->pickups[t].any) return NULL;
    // Armor bonuses blink A/B in vanilla; everything else sits on A.
    if (t == PK_ARMOR && gs->pickups[t].frames[1].present) {
        return Pick(&gs->pickups[t], (tic/16) % 2 ? 'B' : 'A');
    }
    return Pick(&gs->pickups[t], 'A');
}

const SpriteFrame *ProjSpriteFrame(const GameSprites *gs, const Proj *p, long tic)
{
    if (p->isRocket) {
        if (!gs->missile.any) return NULL;
        return Pick(&gs->missile, 'A');
    }
    const SpriteSet *set = (p->ownerKind == MK_CACO) ? &gs->ball[1]
                         : (p->ownerKind == MK_REVENANT) ? &gs->tracer
                         : (p->ownerKind == MK_BARON ||
                            p->ownerKind == MK_HELLKNIGHT) ? &gs->baronBall
                         : (p->ownerKind == MK_MANCUBUS) ? &gs->fatShot
                                                         : &gs->ball[0];
    if (!set->any) return NULL;
    if (set->frames[1].present) return Pick(set, (tic/4) % 2 ? 'B' : 'A');
    return Pick(set, 'A');
}

// Explosions: MISL B-D for rockets, BAL* C-E for fireballs, FBXP A-C for
// revenant tracers, ~4 tics each.
const SpriteFrame *BoomSpriteFrame(const GameSprites *gs, BoomKind kind, long age)
{
    const SpriteSet *set;
    const char *seq;
    if (kind == BOOM_ROCKET || kind == BOOM_BARREL) { set = &gs->missile; seq = "BCD"; }
    else if (kind == BOOM_CACOBALL) { set = &gs->ball[1]; seq = "CDE"; }
    else if (kind == BOOM_TRACER) { set = &gs->tracerBoom; seq = "ABC"; }
    else if (kind == BOOM_BARONBALL) { set = &gs->baronBall; seq = "CDE"; }
    else { set = &gs->ball[0]; seq = "CDE"; }
    if (!set->any) return NULL;
    int i = (int)(age/4);
    if (i < 0 || i > 2) return NULL;
    return Pick(set, seq[i]);
}

const SpriteFrame *BarrelSpriteFrame(const GameSprites *gs, long tic)
{
    if (!gs->barrel.any) return NULL;
    if (gs->barrel.frames[1].present) {
        return Pick(&gs->barrel, (tic/8) % 2 ? 'B' : 'A');
    }
    return Pick(&gs->barrel, 'A');
}

const SpriteFrame *FireSpriteFrame(const GameSprites *gs, long tic)
{
    if (!gs->fire.any) return NULL;
    int n = 0;
    while (n < 8 && gs->fire.frames[n].present) n++;
    if (n == 0) return NULL;
    return Pick(&gs->fire, (char)('A' + (tic/3) % n));
}

int SpriteRotationFrom(float entAngleDeg, Vector2 viewerDoom, Vector2 entDoom)
{
    float angToCam = atan2f(viewerDoom.y - entDoom.y,
                            viewerDoom.x - entDoom.x)*RAD2DEG;
    float rel = entAngleDeg - angToCam + 22.5f;
    int rot = (int)floorf(rel/45.0f) % 8;
    if (rot < 0) rot += 8;
    return rot;
}

static void DrawSpritePose(Camera3D cam, const SpriteFrame *f, int rot,
                           Vector3 base, Color tint, float sway, float stretch)
{
    Texture2D tex = f->tex[rot];
    if (tex.id == 0) {
        rot = 0;
        tex = f->tex[0];
        if (tex.id == 0) return;
    }
    Rectangle src = { 0, 0, f->mirror[rot] ? -(float)tex.width : (float)tex.width, (float)tex.height };
    float scale = f->texelsPerUnit[rot];
    float w = (float)tex.width/scale, h = (float)tex.height/scale;
    // Authentic anchoring: the patch's topoffset places the sprite bottom
    // relative to the entity origin (cacos hang, fireballs center, feet
    // plant) — exactly as Doom positions them.
    Vector3 bottom = { base.x, base.y + (float)f->botOfs[rot], base.z };
    DrawBillboardPro(cam, tex, src, bottom, (Vector3){ 0.0f, 1.0f, 0.0f },
                     (Vector2){ w, h*stretch }, (Vector2){ w*0.5f, 0.0f }, sway, tint);
}

void DrawSpriteBillboard(Camera3D cam, const SpriteFrame *f, int rot,
                         Vector3 base, Color tint)
{
    DrawSpritePose(cam, f, rot, base, tint, 0.0f, 1.0f);
}

void DrawWalkingSprite(Camera3D cam, const SpriteFrame *f, int rot,
                       Vector3 base, Color tint, float stride, float motion)
{
    float step = stride*PI/2.0f;
    DrawSpritePose(cam, f, rot, base, tint, 2.8f*sinf(step)*motion,
                   1.0f + (0.012f - 0.022f*cosf(step*2.0f))*motion);
}

const SpriteFrame *DecoSpriteFrame(const GameSprites *gs, int ednum, long tic)
{
    if (!gs->deco) return NULL;
    for (int d = 0; d < DECO_COUNT; d++) {
        if (DECO[d].ednum != ednum) continue;
        const SpriteSet *set = &gs->deco[gs->decoAlias[d]];
        if (!set->any) return NULL;
        int n = (int)strlen(DECO[d].seq);
        return Pick(set, DECO[d].seq[(tic/6) % n]);
    }
    return NULL;
}
