#include <stddef.h>
#include "io.h"
#include "vec.h"
#include "spi2.h"
#include "sh8601.h"

/* 368 px = 736 B = exactly 46 vectors. Full rows need no tail handling. */
#define ROW_VECTORS (SH8601_WIDTH * 2 / VEC_BYTES)

static uint16_t VEC_ALIGN g_row[SH8601_WIDTH];
static uint16_t VEC_ALIGN g_test[64];

static void show(const char *n, uint32_t v)
{
    con_puts("  "); con_puts(n); con_puts(" = "); con_hex32(v); con_puts("\r\n");
}
static void check(const char *n, int ok)
{
    con_puts(ok ? "  PASS  " : "  FAIL  "); con_puts(n); con_puts("\r\n");
}

/* Rows are solid colour, so a broadcast fill covers the whole row. */
static void colorbars(uint16_t *row, int y)
{
    static uint16_t bars[5];
    static int ready = 0;
    if (!ready) {
        bars[0] = sh8601_rgb565(255, 0, 0);
        bars[1] = sh8601_rgb565(0, 255, 0);
        bars[2] = sh8601_rgb565(0, 0, 255);
        bars[3] = sh8601_rgb565(255, 255, 255);
        bars[4] = sh8601_rgb565(0, 0, 0);
        ready = 1;
    }
    vec_fill16(row, bars[(y * 5) / SH8601_HEIGHT], ROW_VECTORS);
}

/* Vertical gradient. One colour computed per ROW (448 values per frame), then
 * the row itself is a single vectorised broadcast fill - no per-pixel scalar. */
__attribute__((unused)) static void gradient(uint16_t *row, int y)
{
    uint8_t v = (uint8_t)((y * 255) / (SH8601_HEIGHT - 1));
    vec_fill16(row, sh8601_rgb565(v, (uint8_t)(64u + v / 4u),
                                  (uint8_t)(255u - v)), ROW_VECTORS);
}

void app_entry(void)
{
    int rc, i, ok;

    con_puts("\r\n=== metal99 : 128-bit vector unit ===\r\n");

    /* If CPENABLE were wrong, the first EE.* would trap and we would never
     * reach the next line. Getting output at all is half the result. */
    vec_fill16(g_test, 0xBEEFu, 8u);          /* 8 vectors = 64 px */
    show("test[0]", g_test[0]);
    show("test[63]", g_test[63]);
    ok = 1;
    for (i = 0; i < 64; i++) if (g_test[i] != 0xBEEFu) ok = 0;
    check("vec_fill16 filled all 64 px", ok);

    vec_zero(g_test, 8u);
    ok = 1;
    for (i = 0; i < 64; i++) if (g_test[i] != 0u) ok = 0;
    check("vec_zero cleared all 64 px", ok);

    vec_fill16(g_row, 0x1234u, ROW_VECTORS);
    vec_copy(g_test, g_row, 8u);
    ok = 1;
    for (i = 0; i < 64; i++) if (g_test[i] != 0x1234u) ok = 0;
    check("vec_copy moved 128 B", ok);

    /* Timing: vector fill of a full row vs what scalar cost before. */
    {
        uint32_t t0 = cpu_cycles();
        for (i = 0; i < 100; i++) vec_fill16(g_row, 0xAAAAu, ROW_VECTORS);
        con_puts("  row fill (368px) = ");
        con_dec((int32_t)((cpu_cycles() - t0) / 100u));
        con_puts(" cycles/row\r\n");
    }

    /* ---- EXPERIMENT: can the vector unit store to MMIO peripheral space? ----
     * SPI_W0 is at 0x60024098: 8-byte aligned but NOT 16, so EE.VST.128 cannot
     * target it. EE.VST.L/H.64 need only 8-byte alignment, so use two of them.
     * Unknown whether the vector store unit can address peripheral space at
     * all - if it faults we never reach the readback. */
    spi2_init();
    {
        static uint32_t VEC_ALIGN pattern[4] = {
            0x11223344u, 0x55667788u, 0x99AABBCCu, 0xDDEEFF00u
        };
        volatile uint32_t *w = (volatile uint32_t *)0x60024098u;
        uint32_t got[4];

        w[0] = 0u; w[1] = 0u; w[2] = 0u; w[3] = 0u;   /* baseline */
        con_puts("  MMIO vector store: attempting...\r\n");

        __asm__ __volatile__ (
            "ee.vld.128.ip   q3, %0, 0   \n"
            : : "a"(pattern));
        __asm__ __volatile__ (
            "ee.vst.l.64.ip  q3, %0, 8   \n"
            "ee.vst.h.64.ip  q3, %0, 8   \n"
            : "+a"(w) : : "memory");

        w = (volatile uint32_t *)0x60024098u;
        got[0] = w[0]; got[1] = w[1]; got[2] = w[2]; got[3] = w[3];
        show("W0", got[0]); show("W1", got[1]);
        show("W2", got[2]); show("W3", got[3]);
        check("128b via 2x64b store reached MMIO",
              got[0] == 0x11223344u && got[1] == 0x55667788u &&
              got[2] == 0x99AABBCCu && got[3] == 0xDDEEFF00u);
    }

    rc = sh8601_init();
    con_puts("  sh8601_init rc="); con_dec((int32_t)rc); con_puts("\r\n");

    /* ---- RED TEAM: re-measure APB with the CORRECT method ----
     * The original "APB is starved, 2.1 MHz" reading was taken while the
     * FWRITE_QUAD bug was active, i.e. comparing 1-line against 1-line. It was
     * an artifact. Measure again properly: same 64 bytes on 4 lines (128 spi
     * clocks) vs 1 line (512), so the 384-clock delta isolates the bus rate. */
    {
        static uint8_t VEC_ALIGN probe[64];
        int k, n;
        /* N<<12 | H<<6 | L, with H <= N. 0x2102 (my earlier constant) decodes to
         * H=4 N=2 which is INVALID and stops the clock -> device hang. */
        static const uint32_t divs[3] = { 0x80000000u, 0x00001001u, 0x00002042u };
        static const char *dname[3] = { "/1", "/2", "/3" };
        uint32_t t0;

        vec_fill16((uint16_t *)probe, 0x5A5Au, sizeof(probe) / VEC_BYTES);

        for (k = 0; k < 6; k++) {
            int apb = (k >= 3);
            uint32_t q, sgl, d, khz;
            if (spi2_set_src_and_div(apb, divs[k % 3]) != SPI2_OK) {
                con_puts("  src="); con_puts(apb ? "APB " : "XTAL");
                con_puts(" div="); con_puts(dname[k % 3]);
                con_puts("  -> CLOCK STOPPED (sync timeout)\r\n");
                continue;
            }

            t0 = cpu_cycles();
            for (n = 0; n < 200; n++) (void)spi2_xfer(probe, 64u, 1, 0);
            q = (cpu_cycles() - t0) / 200u;
            t0 = cpu_cycles();
            for (n = 0; n < 200; n++) (void)spi2_xfer(probe, 64u, 0, 0);
            sgl = (cpu_cycles() - t0) / 200u;

            d = (sgl > q) ? (sgl - q) : 1u;
            khz = (384u * (CPU_HZ / 1000u)) / d;
            con_puts("  src="); con_puts(apb ? "APB " : "XTAL");
            con_puts(" div="); con_puts(dname[k % 3]);
            con_puts("  delta="); con_dec((int32_t)d);
            con_puts("  -> "); con_dec((int32_t)khz); con_puts(" kHz\r\n");
        }
        /* restore the known-good configuration before touching the panel */
        spi2_set_src_and_div(0, 0x80000000u);
        con_puts("  restored XTAL /1 (40 MHz)\r\n");
    }

    /* ---- PHASE 0c RETEST, now WITH A POSITIVE CONTROL ----
     * The first run concluded "scroll not implemented" from "nothing moved".
     * That is absence-of-evidence reasoning in a system with three documented
     * silent-failure traps. If the command path had been dead at that moment,
     * the observation would have been identical.
     *
     * So alternate two phases against the SAME drawn image:
     *   A: animate 0x37 scroll     -> do the bars MOVE?
     *   B: pulse 0x51 brightness   -> does the screen PULSE?   (control)
     * B pulsing proves commands are live. Only then does A not moving mean
     * "scroll is unimplemented" rather than "nothing was getting through". */
    rc = sh8601_write_frame(colorbars);
    con_puts("  bars drawn once, rc="); con_dec((int32_t)rc); con_puts("\r\n");
    rc = sh8601_scroll_def(0u, SH8601_HEIGHT, 0u);
    con_puts("  0x33 VSCRDEF rc="); con_dec((int32_t)rc); con_puts("\r\n");

    for (i = 0; ; i++) {
        int k;
        con_puts("\r\n  [A] SCROLL 6s - do the bars MOVE?\r\n");
        (void)sh8601_brightness(0xFFu);
        for (k = 0; k < 60; k++) {
            (void)sh8601_scroll_start((uint16_t)((k * 8) % SH8601_HEIGHT));
            delay_ms(100u);
        }
        con_puts("  [B] CONTROL 6s - does the screen PULSE?\r\n");
        (void)sh8601_scroll_start(0u);
        for (k = 0; k < 6; k++) {
            (void)sh8601_brightness(0x00u); delay_ms(500u);
            (void)sh8601_brightness(0xFFu); delay_ms(500u);
        }
    }
}
