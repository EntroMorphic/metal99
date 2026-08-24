/*
 * Render metal99 frames on the host and write a PPM, so visual iteration does
 * not require a flash cycle. Byte order is undone for viewing: the panel wants
 * big-endian RGB565, a normal image file does not.
 *
 *   make -C tests/host && ./tests/host/host_render bars out.ppm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vec.h"

#define W 368
#define H 448
#define ROW_VECTORS (W * 2 / VEC_BYTES)

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
    return (uint16_t)((c >> 8) | (c << 8));      /* panel wire order */
}

static void colorbars(uint16_t *row, int y)
{
    uint16_t bars[5];
    bars[0] = rgb565(255, 0, 0);   bars[1] = rgb565(0, 255, 0);
    bars[2] = rgb565(0, 0, 255);   bars[3] = rgb565(255, 255, 255);
    bars[4] = rgb565(0, 0, 0);
    vec_fill16(row, bars[(y * 5) / H], ROW_VECTORS);
}

static void gradient(uint16_t *row, int y)
{
    uint8_t v = (uint8_t)((y * 255) / (H - 1));
    vec_fill16(row, rgb565(v, (uint8_t)(64u + v / 4u), (uint8_t)(255u - v)), ROW_VECTORS);
}

int main(int argc, char **argv)
{
    const char *which = (argc > 1) ? argv[1] : "bars";
    const char *out   = (argc > 2) ? argv[2] : "frame.ppm";
    void (*fn)(uint16_t *, int) = strcmp(which, "gradient") ? colorbars : gradient;
    static uint16_t VEC_ALIGN row[W];
    FILE *f = fopen(out, "wb");
    int x, y;

    if (!f) { perror("fopen"); return 1; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (y = 0; y < H; y++) {
        fn(row, y);
        for (x = 0; x < W; x++) {
            uint16_t c = (uint16_t)((row[x] >> 8) | (row[x] << 8));  /* undo swap */
            unsigned char px[3];
            px[0] = (unsigned char)(((c >> 11) & 0x1Fu) << 3);
            px[1] = (unsigned char)(((c >> 5)  & 0x3Fu) << 2);
            px[2] = (unsigned char)((c & 0x1Fu) << 3);
            fwrite(px, 1, 3, f);
        }
    }
    fclose(f);
    printf("%s -> %s (%dx%d)\n", which, out, W, H);
    return 0;
}
