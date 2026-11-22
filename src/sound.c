#include "sound.h"

#include <math.h>
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

bool GameAudioLoad(GameAudio *ga, const Wad *wad)
{
    memset(ga, 0, sizeof(*ga));
    if (!IsAudioDeviceReady()) return false;
    int loaded = 0;
    for (int i = 0; i < SND_COUNT; i++) {
        ga->has[i] = DmxToSound(wad, SND_LUMPS[i], &ga->snd[i]);
        loaded += ga->has[i] ? 1 : 0;
    }
    ga->ready = loaded > 0;
    TraceLog(LOG_INFO, "SOUND: loaded %d/%d effects from WAD", loaded, SND_COUNT);
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
    for (long e = from; e < sim->sndHead && played < MAX_PLAYS_PER_DRAIN; e++) {
        const SndEvent *ev = &sim->sndEvents[e % SND_EVENTS_MAX];
        if (ev->id < 0 || ev->id >= SND_COUNT || !ga->has[ev->id]) continue;
        float dx = ev->pos.x - listener.x, dy = ev->pos.y - listener.y;
        float dist = sqrtf(dx*dx + dy*dy);
        if (dist > AUDIBLE_RANGE) continue;
        float vol = 1.0f - dist/AUDIBLE_RANGE;
        SetSoundVolume(ga->snd[ev->id], 0.15f + 0.85f*vol);
        PlaySound(ga->snd[ev->id]);
        if (ga->capture) fprintf((FILE *)ga->capture, "%.4f %s %.3f\n", ga->captureTime,
                                 SND_LUMPS[ev->id], 0.15f + 0.85f*vol);
        played++;
    }
    ga->cursor = sim->sndHead;
}

// Trailer capture: export every effect as WAV and log each play (time,
// lump, gain) so footage dumped with --dump-frames gets its real sound.
bool GameAudioCaptureOpen(GameAudio *ga, const Wad *wad, const char *dir)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/snd", dir);
    MakeDirectory(path);
    for (int i = 0; i < SND_COUNT; i++) {
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
        snprintf(path, sizeof(path), "%s/snd/%s.wav", dir, SND_LUMPS[i]);
        ExportWave(w, path);
    }
    snprintf(path, sizeof(path), "%s/sound.txt", dir);
    ga->capture = fopen(path, "w");
    return ga->capture != NULL;
}
