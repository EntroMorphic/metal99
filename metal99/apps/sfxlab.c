/*
 * SFXLAB - is the corruption before the mixer, or after it?
 *
 * Four rounds of "change something, have a listen" is three too many, and the
 * source PCM is now verified bit-accurate against a fresh decode - 0.01%
 * error, pure integer rounding. So the samples are not the problem and further
 * format changes would be guesswork.
 *
 * This splits the path at the mixer and tests each side with something whose
 * correctness is not a matter of opinion:
 *
 *   BEFORE  sfx_selftest() recomputes the mixer's arithmetic independently and
 *           compares. Needs no ears and no speaker.
 *
 *   AFTER   a pure 440-ish tone is pushed through sfx_play_pcm - the same
 *           voices, ring, DMA and codec an effect uses. The tone app already
 *           proved the codec with its own simpler single-descriptor loop; if a
 *           tone is clean HERE and effects are not, the fault is the effects'
 *           data or its rate. If the tone is ALSO rough, the fault is the ring
 *           or the mixer, and the effects were never the problem.
 *
 * Modes cycle every 4 seconds so one flash answers both, and the screen says
 * which is playing - the panel retains its framebuffer, so a build that draws
 * nothing is indistinguishable from one that never flashed.
 *
 * APP=sfxlab ./metal99/build.sh
 */
#include <stdint.h>
#include "app.h"
#include "io.h"
#include "i2c.h"
#include "sfx.h"
#include "i2s.h"
#include "trig.h"
#include "vec.h"
#include "vg.h"
#include "tile.h"
#include "sh8601.h"

#define TONE_PERIOD 32u                    /* 15625/32 = 488 Hz, exact */
/*
 * A FULL SECOND, not 49 ms.
 *
 * The first version was 768 samples - short enough to sit in the ring and
 * barely refill. That made it a useless control: it certified a path that an
 * effect exercises 36 times over, having exercised it once. A reference has to
 * be at least as demanding as the thing it vindicates.
 *
 * 0.31 s spans nine buffer halves, which is demanding enough, and costs 19 KB.
 * A full second would have been 62 KB and did not fit - the linker said so,
 * which is the right way to find out.
 */
#define TONE_LEN    (TONE_PERIOD * 300u)   /* ~0.31 s = 19 KB, spans 9 halves */

static int16_t VEC_ALIGN g_tone[TONE_LEN];
static int g_up;
static uint32_t g_mode = 99u;

static void build_tone(void)
{
    uint32_t i;
    for (i = 0u; i < TONE_LEN; i++) {
        int a = (int)((i * (uint32_t)TRIG_FULL / TONE_PERIOD)
                      & (uint32_t)(TRIG_FULL - 1));
        /* Same peak as the loudest baked effect, so the comparison is at
         * matched level rather than matched intent. */
        g_tone[i] = (int16_t)((isin(a) * 30000) >> 16);
    }
}

static void lab_init(void)
{
    int rc;
    con_puts("\r\nsfxlab: splitting the audio path at the mixer\r\n");

    i2c_init();
    rc = sfx_init();
    con_puts("  sfx_init rc="); con_dec((int32_t)rc); con_puts("\r\n");
    g_up = (rc == 0);
    if (!g_up) { con_puts("  audio unavailable - nothing below is meaningful\r\n"); return; }

    build_tone();

    /* BEFORE the mixer: arithmetic, no ears required. */
    { uint32_t bad = sfx_selftest();
      con_puts("  mixer self-test: "); con_dec((int32_t)bad);
      con_puts(bad ? " mismatched samples  <-- MIXER IS WRONG\r\n"
                   : " mismatches - mixer arithmetic is correct\r\n"); }

    con_puts("  now cycling: TONE through the mixer, then the two effects.\r\n");
    con_puts("  a clean tone here means the ring and DMA are fine.\r\n");
}

static int lab_frame(uint32_t f)
{
    uint32_t mode = (f / (60u * 4u)) % 4u;   /* 4 s per mode at 60 Hz */
    uint16_t col;

    if (g_up) sfx_service();

    if (mode != g_mode) {
        g_mode = mode;
        if (mode == 0u) { con_puts("  [tone via mixer]\r\n");  sfx_play_pcm(g_tone, TONE_LEN); }
        if (mode == 1u) { con_puts("  [effect: fire]\r\n");     sfx_play(SFX_FIRE); }
        if (mode == 2u) { con_puts("  [effect: kill]\r\n");     sfx_play(SFX_KILL); }
        /*
         * MODE 3 IS THE ONE THAT MATTERS. A pure sine that has been through
         * decode, resample, high-pass, normalise and the generated C array -
         * everything an effect goes through, and everything the runtime tone
         * skips. Clean here exonerates the whole build pipeline.
         */
        if (mode == 3u) { con_puts("  [BAKED SINE through the full pipeline]\r\n");
                          sfx_play(SFX_PROBE); }
        con_puts("    fills="); con_dec((int32_t)sfx_fills());
        con_puts(" starved="); con_dec((int32_t)sfx_starved());
        con_puts("\r\n");
    }

    /* Which mode, at a glance: one, two or three bars. */
    col = (mode == 0u) ? sh8601_rgb565(0, 255, 90)
        : (mode == 1u) ? sh8601_rgb565(255, 200, 0)
        : (mode == 2u) ? sh8601_rgb565(255, 60, 0)
                       : sh8601_rgb565(120, 160, 255);
    vg_set_bg(0x0000u);
    vg_begin();
    { uint32_t b, k;
      for (b = 0u; b <= mode; b++)
        for (k = 0u; k < 30u; k++)
            vg_line(20, (int)(60u + b * 60u + k), 340,
                    (int)(60u + b * 60u + k), col); }
    vg_finish();
    return tile_present(vg_rowfn);
}

const app_t APP = { "sfxlab", 60u, lab_init, lab_frame, 0 };
