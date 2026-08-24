#include "io.h"
#include "spi2.h"

extern uint32_t _bss_start;
extern uint32_t _bss_end;

static uint32_t ccount(void)
{
    uint32_t c;
    __asm__ __volatile__ ("rsr %0, ccount" : "=a"(c));
    return c;
}

static void show(const char *name, uint32_t v)
{
    con_puts("  "); con_puts(name); con_puts(" = "); con_hex32(v); con_puts("\r\n");
}

void app_entry(void)
{
    uint8_t buf[64];
    uint32_t t0, quad, single, delta, khz;
    int i;

    con_puts("\r\n=== metal99 : SPI2 (L1 platform) ===\r\n");
    spi2_init();

    show("SPI_CLOCK", REG32(0x6002400Cu));
    show("SPI_USER ", REG32(0x60024010u));
    show("SPI_MISC ", REG32(0x60024020u));
    show("CLK_GATE ", REG32(0x600240E8u));
    show("clk->gpio11", REG32(0x60004554u + 4u * 11u));
    show("cs ->gpio12", REG32(0x60004554u + 4u * 12u));

    for (i = 0; i < 64; i++) buf[i] = (uint8_t)i;

    /* Repeats forever so a capture window at any moment sees the result. */
    for (;;) {
        /* Same 64 bytes on 4 lines (128 spi clocks) vs 1 line (512). The
         * 384-clock difference isolates bus rate from per-call overhead. */
        t0 = ccount();
        for (i = 0; i < 300; i++) spi2_xfer(buf, 64u, 1, 0);
        quad = (ccount() - t0) / 300u;

        t0 = ccount();
        for (i = 0; i < 300; i++) spi2_xfer(buf, 64u, 0, 0);
        single = (ccount() - t0) / 300u;

        delta = (single > quad) ? (single - quad) : 1u;
        khz = (384u * 20000u) / delta;

        con_puts("64B quad="); con_dec((int32_t)quad);
        con_puts(" single="); con_dec((int32_t)single);
        con_puts(" delta="); con_dec((int32_t)delta);
        con_puts(" -> "); con_dec((int32_t)khz); con_puts(" kHz  ");
        con_puts((khz > 38000u && khz < 42000u)
                 ? "PASS: 40MHz QSPI, quad active\r\n"
                 : "FAIL: unexpected bus rate\r\n");

        t0 = ccount();
        while ((ccount() - t0) < 40000000u) { }   /* 2 s at 20 MHz */
    }
}
