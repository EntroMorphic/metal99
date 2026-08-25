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
    if (rc != SPI2_OK) { spi2_cs_release(); return rc; }

    if (n > 0u) {
        /* CS is held here. Every exit below MUST release it or the panel stays
         * selected and swallows the next command word as pixel data. */
        if (params == NULL) { spi2_cs_release(); return SPI2_E_NULL; }
        /* Parameters are one-line for opcode 0x02. spi2_write releases CS on
         * its final chunk, which ends the command. */
        rc = spi2_write(params, n, 0);
        if (rc != SPI2_OK) { spi2_cs_release(); return rc; }
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

void sh8601_set_dma(int on) { g_use_dma = on; }

static int g_overlap = 1;
void sh8601_set_overlap(int on) { g_overlap = on; }

/*
 * Chunked send with CS held throughout; only the very last chunk of the very
 * last row releases it. The panel treats a CS rise as end-of-write.
 *
 * FIFO ONLY. This used to branch on g_use_dma and do one DMA per row, but
 * sh8601_write_span() has sent DMA traffic through the banded path since
 * DESIGN.md 6.6j and only ever calls stream() from its !g_use_dma branch - so
 * that half was unreachable, along with the descriptor and the debug accessor
 * that read it. It looked live, which is worse than being absent. The
 * one-DMA-per-row measurements it produced are recorded in DESIGN.md 6.6i.
 */
static int stream(const uint8_t *d, uint32_t n, int final_row)
{
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




/*
 * BANDED, DOUBLE-BUFFERED SPAN WRITE.
 *
 * Two changes over one-DMA-per-row, each closing a measured cost:
 *
 *  BANDING removes per-transfer overhead. 448 row transfers cost 0.97 ms above
 *  the 16.49 ms of actual wire time; 14 band transfers cost almost nothing.
 *  The descriptor size field is 12 bits, so one descriptor carries at most
 *  4095 bytes - 5 rows. A band is therefore a CHAIN, and we use 4 rows per
 *  descriptor (2944 B) to divide evenly.
 *
 *  OVERLAP hides rendering. DMA is asynchronous and the CPU is idle for the
 *  whole transfer, so band N+1 is rendered while band N is on the wire. Frame
 *  time becomes max(render, flush) instead of their sum.
 */
#define BAND_ROWS      32
#define ROWS_PER_DESC  4
#define DESCS_PER_BAND (BAND_ROWS / ROWS_PER_DESC)

static uint16_t  VEC_ALIGN g_band[2][BAND_ROWS * SH8601_WIDTH];
static gdma_desc           g_chain[2][DESCS_PER_BAND];

/* Build a descriptor chain covering `rows` rows of band buffer `b`. */
static void band_chain(int b, int rows)
{
    int used = (rows + ROWS_PER_DESC - 1) / ROWS_PER_DESC;
    int d;
    for (d = 0; d < used; d++) {
        int r0 = d * ROWS_PER_DESC;
        int n  = rows - r0;
        int last;
        if (n > ROWS_PER_DESC) n = ROWS_PER_DESC;
        last = (d == used - 1);

        g_chain[b][d].dw0    = GDMA_DW0(n * SH8601_WIDTH * 2,
                                        n * SH8601_WIDTH * 2, last);
        g_chain[b][d].buffer = &g_band[b][(size_t)r0 * SH8601_WIDTH];
        g_chain[b][d].next   = last ? NULL : &g_chain[b][d + 1];
    }
}

#define XGRID 8                     /* 8 px = 16 B: the vector/FIFO alignment unit */

int sh8601_write_span(uint16_t y0, uint16_t y1, void (*rowfn)(uint16_t *row, int y))
{
    return sh8601_write_span_x(0u, y0, (uint16_t)(SH8601_WIDTH - 1), y1, rowfn);
}

int sh8601_write_span_x(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                        void (*rowfn)(uint16_t *row, int y))
{
    uint8_t VEC_ALIGN word[16];
    uint32_t t_frame = cpu_cycles();
    uint32_t t_mark;
    uint32_t span_bytes;
    int rc, y, b = 0, pending = 0, pend_rows = 0, full;

    if (rowfn == NULL)                  return SPI2_E_NULL;
    if (y1 < y0 || y1 >= SH8601_HEIGHT) return SPI2_E_LEN;
    if (x1 < x0 || x1 >= SH8601_WIDTH)  return SPI2_E_LEN;

    /* Snap OUTWARD to the alignment grid: cover more, never less. */
    x0 = (uint16_t)(x0 & ~(uint16_t)(XGRID - 1));
    x1 = (uint16_t)((x1 | (uint16_t)(XGRID - 1)));
    if (x1 >= SH8601_WIDTH) x1 = (uint16_t)(SH8601_WIDTH - 1);

    full = (x0 == 0u) && (x1 == (uint16_t)(SH8601_WIDTH - 1));
    span_bytes = (uint32_t)(x1 - x0 + 1) * 2u;

    rc = sh8601_set_window(x0, y0, x1, y1);
    if (rc != SPI2_OK) return rc;

    word[0] = OPCODE_PIXEL; word[1] = 0x00u; word[2] = 0x2Cu; word[3] = 0x00u;
    rc = spi2_xfer(word, 4u, 0, 1);
    if (rc != SPI2_OK) { spi2_cs_release(); return rc; }
    /* CS is now held until the final row. EVERY exit below releases it. */

    g_stats.render_cycles = 0u;
    g_stats.flush_cycles  = 0u;
    g_stats.bytes         = 0u;

    /* FIFO transport: no banding possible (64-byte FIFO), no overlap possible
     * (transfers are synchronous). Also the ONLY path for a sub-width span - a
     * band of partial rows is not contiguous in memory, so it would need one
     * descriptor per row and overrun the chain. */
    if (!g_use_dma || !full) {
        static uint16_t VEC_ALIGN one[SH8601_WIDTH];
        for (y = (int)y0; y <= (int)y1; y++) {
            t_mark = cpu_cycles();
            rowfn(one, y);                       /* renders the whole row   */
            g_stats.render_cycles += cpu_cycles() - t_mark;

            t_mark = cpu_cycles();
            /* &one[x0] is 16-byte aligned because x0 is a multiple of 8 px,
             * and span_bytes is a multiple of 16 for the same reason - so the
             * FIFO's vector load reads exactly the bytes it sends. */
            rc = stream((const uint8_t *)&one[x0], span_bytes, (y == (int)y1));
            g_stats.flush_cycles += cpu_cycles() - t_mark;
            if (rc != SPI2_OK) { spi2_cs_release(); return rc; }
            g_stats.bytes += span_bytes;
        }
        g_stats.total_cycles = cpu_cycles() - t_frame;
        return SPI2_OK;
    }

    for (y = (int)y0; y <= (int)y1; ) {
        int rows = (int)y1 - y + 1;
        int r;
        if (rows > BAND_ROWS) rows = BAND_ROWS;

        /* Render into the free buffer. If a transfer is in flight this runs
         * CONCURRENTLY with it - that is the whole point. */
        t_mark = cpu_cycles();
        for (r = 0; r < rows; r++) {
            rowfn(&g_band[b][(size_t)r * SH8601_WIDTH], y + r);
        }
        g_stats.render_cycles += cpu_cycles() - t_mark;

        /* Only now collect the previous transfer. */
        if (pending) {
            t_mark = cpu_cycles();
            rc = spi2_dma_finish();
            g_stats.flush_cycles += cpu_cycles() - t_mark;
            if (rc != SPI2_OK) { spi2_cs_release(); return rc; }
            g_stats.bytes += (uint32_t)pend_rows * SH8601_WIDTH * 2u;
        }

        band_chain(b, rows);
        t_mark = cpu_cycles();
        rc = spi2_dma_start(g_chain[b], (uint32_t)rows * SH8601_WIDTH * 2u,
                            1 /* quad */, (y + rows > (int)y1) ? 0 : 1);
        g_stats.flush_cycles += cpu_cycles() - t_mark;
        if (rc != SPI2_OK) { spi2_cs_release(); return rc; }

        pending = 1; pend_rows = rows;

        /* Overlap disabled: collect immediately, so the next band's render
         * cannot run concurrently. This is the control for the overlap claim. */
        if (!g_overlap) {
            t_mark = cpu_cycles();
            rc = spi2_dma_finish();
            g_stats.flush_cycles += cpu_cycles() - t_mark;
            if (rc != SPI2_OK) { spi2_cs_release(); return rc; }
            g_stats.bytes += (uint32_t)rows * SH8601_WIDTH * 2u;
            pending = 0;
        }

        y += rows;
        b ^= 1;
    }

    if (pending) {
        t_mark = cpu_cycles();
        rc = spi2_dma_finish();
        g_stats.flush_cycles += cpu_cycles() - t_mark;
        if (rc != SPI2_OK) { spi2_cs_release(); return rc; }
        g_stats.bytes += (uint32_t)pend_rows * SH8601_WIDTH * 2u;
    }
    g_stats.total_cycles = cpu_cycles() - t_frame;
    return SPI2_OK;
}

int sh8601_write_frame(void (*rowfn)(uint16_t *row, int y))
{
    return sh8601_write_span(0u, (uint16_t)(SH8601_HEIGHT - 1), rowfn);
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
