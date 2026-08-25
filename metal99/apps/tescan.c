/*
 * TE PIN SCAN - is the SH8601's tearing-effect output wired to anything?
 *
 * Waveshare's own BSP (esp32_s3_touch_amoled_1_8.h) defines CS, PCLK, DATA0-3
 * and the touch INT, and no TE pin. Their Arduino pin_config.h agrees. That is
 * two documents saying the line is not broken out - but neither is a
 * measurement, and the panel IS pulsing TE: sh8601_init sends 0x35 with
 * parameter 0x00, so the output is enabled at the panel end regardless of
 * where it goes.
 *
 * So: enable every free pad as an input and look for something that toggles at
 * the panel's refresh rate. TE in mode 0 pulses once per frame, ~60 Hz, so a
 * 200 ms window should hold about 12 pulses - 24 edges.
 *
 * DISTINGUISHING DRIVEN FROM FLOATING is the whole trick. A floating pad picks
 * up noise and can show edges that look like a signal. A pad is only driven if
 * it reads the SAME under an internal pull-up as under a pull-down; a floating
 * one follows whichever pull is applied. Every candidate is therefore measured
 * twice, and a pin is only reported as a real signal if it disagrees with both
 * pulls.
 *
 * This is a probe, not a feature. It exists to answer one question on hardware
 * that no datasheet in the repo answers. APP=tescan ./metal99/build.sh
 */
#include <stdint.h>
#include "app.h"
#include "io.h"

#define GPIO_BASE     0x60004000u
#define GPIO_IN0      REG32(GPIO_BASE + 0x3Cu)   /* pads 0..31  */
#define GPIO_IN1      REG32(GPIO_BASE + 0x40u)   /* pads 32..48 */
#define IO_MUX_GPIO(n) REG32(0x60009004u + 4u * (uint32_t)(n))

#define IOMUX_MCU_SEL_S  12
#define IOMUX_FUN_IE     (1u << 9)
#define IOMUX_FUN_WPU    (1u << 8)
#define IOMUX_FUN_WPD    (1u << 7)
#define IOMUX_GPIO_FUNC  (1u << IOMUX_MCU_SEL_S)  /* function 1 = GPIO matrix */

#define GPIO_OUT_W1TS     REG32(GPIO_BASE + 0x08u)
#define GPIO_OUT_W1TC     REG32(GPIO_BASE + 0x0Cu)
#define GPIO_ENABLE_W1TS  REG32(GPIO_BASE + 0x24u)
#define GPIO_ENABLE_W1TC  REG32(GPIO_BASE + 0x28u)
#define GPIO_FUNC_OUT(n)  REG32(GPIO_BASE + 0x554u + 4u * (uint32_t)(n))
#define SIG_GPIO_OUT      256u

/*
 * Pads NOT scanned, and why - guessing wrong here costs a reflash or a brick.
 *   4,5,6,7,11,12  QSPI to the panel      1,2,3    SD card
 *   14,15          I2C to the touch       21       touch INT
 *   8,9,10,16,45   I2S to the ES8311      46       audio PA enable (strapping)
 *   19,20          USB D-/D+ - THE CONSOLE. Touching these kills our own output.
 *   26..32         SPI flash and PSRAM. Reconfiguring these hangs the chip.
 */
static const uint8_t CAND[] = {
    0, 13, 17, 18, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 47, 48
};
#define NCAND ((int)(sizeof CAND / sizeof CAND[0]))

static uint32_t pad_read(uint32_t g)
{
    return (g < 32u) ? ((GPIO_IN0 >> g) & 1u)
                     : ((GPIO_IN1 >> (g - 32u)) & 1u);
}

/*
 * Count edges over `ms`, with the given internal pull. Returns edges; *held
 * receives the level if the pad never moved.
 *
 * The window is closed on the CYCLE COUNTER, not on a loop trip count. The
 * first version of this spun `ms * 200` times on the assumption of 200 samples
 * per millisecond, and then divided by that same assumption to report a
 * frequency - so the Hz it printed was a restatement of the guess, and would
 * have been wrong by whatever factor the guess was wrong by, with no symptom.
 * Timing the window means the reported rate is measured.
 */
static uint32_t edges(uint32_t g, uint32_t pull, uint32_t ms, uint32_t *held,
                      uint32_t *samples)
{
    uint32_t e = 0, last, cur, n = 0;
    uint32_t t0, span = (CPU_HZ / 1000u) * ms;

    IO_MUX_GPIO(g) = IOMUX_GPIO_FUNC | IOMUX_FUN_IE | pull;
    delay_ms(2);                       /* let the pull settle */
    last = pad_read(g);
    t0 = cpu_cycles();
    while (cpu_cycles() - t0 < span) {
        cur = pad_read(g);
        if (cur != last) { e++; last = cur; }
        n++;
    }
    *held = last;
    *samples = n;
    return e;
}

/*
 * PROVE THE SCANNER CAN SEE A SIGNAL before believing it saw none.
 *
 * "Every pad is floating" and "the sampling loop is broken" produce identical
 * output, and this repo has shipped a self-test that could not fail before
 * (selftest.h). So: drive a known-floating pad as an output, toggle it at a
 * known rate, and require the scanner to report that rate back. If this does
 * not pass, nothing below it means anything.
 */
#define SELFTEST_PAD  17u
#define SELFTEST_HZ   60u

static int scanner_selftest(void)
{
    uint32_t e = 0, last, cur, n = 0, half, t0, tnext, span, held, lvl = 0;

    /* Output driver on, input buffer on so we read back what we drive. */
    IO_MUX_GPIO(SELFTEST_PAD) = IOMUX_GPIO_FUNC | IOMUX_FUN_IE;
    GPIO_ENABLE_W1TS = (1u << SELFTEST_PAD);
    GPIO_FUNC_OUT(SELFTEST_PAD) = SIG_GPIO_OUT;

    half = CPU_HZ / (SELFTEST_HZ * 2u);      /* half period in cycles */
    span = CPU_HZ / 5u;                      /* 200 ms */
    t0 = cpu_cycles(); tnext = t0 + half;
    last = pad_read(SELFTEST_PAD);
    while (cpu_cycles() - t0 < span) {
        if (cpu_cycles() - tnext < 0x80000000u) {   /* wrap-safe compare */
            lvl ^= 1u;
            if (lvl) GPIO_OUT_W1TS = (1u << SELFTEST_PAD);
            else     GPIO_OUT_W1TC = (1u << SELFTEST_PAD);
            tnext += half;
        }
        cur = pad_read(SELFTEST_PAD);
        if (cur != last) { e++; last = cur; }
        n++;
    }
    /* Hand the pad back: input only, no driver, no pull. */
    GPIO_ENABLE_W1TC = (1u << SELFTEST_PAD);
    IO_MUX_GPIO(SELFTEST_PAD) = IOMUX_GPIO_FUNC;
    held = e / 2u * 5u;                      /* edges/2 over 0.2 s -> Hz */

    con_puts("scanner self-test: drove pad "); con_dec((int32_t)SELFTEST_PAD);
    con_puts(" at "); con_dec((int32_t)SELFTEST_HZ);
    con_puts(" Hz, measured "); con_dec((int32_t)held);
    con_puts(" Hz over "); con_dec((int32_t)n); con_puts(" samples -> ");
    if (held >= SELFTEST_HZ - 6u && held <= SELFTEST_HZ + 6u) {
        con_puts("PASS\r\n"); return 1;
    }
    con_puts("FAIL - scan results below are meaningless\r\n");
    return 0;
}

static void scan(void)
{
    int i;
    con_puts("\r\nTE scan: looking for a ~60Hz pulse on free pads\r\n");
    if (!scanner_selftest()) return;
    con_puts("pad  pulldown(edges/level)  pullup(edges/level)  verdict\r\n");

    for (i = 0; i < NCAND; i++) {
        uint32_t g = CAND[i], hd = 0, hu = 0, ed, eu, sd = 0, su = 0;

        ed = edges(g, IOMUX_FUN_WPD, 200u, &hd, &sd);
        eu = edges(g, IOMUX_FUN_WPU, 200u, &hu, &su);

        con_puts("  "); con_dec((int32_t)g);
        con_puts("\t"); con_dec((int32_t)ed); con_puts("/"); con_dec((int32_t)hd);
        con_puts("\t\t"); con_dec((int32_t)eu); con_puts("/"); con_dec((int32_t)hu);
        con_puts("\t\t");

        /* Floating: follows the pull, quiet under both. That is the expected
         * answer for every pad if TE is genuinely not routed. */
        if (ed < 4u && eu < 4u && hd == 0u && hu == 1u) {
            con_puts("floating");
        } else if (ed < 4u && eu < 4u && hd == hu) {
            con_puts("DRIVEN, static");        /* tied high or low somewhere */
        } else if (ed >= 4u && eu >= 4u) {
            /* Toggling regardless of the pull - a real output. Is it TE? TE in
             * mode 0 gives one pulse per refresh; report the rate so the
             * number can be checked against ~60 Hz rather than trusted. */
            uint32_t hz = (ed < eu ? ed : eu) / 2u * 5u;   /* edges/2 over 0.2s */
            con_puts("SIGNAL ~"); con_dec((int32_t)hz); con_puts(" Hz");
            if (hz >= 40u && hz <= 90u) con_puts("  <-- TE CANDIDATE");
        } else {
            con_puts("noisy/floating");
        }
        con_puts("\r\n");

        /* Leave the pad as we found it: input, no pull, no output driver. */
        IO_MUX_GPIO(g) = IOMUX_GPIO_FUNC;
    }
    con_puts("scan complete\r\n");
}

static void te_init(void) { scan(); }
static int  te_frame(uint32_t f) { (void)f; return 0; }

/* 1 Hz: the scan runs once in init; the frame loop exists only to keep main.c
 * happy and the watchdog fed. */
const app_t APP = { "tescan", 1u, te_init, te_frame, 0 };
