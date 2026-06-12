#ifndef HELLDROP_SOUND_H
#define HELLDROP_SOUND_H

#include <stdbool.h>
#include "raylib.h"
#include "wad.h"
#include "sim.h"

// Doom DMX sound effects from the WAD: header (format 3, sample rate,
// length), 16 pad bytes each side, 8-bit unsigned mono PCM. Missing lumps
// just stay silent. The sim emits SndEvents; main drains them through
// GameAudioDrain with distance attenuation from the camera target.
typedef struct GameAudio {
    Sound snd[SND_COUNT];
    bool has[SND_COUNT];
    long cursor;            // last sim sndHead consumed
    bool ready;
} GameAudio;

bool GameAudioLoad(GameAudio *ga, const Wad *wad);   // needs InitAudioDevice
void GameAudioUnload(GameAudio *ga);
// Play new sim sound events; listener is the camera target (doom coords).
void GameAudioDrain(GameAudio *ga, const Sim *sim, Vector2 listener);

#endif
