/*
 * TONE - the first sound. Or the first silence, which is also information.
 *
 * Everything below this has been verified as far as it can be without making
 * noise: the codec answers on I2C with the right chip ID, and all 26 of its
 * configuration registers read back what was written. What that does NOT prove
 * is that clocks and samples reach it, or that the amplifier is powered, or
 * that the speaker is connected. This app is where those become testable, and
 * the test is your ears.
 *
 * A CONTINUOUS TONE, not a blip, and not the sound effects. If the clock
 * dividers are wrong the pitch is wrong rather than absent, which is a
 * different and much more useful failure than silence. If the DMA loop is
 * wrong you get one 16 ms click and then nothing, which is distinguishable
 * from silence if you know to listen for it. A one-shot laser sound would
 * conflate all three.
 *
 * 488.28 Hz, which is 15625/32 - an exact number of samples per period, so the
 * loop seam is silent. A tone whose period does not divide the buffer clicks
 * once per loop, and that click is easy to mistake for a DMA fault.
 *
 * APP=tone ./metal99/build.sh
 */
#include <stdint.h>
#include "app.h"
#include "io.h"
#include "i2c.h"
#include "es8311.h"
#include "i2s.h"
#include "trig.h"
#include "vec.h"
#include "vg.h"
#include "tile.h"
#include "sh8601.h"

#define PERIOD_SAMPLES 32u                 /* 15625 / 32 = 488.28 Hz */
#define PERIODS        31u        /* 992 frames = 63 ms per pass */
#define NFRAMES        (PERIOD_SAMPLES * PERIODS)
/*
 * QUIET, and briefly. This was 28000 - near full scale - and looping forever,
 * on the reasoning that an unmissable tone is the clearest possible signal
 * during bring-up. It was, and it also made the device genuinely unpleasant to
 * be near. A diagnostic that has to be unplugged is a diagnostic nobody runs
 * twice.
 */
#define AMPLITUDE      3000                /* ~9% of full scale */
#define BEEP_FRAMES    (I2S_RATE_HZ / 2u)  /* half a second, then stop */

static int16_t VEC_ALIGN g_wave[NFRAMES * 2];   /* stereo interleaved */
static int g_ready;
static int g_stopped;

static void build_wave(void)
{
    uint32_t i;
    for (i = 0u; i < NFRAMES; i++) {
        /* trig.h is binary angles: TRIG_FULL steps per turn, Q16 output.
         * 32 samples per period means the step is TRIG_FULL/32. */
        int a = (int)((i * (uint32_t)TRIG_FULL / PERIOD_SAMPLES) & (uint32_t)(TRIG_FULL - 1));
        int16_t s = (int16_t)((isin(a) * AMPLITUDE) >> 16);
        g_wave[i * 2u]      = s;           /* left  */
        g_wave[i * 2u + 1u] = s;           /* right */
    }
}

static void tone_init(void)
{
    int rc;

    con_puts("\r\ntone: 488.28 Hz continuous, 15625 Hz stereo\r\n");

    i2c_init();
    rc = es8311_init();
    con_puts("  es8311_init rc="); con_dec((int32_t)rc); con_puts("\r\n");
    if (rc != ES8311_OK) { con_puts("  codec not configured - stopping\r\n"); return; }

    rc = i2s_init();
    con_puts("  i2s_init    rc="); con_dec((int32_t)rc); con_puts("\r\n");
    if (rc != I2S_OK) { con_puts("  i2s not configured - stopping\r\n"); return; }

    build_wave();

    /*
     * Volume before amplifier, always. The codec powers up muted (DAC32 = 0),
     * so raising volume first and enabling the amp second means the speaker
     * never sees the DAC's power-up transient - which on some codecs is a
     * substantial pop.
     */
    (void)es8311_volume(0x60u);            /* well below 0 dB */
    es8311_amp(1);

    rc = i2s_play_loop(g_wave, NFRAMES);
    con_puts("  play        rc="); con_dec((int32_t)rc);
    con_puts("  frames=");         con_dec((int32_t)i2s_frames_out());
    con_puts("\r\n");
    /*
     * Can the pad measurement see a signal on these pins at all? 200 software
     * toggles should read back as ~200 edges. Anything else means the readings
     * below are not evidence of anything.
     */
    con_puts("  pad readback self-test (200 toggles each):\r\n");
    con_puts("    GPIO9 BCLK="); con_dec((int32_t)i2s_dbg_pad_selftest(9u, 200u));
    con_puts("  GPIO45 WS=");    con_dec((int32_t)i2s_dbg_pad_selftest(45u, 200u));
    con_puts("  GPIO16 MCLK=");  con_dec((int32_t)i2s_dbg_pad_selftest(16u, 200u));
    con_puts("  GPIO8 DOUT=");   con_dec((int32_t)i2s_dbg_pad_selftest(8u, 200u));
    con_puts("\r\n");

    con_puts("  amp pin reads back: "); con_dec((int32_t)es8311_amp_level());
    con_puts("  (1 = driven high = enabled)\r\n");
    con_puts("  wave[0..3] = ");
    { int k; for (k = 0; k < 4; k++) { con_dec((int32_t)g_wave[k]); con_puts(" "); } }
    con_puts(" peak="); 
    { int k, pk = 0; for (k = 0; k < (int)(NFRAMES*2); k++) {
        int v = g_wave[k] < 0 ? -g_wave[k] : g_wave[k]; if (v > pk) pk = v; }
      con_dec((int32_t)pk); }
    con_puts("\r\n");
    con_puts("  amp ON, volume 0xBF. You should hear a steady tone.\r\n");
    con_puts("  wrong PITCH means the dividers are off. ONE CLICK means the\r\n");
    con_puts("  DMA loop stopped. SILENCE means clocks, amp or speaker.\r\n");
    g_ready = 1;
}

/*
 * A MOVING BAR, deliberately.
 *
 * This app used to draw nothing at all, on the reasoning that a quiet display
 * is one less thing between a symptom and a diagnosis. That was wrong in a way
 * worth recording: the panel retains its framebuffer, so "draws nothing" means
 * the boot test patterns stay on the glass indefinitely - and a build that
 * never changes the screen is indistinguishable from a build that never
 * flashed. It cost a round of "did you reflash it?", which is exactly the kind
 * of ambiguity this project keeps paying for.
 *
 * The bar sweeps in step with the audio buffer, so it also says the frame loop
 * is running and re-arming, not wedged.
 */
static int tone_frame(uint32_t f)
{
    int y = (int)((f * 3u) % (uint32_t)(SH8601_HEIGHT - 40));

    vg_set_bg(0x0000u);
    vg_begin();
    /* Green while the audio path reports healthy, red if init failed. */
    { uint16_t c = g_ready ? sh8601_rgb565(0, 255, 90)
                           : sh8601_rgb565(255, 40, 0);
      int k;
      for (k = 0; k < 40; k++) vg_line(0, y + k, SH8601_WIDTH - 1, y + k, c); }
    vg_finish();
    (void)tile_present(vg_rowfn);

    if (!g_ready) return 0;

    /* Beep, then stop and stay stopped. Long enough to hear, short enough to
     * live with. Re-flash to hear it again. */
    if (f * (I2S_RATE_HZ / 30u) > BEEP_FRAMES) {
        if (!g_stopped) { i2s_stop(); es8311_amp(0); g_stopped = 1;
                          con_puts("  beep done, amp off\r\n"); }
        return 0;
    }
    (void)i2s_service();          /* re-arm before the buffer drains */

    /*
     * Report from the RUNNING loop, once a second, not from init.
     *
     * Measuring in init samples the instant after arming, before DMA has
     * delivered anything - WS and DOUT read zero and mean nothing. Steady
     * state is the only state worth measuring, and it took reading a
     * misleading zero to notice that.
     */
    if ((f % 60u) == 30u) {
        con_puts("  MCLK="); con_dec((int32_t)i2s_dbg_pad_edges(16u, 2000u));
        con_puts(" BCLK=");  con_dec((int32_t)i2s_dbg_pad_edges(9u, 2000u));
        con_puts(" WS=");    con_dec((int32_t)i2s_dbg_pad_edges(45u, 2000u));
        con_puts(" DOUT=");  con_dec((int32_t)i2s_dbg_pad_edges(8u, 2000u));
        con_puts(" rearms="); con_dec((int32_t)i2s_rearms());
        con_puts(" TXC=0x");  con_hex32(i2s_dbg_tx_conf());
        con_puts(" dw0=0x");  con_hex32(i2s_dbg_desc_dw0());
        con_puts(" link=0x"); con_hex32(i2s_dbg_out_link());
        con_puts("\r\n");
    }
    return 0;
}

const app_t APP = { "tone", 30u, tone_init, tone_frame, 0 };
