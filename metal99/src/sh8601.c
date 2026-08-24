#include "sh8601.h"
#include "spi2.h"
#include <stddef.h>

#define OPCODE_PARAM 0x02u

int sh8601_cmd(uint8_t cmd, const uint8_t *params, uint32_t n)
{
    uint8_t word[4];
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
    return sh8601_cmd(0x51u, &level, 1u);
}

#define OPCODE_PIXEL 0x32u

uint16_t sh8601_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
    return (uint16_t)((c >> 8) | (c << 8));   /* panel wants big-endian */
}

int sh8601_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t p[4];
    int rc;

    p[0] = (uint8_t)(x0 >> 8); p[1] = (uint8_t)(x0 & 0xFFu);
    p[2] = (uint8_t)(x1 >> 8); p[3] = (uint8_t)(x1 & 0xFFu);
    rc = sh8601_cmd(0x2Au, p, 4u);
    if (rc != SPI2_OK) return rc;

    p[0] = (uint8_t)(y0 >> 8); p[1] = (uint8_t)(y0 & 0xFFu);
    p[2] = (uint8_t)(y1 >> 8); p[3] = (uint8_t)(y1 & 0xFFu);
    return sh8601_cmd(0x2Bu, p, 4u);
}

/* Chunked send with CS held throughout; only the very last chunk of the very
 * last row releases it. The panel treats a CS rise as end-of-write. */
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

int sh8601_write_frame(void (*rowfn)(uint16_t *row, int y))
{
    static uint16_t row[SH8601_WIDTH];
    uint8_t word[4];
    int rc, y;

    if (rowfn == NULL) return SPI2_E_NULL;

    rc = sh8601_set_window(0u, 0u, SH8601_WIDTH - 1u, SH8601_HEIGHT - 1u);
    if (rc != SPI2_OK) return rc;

    /* 0x32 opcode selects the four-line data phase; 0x2C is memory-write. */
    word[0] = OPCODE_PIXEL; word[1] = 0x00u; word[2] = 0x2Cu; word[3] = 0x00u;
    rc = spi2_xfer(word, 4u, 0 /* command is one-line */, 1 /* hold CS */);
    if (rc != SPI2_OK) return rc;

    for (y = 0; y < SH8601_HEIGHT; y++) {
        rowfn(row, y);
        rc = stream((const uint8_t *)row, (uint32_t)SH8601_WIDTH * 2u,
                    (y == SH8601_HEIGHT - 1));
        if (rc != SPI2_OK) return rc;
    }
    return SPI2_OK;
}
