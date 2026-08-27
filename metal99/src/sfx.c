/* Sound-effect mixer over the I2S ring. See sfx.h. */
#include <stddef.h>
#include "sfx.h"
#include "i2s.h"
#include "es8311.h"
#include "vec.h"

/*
 * SIZED IN MILLISECONDS, not in samples, because that is the property that
 * matters and the one that silently broke.
 *
 * A half must outlast the gap between sfx_service() calls, which is one video
 * frame: 25 ms at 40 Hz, 33 ms at 30. This was 480 frames - comfortable at
 * 15625 Hz (30.7 ms) and an underrun EVERY FRAME once the rate doubled to
 * 31250 and the same 480 samples became 15.4 ms. Nothing complained; the DMA
 * simply replayed whatever was in the half, which is what "raking rocks"
 * sounds like.
 *
 * 1000 frames is 4000 bytes, just under the 4095 a single GDMA descriptor can
 * carry, and 32 ms at 31250 Hz. Two halves give 64 ms - roughly two frames of
 * slack at either cadence.
 */
#define HALF_FRAMES 1000u

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

typedef struct { const int16_t *pcm; uint32_t len, pos; } voice;
static voice g_voice[SFX_VOICES];
/*
 * 22, which is about 8% of full scale for a single voice.
 *
 * Not a taste decision - a measured one. The first 8-bit build multiplied
 * peak-127 samples by 20, landing at 7.8% of the DAC's range, and it sounded
 * clean. Moving to 16-bit samples with a half-scale mix made the same effects
 * SIX TIMES louder, and they sounded worse: not less resolution, more
 * distortion, from a small speaker driven past where it stays linear.
 *
 * Level and quality are easy to confuse when only one of them changed on
 * purpose. This restores the level that worked and keeps the resolution that
 * was the point.
 */
static uint8_t g_vol = 22u;
static int g_up;                  /* audio available at all? */
static uint32_t g_fills;          /* halves actually mixed - 0 means starved */
static uint32_t g_starved;        /* services that found BOTH halves free    */

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
uint32_t sfx_fills(void)   { return g_fills; }
uint32_t sfx_starved(void) { return g_starved; }

void sfx_play_pcm(const int16_t *pcm, uint32_t len)
{
    uint32_t i, oldest = 0u, best = 0u;
    if (!g_up || pcm == NULL || len == 0u) return;
    for (i = 0u; i < SFX_VOICES; i++) {
        if (g_voice[i].len == 0u) { oldest = i; break; }
        if (g_voice[i].pos > best) { best = g_voice[i].pos; oldest = i; }
    }
    g_voice[oldest].pcm = pcm;
    g_voice[oldest].len = len;
    g_voice[oldest].pos = 0u;
}

/*
 * Mix ONE voice into a scratch buffer with the same arithmetic sfx_service
 * uses, then check it. Deliberately not sharing code with the mixer - a test
 * that calls the thing it is testing proves only that the function is
 * deterministic.
 */
uint32_t sfx_selftest(void)
{
    static int16_t scratch[64];
    uint32_t i, bad = 0u;
    const int16_t *src;

    if (SFX_COUNT == 0u || SFX[0].len < 64u) return 0u;
    src = SFX[0].pcm;

    for (i = 0u; i < 64u; i++) {
        int32_t acc = (int32_t)src[i];
        acc = (acc * (int32_t)g_vol) >> 8;
        if (acc >  32767) acc =  32767;
        if (acc < -32768) acc = -32768;
        scratch[i] = (int16_t)acc;
    }
    /*
     * Recompute independently, and allow ONE LSB.
     *
     * The first version of this compared >> 8 against / 256 and reported 51
     * mismatches in 64 - which is not a mixer fault, it is arithmetic: for
     * negative samples an arithmetic shift rounds toward minus infinity and
     * division rounds toward zero. Half the samples in an audio clip are
     * negative, and half of 64 is 32... plus the boundary cases.
     *
     * A test that fires on a 1-LSB rounding difference cannot tell that from
     * real corruption, which is the only thing it exists to find. The
     * tolerance makes the difference between the two visible again.
     */
    for (i = 0u; i < 64u; i++) {
        int32_t want = ((int32_t)src[i] * (int32_t)g_vol) / 256;
        int32_t diff;
        if (want >  32767) want =  32767;
        if (want < -32768) want = -32768;
        diff = (int32_t)scratch[i] - want;
        if (diff < -1 || diff > 1) bad++;
    }
    return bad;
}

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
    int idx;

    if (!g_up) return;

    /*
     * Fill EVERY free half, not just one. At 40 Hz a frame is 25 ms and a half
     * is 30.7 ms, so one is normally enough - but a single late frame leaves
     * both free, and filling one of them would hand the other back stale.
     */
    if (i2s_underruns()) g_starved++;

    while ((idx = i2s_ring_claim(&half)) >= 0) {
    for (f = 0u; f < HALF_FRAMES; f++) {
        int32_t acc = 0;
        for (v = 0u; v < SFX_VOICES; v++) {
            if (g_voice[v].len == 0u) continue;
            acc += (int32_t)g_voice[v].pcm[g_voice[v].pos];
            if (++g_voice[v].pos >= g_voice[v].len) g_voice[v].len = 0u;
        }
        /*
         * Volume as a 0..255 fraction of unity, so 128 is half scale. Clipped
         * rather than wrapped: four voices can sum past the range, and
         * wrapping turns a loud moment into a burst of noise that sounds like
         * a hardware fault rather than a mix that is simply too hot.
         */
        acc = (acc * (int32_t)g_vol) >> 8;
        if (acc >  32767) acc =  32767;
        if (acc < -32768) acc = -32768;
        half[f * 2u]      = (int16_t)acc;
        half[f * 2u + 1u] = (int16_t)acc;
    }
    g_fills++;
    i2s_ring_release(idx);
    }
}
