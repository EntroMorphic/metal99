#include <stddef.h>
#include "io.h"
#include "vec.h"
#include "spi2.h"
#include "sh8601.h"
#include "gdma.h"
#include "clk.h"

/* 368 px = 736 B = exactly 46 vectors. Full rows need no scalar tail. */
#define ROW_VECTORS (SH8601_WIDTH * 2 / VEC_BYTES)

/* Theoretical wire time for one frame at 40 MHz over 4 lines:
 *   368*448*16 bits / (4 * 40e6) = 16.49 ms  -> in CPU cycles at 20 MHz */
/* Wire time is 16.49 ms and does NOT change with CPU clock; express it in
 * cycles at the boot clock and scale, so utilisation stays correct after a
 * PLL switch. */
#define WIRE_CYCLES_PER_FRAME_20M 329792u


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

__attribute__((unused)) static void gradient(uint16_t *row, int y)
{
    uint8_t v = (uint8_t)((y * 255) / (SH8601_HEIGHT - 1));
    vec_fill16(row, sh8601_rgb565(v, (uint8_t)(64u + v / 4u),
                                  (uint8_t)(255u - v)), ROW_VECTORS);
}

/* Fixed-point ms with two decimals, from cycles. No float, no scalar loops. */
static void put_ms(const char *label, uint32_t cycles)
{
    uint32_t us = cycles / (CPU_HZ / 1000000u);
    con_puts(label);
    con_dec((int32_t)(us / 1000u)); con_putc('.');
    con_dec((int32_t)((us % 1000u) / 10u));
    con_puts("ms ");
}

/* Busy-wait an exact number of CPU CYCLES, independent of g_cpu_hz. The host
 * timestamps the marker, so the wall-clock interval reveals the true frequency
 * - an independent check that does not trust our own idea of the clock. */
static void tick_cycles(uint32_t cycles)
{
    uint32_t s0 = cpu_cycles();
    while ((cpu_cycles() - s0) < cycles) { }
}

void app_entry(void)
{
    int rc, i;

    con_puts("\r\n=== metal99 : PLL clock switch ===\r\n");
    con_puts("MARK lines are every 20,000,000 CPU CYCLES exactly.\r\n");
    con_puts("host interval 1.0s => 20MHz;  0.125s => 160MHz\r\n");

    con_puts("cpu_src="); con_dec((int32_t)clk_cpu_src());
    con_puts(" (0=XTAL 1=PLL)\r\n");

    for (i = 0; i < 3; i++) { tick_cycles(20000000u); con_puts("MARK boot\r\n"); }

    rc = clk_set_cpu_pll(160u);
    con_puts("clk_set_cpu_pll(160) rc="); con_dec((int32_t)rc);
    con_puts("  cpu_src="); con_dec((int32_t)clk_cpu_src());
    con_puts("\r\n");

    for (i = 0; i < 8; i++) { tick_cycles(20000000u); con_puts("MARK pll\r\n"); }

    con_puts("survived the switch. bringing up the panel at the new clock.\r\n");

    spi2_init();
    gdma_init();
    rc = sh8601_init();
    con_puts("sh8601_init rc="); con_dec((int32_t)rc); con_puts("\r\n");
    con_puts("xport | render flush total | eff kB/s | wire-util% | fps\r\n");

    for (i = 0; ; i++) {
        const sh8601_stats *st;
        uint32_t kbps, util, fps10;

        sh8601_set_dma(i & 1);
        rc = sh8601_write_frame(colorbars);
        if (rc != SPI2_OK) {
            con_puts("  frame FAILED rc="); con_dec((int32_t)rc); con_puts("\r\n");
            delay_ms(1000u); continue;
        }
        st = sh8601_last_frame();
        kbps  = (st->total_cycles == 0u) ? 0u
              : (uint32_t)(((uint64_t)st->bytes * CPU_HZ) / st->total_cycles / 1024u);
        util  = (st->flush_cycles == 0u) ? 0u
              : (WIRE_CYCLES_PER_FRAME_20M * (CPU_HZ / 1000000u) / 20u * 100u) / st->flush_cycles;
        fps10 = (st->total_cycles == 0u) ? 0u
              : (uint32_t)(((uint64_t)CPU_HZ * 10u) / st->total_cycles);

        con_puts(((i & 1) == 0) ? " FIFO | " : " GDMA | ");
        put_ms("", st->render_cycles);
        put_ms("", st->flush_cycles);
        put_ms("", st->total_cycles);
        con_puts("| "); con_dec((int32_t)kbps); con_puts(" kB/s ");
        con_puts("| "); con_dec((int32_t)util); con_puts("% ");
        con_puts("| "); con_dec((int32_t)(fps10 / 10u)); con_putc('.');
        con_dec((int32_t)(fps10 % 10u)); con_puts("\r\n");
        delay_ms(400u);
    }
}
