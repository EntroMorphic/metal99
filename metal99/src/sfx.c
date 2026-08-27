/* Sound-effect mixer over the I2S ring. See sfx.h. */
#include <stddef.h>
#include "sfx.h"
#include "i2s.h"
#include "es8311.h"
#include "vec.h"

#define HALF_FRAMES 480u          /* 30.7 ms per half at 15625 Hz */

/*
 * The buffer the DMA loops over. Two halves; the hardware plays one while
 * sfx_service fills the other.
 *
 * 480 frames is chosen against the FRAME RATE, not the sample rate: gridvoid
 * runs at 40 Hz, so a half lasts longer than a frame period and one refill per
 * frame is always enough. Sizing it to the sample rate instead would have made
 * the audio correct only while the video kept up.
 */
static int16_t VEC_ALIGN g_buf[HALF_FRAMES * 2u * I2S_CHANNELS];

typedef struct { const int8_t *pcm; uint32_t len, pos; } voice;
static voice g_voice[SFX_VOICES];
static uint8_t g_vol = 160u;
static int g_up;                  /* audio available at all? */

int sfx_init(void)
{
    uint32_t i;
    int rc;

    for (i = 0u; i < SFX_VOICES; i++) g_voice[i].len = 0u;
    for (i = 0u; i < (uint32_t)(sizeof g_buf / sizeof g_buf[0]); i++) g_buf[i] = 0;

    rc = es8311_init();
    if (rc != ES8311_OK) return rc;
    rc = i2s_init();
    if (rc != I2S_OK)    return rc;

    (void)es8311_volume(0x78u);   /* moderate; the mix scales below this */
    es8311_amp(1);

    rc = i2s_play_ring(g_buf, HALF_FRAMES);
    if (rc != I2S_OK) return rc;

    g_up = 1;
    return 0;
}

void sfx_volume(uint8_t v) { g_vol = v; }

void sfx_play(uint32_t clip)
{
    uint32_t i, oldest = 0u, best = 0u;

    if (!g_up || clip >= SFX_COUNT) return;

    /* A free voice if there is one, otherwise steal the one furthest through
     * its clip - the least of it left to lose. */
    for (i = 0u; i < SFX_VOICES; i++) {
        if (g_voice[i].len == 0u) { oldest = i; goto start; }
        if (g_voice[i].pos > best) { best = g_voice[i].pos; oldest = i; }
    }
start:
    g_voice[oldest].pcm = SFX[clip].pcm;
    g_voice[oldest].len = SFX[clip].len;
    g_voice[oldest].pos = 0u;
}

void sfx_service(void)
{
    int16_t *half;
    uint32_t f, v;

    if (!g_up) return;

    half = i2s_ring_claim();
    if (half == NULL) return;             /* both halves still in flight */

    for (f = 0u; f < HALF_FRAMES; f++) {
        int32_t acc = 0;
        for (v = 0u; v < SFX_VOICES; v++) {
            if (g_voice[v].len == 0u) continue;
            acc += (int32_t)g_voice[v].pcm[g_voice[v].pos];
            if (++g_voice[v].pos >= g_voice[v].len) g_voice[v].len = 0u;
        }
        /*
         * 8-bit samples scaled to 16-bit and then by volume. Clipped rather
         * than wrapped: four voices at full scale can exceed the range, and
         * wrapping turns a loud moment into a burst of noise that sounds like
         * a hardware fault rather than a mix that is too hot.
         */
        acc = (acc * (int32_t)g_vol) >> 3;
        if (acc >  32767) acc =  32767;
        if (acc < -32768) acc = -32768;
        half[f * 2u]      = (int16_t)acc;
        half[f * 2u + 1u] = (int16_t)acc;
    }
    i2s_ring_release();
}
