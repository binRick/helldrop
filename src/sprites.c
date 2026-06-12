#include "sprites.h"

#include <math.h>
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
    [MK_CACO]       = { "HEAD", "A",      "BCD", 'E', "FGHIJK", NULL },
    [MK_ARCHVILE]   = { "VILE", "ABCDEF", "GHIJKLMNOP", 'Q', "RSTUVWXYZ", "[\\]" },
};

static const MobAnim PLAYER_ANIM = { "PLAY", "ABCD", "EF", 'G', "HIJKLMN", NULL };

static const char *PICKUP_LUMPS[PK_COUNT] = {
    [PK_STIM] = "STIM", [PK_MEDIKIT] = "MEDI", [PK_ARMOR] = "ARM1",
    [PK_SHOTGUN] = "SHOT", [PK_CLIP] = "CLIP", [PK_CLIPBOX] = "AMMO",
    [PK_SHELLS] = "SHEL", [PK_SHELLBOX] = "SBOX",
    [PK_CHAINGUN] = "MGUN", [PK_RLAUNCH] = "LAUN",
    [PK_ROCKET] = "ROCK", [PK_ROCKETBOX] = "BROK",
};

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

static void SetRegister(SpriteSet *set, int frame, int rot, Texture2D tex, bool mirror)
{
    if (frame < 0 || frame >= SPR_FRAMES) return;
    SpriteFrame *f = &set->frames[frame];
    if (rot == 0) {
        for (int r = 0; r < 8; r++) {
            f->tex[r] = tex;
            f->mirror[r] = false;
        }
    } else if (rot >= 1 && rot <= 8) {
        f->tex[rot - 1] = tex;
        f->mirror[rot - 1] = mirror;
    } else {
        return;
    }
    f->present = true;
    set->any = true;
}

static void SpriteSetLoad(SpriteSet *set, const Wad *wad, const char *name4,
                          const unsigned char *pal)
{
    memset(set, 0, sizeof(*set));
    for (int i = 0; i < wad->lumpCount; i++) {
        const char *n = wad->lumps[i].name;
        size_t len = strlen(n);
        if ((len != 6 && len != 8) || strncmp(n, name4, 4) != 0) continue;
        Texture2D tex;
        if (!DoomPatchToTexture(wad, i, pal, &tex)) continue;
        if (set->ownedCount < SPR_FRAMES*8) set->owned[set->ownedCount++] = tex;
        SetRegister(set, n[4] - 'A', n[5] - '0', tex, false);
        if (len == 8) SetRegister(set, n[6] - 'A', n[7] - '0', tex, true);
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
        SpriteSetLoad(&gs->mobs[k], wad, MOB_ANIM[k].lump, pal);
    }
    SpriteSetLoad(&gs->player, wad, PLAYER_ANIM.lump, pal);
    SpriteSetLoad(&gs->ball[0], wad, "BAL1", pal);
    SpriteSetLoad(&gs->ball[1], wad, "BAL2", pal);
    SpriteSetLoad(&gs->missile, wad, "MISL", pal);
    SpriteSetLoad(&gs->fire, wad, "FIRE", pal);
    for (int p = 0; p < PK_COUNT; p++) {
        SpriteSetLoad(&gs->pickups[p], wad, PICKUP_LUMPS[p], pal);
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
    SpriteSetUnload(&gs->missile);
    SpriteSetUnload(&gs->fire);
    for (int p = 0; p < PK_COUNT; p++) SpriteSetUnload(&gs->pickups[p]);
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

const SpriteFrame *MobSpriteFrame(const GameSprites *gs, const Mob *m, long tic)
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
        return Pick(set, an->walk[(tic/6) % n]);
    }
    }
}

const SpriteFrame *DGSpriteFrame(const GameSprites *gs, const DoomGuy *dg, long tic)
{
    const SpriteSet *set = &gs->player;
    if (!set->any) return NULL;
    if (dg->state == DG_DEAD) return PickDeath(set, PLAYER_ANIM.death, tic - dg->deathTic);
    int cdMax = (dg->weapon == WPN_SHOTGUN) ? 37 : 10;
    if (dg->state == DG_FIGHT && dg->fireCooldown > cdMax - 6) {
        return Pick(set, PLAYER_ANIM.attack[(dg->fireCooldown > cdMax - 3) ? 1 : 0]);
    }
    int n = (int)strlen(PLAYER_ANIM.walk);
    return Pick(set, PLAYER_ANIM.walk[(tic/4) % n]);
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
    const SpriteSet *set = (p->ownerKind == MK_CACO) ? &gs->ball[1] : &gs->ball[0];
    if (!set->any) return NULL;
    if (set->frames[1].present) return Pick(set, (tic/4) % 2 ? 'B' : 'A');
    return Pick(set, 'A');
}

// Explosions: MISL B-D for rockets, BAL* C-E for fireballs, ~4 tics each.
const SpriteFrame *BoomSpriteFrame(const GameSprites *gs, BoomKind kind, long age)
{
    const SpriteSet *set;
    const char *seq;
    if (kind == BOOM_ROCKET) { set = &gs->missile; seq = "BCD"; }
    else if (kind == BOOM_CACOBALL) { set = &gs->ball[1]; seq = "CDE"; }
    else { set = &gs->ball[0]; seq = "CDE"; }
    if (!set->any) return NULL;
    int i = (int)(age/4);
    if (i < 0 || i > 2) return NULL;
    return Pick(set, seq[i]);
}

const SpriteFrame *FireSpriteFrame(const GameSprites *gs, long tic)
{
    if (!gs->fire.any) return NULL;
    int n = 0;
    while (n < 8 && gs->fire.frames[n].present) n++;
    if (n == 0) return NULL;
    return Pick(&gs->fire, (char)('A' + (tic/3) % n));
}

int SpriteRotation(float entAngleDeg, float camYawDeg)
{
    // Direction from any scene point toward the (orthographic, effectively
    // infinitely far) camera, in doom coords: (sin yaw, -cos yaw).
    float yaw = camYawDeg*DEG2RAD;
    float angToCam = atan2f(-cosf(yaw), sinf(yaw))*RAD2DEG;
    float rel = entAngleDeg - angToCam + 22.5f;
    int rot = (int)floorf(rel/45.0f) % 8;
    if (rot < 0) rot += 8;
    return rot;
}

void DrawSpriteBillboard(Camera3D cam, const SpriteFrame *f, int rot,
                         Vector3 base, Color tint)
{
    Texture2D tex = f->tex[rot];
    if (tex.id == 0) {
        rot = 0;
        tex = f->tex[0];
        if (tex.id == 0) return;
    }
    float w = (float)tex.width, h = (float)tex.height;
    Rectangle src = { 0, 0, f->mirror[rot] ? -w : w, h };
    Vector3 center = { base.x, base.y + h*0.5f, base.z };
    DrawBillboardPro(cam, tex, src, center, (Vector3){ 0.0f, 1.0f, 0.0f },
                     (Vector2){ w, h }, (Vector2){ w*0.5f, h*0.5f }, 0.0f, tint);
}
