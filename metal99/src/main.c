#include <stddef.h>
#include "io.h"
#include "spi2.h"

static void show(const char *name, uint32_t v)
{
    con_puts("  "); con_puts(name); con_puts(" = "); con_hex32(v); con_puts("\r\n");
}

static void check(const char *what, int pass)
{
    con_puts(pass ? "  PASS  " : "  FAIL  "); con_puts(what); con_puts("\r\n");
}

void app_entry(void)
{
    uint8_t buf[256];
    uint32_t t0, quad, single, small, delta, khz;
    int i;

    con_puts("\r\n=== metal99 : SPI2 red-team regression ===\r\n");
    spi2_init();

    show("SPI_CLOCK ", REG32(0x6002400Cu));
    show("SPI_SLAVE ", REG32(0x600240E0u));
    show("SPI_DMACFG", REG32(0x60024030u));
    show("CLK_GATE  ", REG32(0x600240E8u));

    for (i = 0; i < 256; i++) buf[i] = (uint8_t)i;

    /* --- F2/F3: inherited state cleared, config latched by init --- */
    check("SPI_SLAVE cleared",    REG32(0x600240E0u) == 0u);
    /* Bits 0/1 are READ-ONLY status (DMA_OUTFIFO_EMPTY, DMA_INFIFO_FULL),
     * reset-default 1 and meaning "idle". Only the writable bits must be 0. */
    check("SPI_DMA_CONF writable bits cleared",
          (REG32(0x60024030u) & ~0x3u) == 0u);

    /* --- F1: out-of-range now reports instead of silently dropping --- */
    check("len=0   -> E_LEN",  spi2_xfer(buf, 0u,  1, 0) == SPI2_E_LEN);
    check("len=65  -> E_LEN",  spi2_xfer(buf, 65u, 1, 0) == SPI2_E_LEN);
    check("NULL    -> E_NULL", spi2_xfer(NULL, 8u, 1, 0) == SPI2_E_NULL);
    check("len=64  -> OK",     spi2_xfer(buf, 64u, 1, 0) == SPI2_OK);

    /* --- F1: arbitrary length chunks internally, incl. a ragged tail --- */
    check("write 200B -> OK",  spi2_write(buf, 200u, 1) == SPI2_OK);
    check("write 1B   -> OK",  spi2_write(buf, 1u,   1) == SPI2_OK);
    check("write 0B   -> E",   spi2_write(buf, 0u,   1) == SPI2_E_LEN);

    for (;;) {
        /* --- regression: bus rate must still be 40 MHz --- */
        t0 = cpu_cycles();
        for (i = 0; i < 300; i++) (void)spi2_xfer(buf, 64u, 1, 0);
        quad = (cpu_cycles() - t0) / 300u;

        t0 = cpu_cycles();
        for (i = 0; i < 300; i++) (void)spi2_xfer(buf, 64u, 0, 0);
        single = (cpu_cycles() - t0) / 300u;

        /* --- F6: small transfers should be much cheaper now (was ~889) --- */
        t0 = cpu_cycles();
        for (i = 0; i < 300; i++) (void)spi2_xfer(buf, 4u, 0, 0);
        small = (cpu_cycles() - t0) / 300u;

        delta = (single > quad) ? (single - quad) : 1u;
        khz = (384u * (CPU_HZ / 1000u)) / delta;

        con_puts("  64Bq="); con_dec((int32_t)quad);
        con_puts(" 64Bs="); con_dec((int32_t)single);
        con_puts(" 4B="); con_dec((int32_t)small);
        con_puts(" -> "); con_dec((int32_t)khz); con_puts(" kHz  ");
        con_puts((khz > 38000u && khz < 42000u) ? "PASS 40MHz\r\n" : "FAIL rate\r\n");

        delay_ms(2000u);
    }
}
