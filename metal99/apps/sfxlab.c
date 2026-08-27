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
#include "es8311.h"
#include "i2s.h"
#include "vec.h"
#include "vg.h"
#include "tile.h"
#include "sh8601.h"

static int      g_up;
static uint32_t g_mode = 99u;

static void emit_sample(int32_t v) { con_dec(v); con_puts(" "); }

static void lab_init(void)
{
    int rc;
    con_puts("\r\nsfxlab: splitting the audio path at the mixer\r\n");

    i2c_init();
    rc = sfx_init();
    con_puts("  sfx_init rc="); con_dec((int32_t)rc); con_puts("\r\n");
    g_up = (rc == 0);
    if (!g_up) { con_puts("  audio unavailable - nothing below is meaningful\r\n"); return; }



    { con_puts("  first decoded samples: ");
      sfx_dbg_first_samples(emit_sample, 16u);
      con_puts("\r\n"); }

    con_puts("  now cycling: TONE through the mixer, then the two effects.\r\n");
    con_puts("  a clean tone here means the ring and DMA are fine.\r\n");
}

static int lab_frame(uint32_t f)
{
    uint32_t mode = (f / (60u * 4u)) % 2u;   /* 4 s per mode at 60 Hz */
    uint16_t col;

    if (g_up) sfx_service();

    if (mode != g_mode) {
        g_mode = mode;
        if (mode == 0u) { con_puts("  [fire]\r\n"); sfx_play(SFX_FIRE); }
        if (mode == 1u) { con_puts("  [kill]\r\n"); sfx_play(SFX_KILL); }


        con_puts("    fills="); con_dec((int32_t)sfx_fills());
        con_puts(" starved="); con_dec((int32_t)sfx_starved());
        con_puts(" decodes="); con_dec((int32_t)sfx_decodes());
        con_puts(" fail="); con_dec((int32_t)sfx_decode_fail());
        con_puts("\r\n");
    }

    /* Which mode, at a glance: one, two or three bars. */
    col = (mode == 0u) ? sh8601_rgb565(255, 200, 0)
                       : sh8601_rgb565(255, 60, 0);
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
