#include "dc64_sfx.h"

#include <kos.h>
#include <dc/sound/sfxmgr.h>
#include <malloc.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#define SFX_RATE 22050

static sfxhnd_t g_sfx[NSFX];

// f0 to f1 Hz over ms: a soft square (fundamental plus a third of the third harmonic) under an exponential decay
static sfxhnd_t tone(float f0, float f1, int ms, float decay) {
    int n = (SFX_RATE * ms / 1000 + 15) & ~15;  // KOS wants whole 32-byte blocks
    int16_t *pcm = memalign(32, (size_t)n * 2);
    if (!pcm) return SFXHND_INVALID;
    float ph = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / (float)n, f = f0 + (f1 - f0) * t;
        ph += 2.0f * (float)M_PI * f / SFX_RATE;
        float attack = fminf(1.0f, (float)i / (SFX_RATE * 0.002f)), env = attack * expf(-decay * t);
        pcm[i] = (int16_t)((sinf(ph) + sinf(3.0f * ph) / 3.0f) * env * 0.4f * 32767.0f);
    }
    sfxhnd_t h = snd_sfx_load_raw_buf((char *)pcm, (size_t)n * 2, SFX_RATE, 16, 1);
    free(pcm);
    return h;
}

void dc64_sfx_init(void) {
    g_sfx[SFX_MOVE] = tone(1320, 1320, 35, 6);
    g_sfx[SFX_SELECT] = tone(880, 1760, 110, 4);
    g_sfx[SFX_DENY] = tone(220, 180, 120, 3);
}

void dc64_sfx(int which) {
    static const int vol[NSFX] = { [SFX_MOVE] = 120, [SFX_SELECT] = 170, [SFX_DENY] = 160 };
    if (g_sfx[which] != SFXHND_INVALID) snd_sfx_play(g_sfx[which], vol[which], 128);
}
