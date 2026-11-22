#include "sound.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define AUDIBLE_RANGE 2200.0f
#define MAX_PLAYS_PER_DRAIN 8

static const char *SND_LUMPS[SND_COUNT] = {
    [SND_PISTOL] = "DSPISTOL", [SND_SHOTGUN] = "DSSHOTGN",
    [SND_FIRSHT] = "DSFIRSHT", [SND_FIRXPL] = "DSFIRXPL",
    [SND_CLAW] = "DSCLAW",     [SND_SGTATK] = "DSSGTATK",
    [SND_POSIT] = "DSPOSIT1",  [SND_BGSIT] = "DSBGSIT1",
    [SND_SGTSIT] = "DSSGTSIT", [SND_CACSIT] = "DSCACSIT",
    [SND_VILSIT] = "DSVILSIT",
    [SND_SKLATK] = "DSSKLATK", [SND_SKESIT] = "DSSKESIT",
    [SND_SKEATK] = "DSSKEATK", [SND_SKEDTH] = "DSSKEDTH",
    [SND_BRSSIT] = "DSBRSSIT", [SND_BRSDTH] = "DSBRSDTH",
    [SND_POPAIN] = "DSPOPAIN", [SND_DMPAIN] = "DSDMPAIN",
    [SND_PLPAIN] = "DSPLPAIN", [SND_VIPAIN] = "DSVIPAIN",
    [SND_PODTH] = "DSPODTH1",  [SND_BGDTH] = "DSBGDTH1",
    [SND_SGTDTH] = "DSSGTDTH", [SND_CACDTH] = "DSCACDTH",
    [SND_VILDTH] = "DSVILDTH", [SND_PLDETH] = "DSPLDETH",
    [SND_DOROPN] = "DSDOROPN", [SND_DORCLS] = "DSDORCLS",
    [SND_ITEMUP] = "DSITEMUP", [SND_WPNUP] = "DSWPNUP",
    [SND_TELEPT] = "DSTELEPT",
    [SND_VILATK] = "DSVILATK", [SND_FLAMST] = "DSFLAMST",
    [SND_SLOP] = "DSSLOP",
    [SND_RLAUNC] = "DSRLAUNC", [SND_BAREXP] = "DSBAREXP",
    [SND_PSTART] = "DSPSTART", [SND_PSTOP] = "DSPSTOP",
    [SND_SUMMON] = "HDSUMMON", [SND_BARREL_BLAST] = "HDBLAST",
    [SND_BARREL_READY] = "HDREADY",
};

static bool DmxToSound(const Wad *wad, const char *name, Sound *out)
{
    int idx = WadFindLump(wad, name, 0);
    if (idx < 0) return false;
    int size = 0;
    const unsigned char *d = WadLumpData(wad, idx, &size);
    if (!d || size < 8 + 32) return false;
    int format = d[0] | (d[1] << 8);
    int rate = d[2] | (d[3] << 8);
    unsigned len = (unsigned)d[4] | ((unsigned)d[5] << 8) |
                   ((unsigned)d[6] << 16) | ((unsigned)d[7] << 24);
    if (format != 3 || rate <= 0) return false;
    // len counts the payload incl. 16 pad bytes each side in most WADs;
    // clamp to what's actually in the lump and skip the leading pad.
    unsigned avail = (unsigned)size - 8;
    if (len > avail) len = avail;
    if (len <= 32) return false;
    unsigned samples = len - 32;
    Wave w = {
        .frameCount = samples,
        .sampleRate = (unsigned)rate,
        .sampleSize = 8,
        .channels = 1,
        .data = (void *)(d + 8 + 16),   // LoadSoundFromWave copies
    };
    *out = LoadSoundFromWave(w);
    return out->frameCount > 0;
}

// Original PCM effects. A fixed noise seed keeps the game and capture mix in sync.
static Wave MakeEffect(int id, int16_t *samples)
{
    const unsigned rate = 24000;
    unsigned count = id == SND_BARREL_BLAST ? 12000 : id == SND_SUMMON ? 7200 : 3840;
    uint32_t noise = 0x9e3779b9u;
    float rumble = 0.0f;
    for (unsigned i = 0; i < count; i++) {
        float t = (float)i/rate;
        noise = noise*1664525u + 1013904223u;
        float grain = (float)(noise >> 16)/32768.0f - 1.0f;
        rumble += (grain - rumble)*0.08f;
        float value;
        switch (id) {
        case SND_BARREL_BLAST:
            value = 0.80f*(grain*0.58f*expf(-t*55.0f)
                          + sinf(6.2831853f*(74.0f - 46.0f*t)*t)*0.50f*expf(-t*10.0f)
                          + rumble*0.37f*expf(-t*7.0f));
            break;
        case SND_SUMMON:
            value = sinf(6.2831853f*(135.0f*t + 490.0f*t*t))*0.42f*expf(-t*10.0f)
                    + rumble*0.65f*expf(-t*5.0f);
            break;
        default:
            value = (sinf(6.2831853f*770.0f*t) + sinf(6.2831853f*1155.0f*t))*0.22f*expf(-t*15.0f);
            break;
        }
        float fade = fminf(fminf(t*800.0f, (float)(count - 1 - i)/240.0f), 1.0f);
        samples[i] = (int16_t)(fmaxf(-1.0f, fminf(1.0f, value*fade))*32767.0f);
    }
    return (Wave){ .frameCount = count, .sampleRate = rate, .sampleSize = 16,
                   .channels = 1, .data = samples };
}

bool GameAudioLoad(GameAudio *ga, const Wad *wad)
{
    memset(ga, 0, sizeof(*ga));
    if (!IsAudioDeviceReady()) return false;
    int loaded = 0;
    int16_t samples[12000];
    for (int i = 0; i < SND_COUNT; i++) {
        if (i >= SND_SUMMON) {
            ga->snd[i] = LoadSoundFromWave(MakeEffect(i, samples));
            ga->has[i] = ga->snd[i].frameCount > 0;
        } else {
            ga->has[i] = DmxToSound(wad, SND_LUMPS[i], &ga->snd[i]);
        }
        loaded += ga->has[i] ? 1 : 0;
    }
    ga->ready = loaded > 0;
    TraceLog(LOG_INFO, "SOUND: loaded %d/%d effects", loaded, SND_COUNT);
    return ga->ready;
}

void GameAudioUnload(GameAudio *ga)
{
    if (!ga) return;
    for (int i = 0; i < SND_COUNT; i++) {
        if (ga->has[i]) UnloadSound(ga->snd[i]);
    }
    memset(ga, 0, sizeof(*ga));
}

void GameAudioDrain(GameAudio *ga, const Sim *sim, Vector2 listener)
{
    if (!ga->ready) {
        ga->cursor = sim->sndHead;
        return;
    }
    long from = ga->cursor;
    // After a turbo burst (or restart) skip stale events beyond the ring.
    if (sim->sndHead - from > SND_EVENTS_MAX) from = sim->sndHead - SND_EVENTS_MAX;
    if (from > sim->sndHead) from = sim->sndHead;   // sim restarted
    int played = 0;
    // Reserve the first voices for player feedback when combat is busiest.
    for (int pass = 0; pass < 2 && played < MAX_PLAYS_PER_DRAIN; pass++) {
        for (long e = from; e < sim->sndHead && played < MAX_PLAYS_PER_DRAIN; e++) {
            const SndEvent *ev = &sim->sndEvents[e % SND_EVENTS_MAX];
            if (ev->id < 0 || ev->id >= SND_COUNT || !ga->has[ev->id]) continue;
            bool priority = ev->id >= SND_SUMMON;
            if (priority != (pass == 0)) continue;
            float dx = ev->pos.x - listener.x, dy = ev->pos.y - listener.y;
            float dist = sqrtf(dx*dx + dy*dy);
            if (dist > AUDIBLE_RANGE && ev->id != SND_SUMMON) continue;
            float gain = fminf(1.0f, fmaxf(0.0f, 1.0f - dist/AUDIBLE_RANGE));
            float volume = ev->id == SND_SUMMON ? 0.80f : 0.15f + 0.85f*gain;
            SetSoundVolume(ga->snd[ev->id], volume);
            PlaySound(ga->snd[ev->id]);
            if (ga->capture) fprintf((FILE *)ga->capture, "%.4f %s %.3f\n", ga->captureTime,
                                     SND_LUMPS[ev->id], volume);
            played++;
        }
    }
    ga->cursor = sim->sndHead;
}

void GameAudioBarrelReady(GameAudio *ga)
{
    if (!ga->ready || !ga->has[SND_BARREL_READY]) return;
    SetSoundVolume(ga->snd[SND_BARREL_READY], 0.72f);
    PlaySound(ga->snd[SND_BARREL_READY]);
}

// Trailer capture: export every effect as WAV and log each play (time,
// lump, gain) so footage dumped with --dump-frames gets its real sound.
bool GameAudioCaptureOpen(GameAudio *ga, const Wad *wad, const char *dir)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/snd", dir);
    MakeDirectory(path);
    int16_t samples[12000];
    for (int i = 0; i < SND_COUNT; i++) {
        snprintf(path, sizeof(path), "%s/snd/%s.wav", dir, SND_LUMPS[i]);
        if (i >= SND_SUMMON) {
            ExportWave(MakeEffect(i, samples), path);
            continue;
        }
        int idx = WadFindLump(wad, SND_LUMPS[i], 0);
        if (idx < 0) continue;
        int size = 0;
        const unsigned char *d = WadLumpData(wad, idx, &size);
        if (!d || size < 40) continue;
        int rate = d[2] | (d[3] << 8);
        unsigned len = (unsigned)d[4] | ((unsigned)d[5] << 8) | ((unsigned)d[6] << 16) | ((unsigned)d[7] << 24);
        if (len > (unsigned)size - 8) len = (unsigned)size - 8;
        if (len <= 32) continue;
        Wave w = { .frameCount = len - 32, .sampleRate = (unsigned)rate, .sampleSize = 8,
                   .channels = 1, .data = (void *)(d + 8 + 16) };
        ExportWave(w, path);
    }
    snprintf(path, sizeof(path), "%s/sound.txt", dir);
    ga->capture = fopen(path, "w");
    return ga->capture != NULL;
}
