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
static void gradient(uint16_t *row, int y)
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

    for (i = 0; ; i++) {
        uint32_t t0 = cpu_cycles();
        rc = sh8601_write_frame(((i & 1) == 0) ? colorbars : gradient);
        con_puts(((i & 1) == 0) ? "  BARS     " : "  GRADIENT ");
        con_puts("rc="); con_dec((int32_t)rc);
        con_puts("  frame="); con_dec((int32_t)((cpu_cycles() - t0) / (CPU_HZ / 1000u)));
        con_puts(" ms\r\n");
        delay_ms(2000u);
    }
}
