#include "sh8601.h"
#include "spi2.h"
#include "io.h"
#include "vec.h"
#include "gdma.h"
#include <stddef.h>

#define OPCODE_PARAM 0x02u

int sh8601_cmd(uint8_t cmd, const uint8_t *params, uint32_t n)
{
    uint8_t VEC_ALIGN word[16];   /* padded: FIFO load is vectorised */
    int rc;

    word[0] = OPCODE_PARAM;
    word[1] = 0x00u;
    word[2] = cmd;
    word[3] = 0x00u;

    /* Command word on one line. Hold CS if parameters follow, because the
     * panel treats a CS rise as end-of-command. */
    rc = spi2_xfer(word, 4u, 0, (n > 0u) ? 1 : 0);
    if (rc != SPI2_OK) return rc;

    if (n > 0u) {
        if (params == NULL) return SPI2_E_NULL;
        /* Parameters are one-line for opcode 0x02. spi2_write releases CS on
         * its final chunk, which ends the command. */
        rc = spi2_write(params, n, 0);
        if (rc != SPI2_OK) return rc;
    }
    return SPI2_OK;
}

int sh8601_brightness(uint8_t level)
{
    uint8_t VEC_ALIGN p[16];
    p[0] = level;
    return sh8601_cmd(0x51u, p, 1u);
}

#define OPCODE_PIXEL 0x32u

uint16_t sh8601_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
    return (uint16_t)((c >> 8) | (c << 8));   /* panel wants big-endian */
}

int sh8601_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t VEC_ALIGN p[16];
    int rc;

    p[0] = (uint8_t)(x0 >> 8); p[1] = (uint8_t)(x0 & 0xFFu);
    p[2] = (uint8_t)(x1 >> 8); p[3] = (uint8_t)(x1 & 0xFFu);
    rc = sh8601_cmd(0x2Au, p, 4u);
    if (rc != SPI2_OK) return rc;

    p[0] = (uint8_t)(y0 >> 8); p[1] = (uint8_t)(y0 & 0xFFu);
    p[2] = (uint8_t)(y1 >> 8); p[3] = (uint8_t)(y1 & 0xFFu);
    return sh8601_cmd(0x2Bu, p, 4u);
}

static int g_use_dma = 0;
static gdma_desc g_desc;               /* one row fits: 736 B < 4095 B */

void sh8601_set_dma(int on) { g_use_dma = on; }

/* Chunked send with CS held throughout; only the very last chunk of the very
 * last row releases it. The panel treats a CS rise as end-of-write. */
static int stream(const uint8_t *d, uint32_t n, int final_row)
{
    if (g_use_dma) {
        /* One descriptor per row. 736 B is inside the 12-bit size field, so no
         * chaining is needed yet; banding comes next. */
        g_desc.dw0    = GDMA_DW0(n, n, 1);
        g_desc.buffer = d;
        g_desc.next   = NULL;
        return spi2_xfer_dma(&g_desc, n, 1 /* quad */, final_row ? 0 : 1);
    }

    while (n > 0u) {
        uint32_t c = (n > (uint32_t)SPI2_FIFO_BYTES) ? (uint32_t)SPI2_FIFO_BYTES : n;
        int is_last = (final_row != 0) && (c == n);
        int rc = spi2_xfer(d, c, 1 /* quad */, is_last ? 0 : 1);
        if (rc != SPI2_OK) return rc;
        d += c;
        n -= c;
    }
    return SPI2_OK;
}

static sh8601_stats g_stats;

const sh8601_stats *sh8601_last_frame(void) { return &g_stats; }

int sh8601_write_frame(void (*rowfn)(uint16_t *row, int y))
{
    static uint16_t VEC_ALIGN row[SH8601_WIDTH];
    uint32_t t_frame = cpu_cycles();
    uint32_t t_mark;
    uint8_t VEC_ALIGN word[16];
    int rc, y;

    if (rowfn == NULL) return SPI2_E_NULL;

    rc = sh8601_set_window(0u, 0u, SH8601_WIDTH - 1u, SH8601_HEIGHT - 1u);
    if (rc != SPI2_OK) return rc;

    /* 0x32 opcode selects the four-line data phase; 0x2C is memory-write. */
    word[0] = OPCODE_PIXEL; word[1] = 0x00u; word[2] = 0x2Cu; word[3] = 0x00u;
    rc = spi2_xfer(word, 4u, 0 /* command is one-line */, 1 /* hold CS */);
    if (rc != SPI2_OK) return rc;

    g_stats.render_cycles = 0u;
    g_stats.flush_cycles  = 0u;
    g_stats.bytes         = 0u;

    for (y = 0; y < SH8601_HEIGHT; y++) {
        t_mark = cpu_cycles();
        rowfn(row, y);
        g_stats.render_cycles += cpu_cycles() - t_mark;

        t_mark = cpu_cycles();
        rc = stream((const uint8_t *)row, (uint32_t)SH8601_WIDTH * 2u,
                    (y == SH8601_HEIGHT - 1));
        g_stats.flush_cycles += cpu_cycles() - t_mark;
        if (rc != SPI2_OK) return rc;
        g_stats.bytes += (uint32_t)SH8601_WIDTH * 2u;
    }
    g_stats.total_cycles = cpu_cycles() - t_frame;
    return SPI2_OK;
}

int sh8601_sleep(void)
{
    int rc = sh8601_cmd(0x28u, NULL, 0u);      /* display off */
    if (rc != SPI2_OK) return rc;
    delay_ms(20u);
    rc = sh8601_cmd(0x10u, NULL, 0u);          /* sleep in */
    if (rc != SPI2_OK) return rc;
    delay_ms(120u);                            /* datasheet settle */
    return SPI2_OK;
}

int sh8601_init(void)
{
    /*
     * The BSP's nine commands are only the VENDOR portion. The esp_lcd_sh8601
     * driver sends more around them, and leaving those out is why our first
     * cold-start attempt stayed black:
     *
     *   panel_reset() : 0x01 software reset + 80 ms (this board has no reset
     *                   pin, so software reset is the only path)
     *   panel_init()  : 0x36 MADCTL then 0x3A COLMOD, BEFORE the vendor list
     *
     * COLMOD is the pixel-format register. Without it the panel has no idea
     * the stream is RGB565, so pixels land as noise or nothing at all.
     */
    static const uint8_t VEC_ALIGN p36[16] = { 0x00u };   /* MADCTL: RGB element order  */
    static const uint8_t VEC_ALIGN p3A[16] = { 0x55u };   /* COLMOD: 16bpp RGB565       */

    /* Transcribed from Waveshare BSP 2.0.0 (the SH8601 revision - BSP >= 2.0.3
     * is the CO5300 board and its sequence would be wrong here). */
    static const uint8_t VEC_ALIGN p44[16] = { 0x01u, 0xD1u };   /* tear scanline      */
    static const uint8_t VEC_ALIGN p35[16] = { 0x00u };          /* tearing effect on  */
    static const uint8_t VEC_ALIGN p53[16] = { 0x20u };          /* WRCTRLD, BCTRL on  */
    static const uint8_t VEC_ALIGN p51_0[16] = { 0x00u };
    static const uint8_t VEC_ALIGN p51_f[16] = { 0xFFu };
    int rc;

    /* Software reset first - no reset pin on this board. */
    rc = sh8601_cmd(0x01u, NULL, 0u);   if (rc != SPI2_OK) return rc;
    delay_ms(80u);

    /* Format registers must precede the vendor sequence. */
    rc = sh8601_cmd(0x36u, p36, 1u);    if (rc != SPI2_OK) return rc;
    rc = sh8601_cmd(0x3Au, p3A, 1u);    if (rc != SPI2_OK) return rc;

    rc = sh8601_cmd(0x11u, NULL, 0u);   if (rc != SPI2_OK) return rc;
    delay_ms(120u);                     /* MINIMUM after sleep out */

    rc = sh8601_cmd(0x44u, p44, 2u);    if (rc != SPI2_OK) return rc;
    rc = sh8601_cmd(0x35u, p35, 1u);    if (rc != SPI2_OK) return rc;
    rc = sh8601_cmd(0x53u, p53, 1u);    if (rc != SPI2_OK) return rc;
    delay_ms(10u);

    rc = sh8601_set_window(0u, 0u, SH8601_WIDTH - 1u, SH8601_HEIGHT - 1u);
    if (rc != SPI2_OK) return rc;

    /* Brightness 0 BEFORE display-on, full AFTER: suppresses a flash of
     * whatever garbage is sitting in panel RAM at power-up. Keep this order. */
    rc = sh8601_cmd(0x51u, p51_0, 1u);  if (rc != SPI2_OK) return rc;
    delay_ms(10u);
    rc = sh8601_cmd(0x29u, NULL, 0u);   if (rc != SPI2_OK) return rc;
    delay_ms(10u);
    rc = sh8601_cmd(0x51u, p51_f, 1u);  if (rc != SPI2_OK) return rc;

    return SPI2_OK;
}

int sh8601_scroll_def(uint16_t tfa, uint16_t vsa, uint16_t bfa)
{
    uint8_t VEC_ALIGN p[16];
    p[0] = (uint8_t)(tfa >> 8); p[1] = (uint8_t)(tfa & 0xFFu);
    p[2] = (uint8_t)(vsa >> 8); p[3] = (uint8_t)(vsa & 0xFFu);
    p[4] = (uint8_t)(bfa >> 8); p[5] = (uint8_t)(bfa & 0xFFu);
    return sh8601_cmd(0x33u, p, 6u);
}

int sh8601_scroll_start(uint16_t row)
{
    uint8_t VEC_ALIGN p[16];
    p[0] = (uint8_t)(row >> 8);
    p[1] = (uint8_t)(row & 0xFFu);
    return sh8601_cmd(0x37u, p, 2u);
}
