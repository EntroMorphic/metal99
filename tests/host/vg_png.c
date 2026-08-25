/*
 * Render a vector test pattern to an image.
 *
 * A rasteriser is all edge cases - steep vs shallow, clipped, flat, reversed -
 * and every one of them is obvious in a picture and invisible in a number.
 * This is the first thing to run after touching vg.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include "vg.h"
#include "sh8601.h"
#include "trig.h"

#define W SH8601_WIDTH
#define H SH8601_HEIGHT

static uint16_t g_fb[H][W];
static uint16_t g_row[W];

static void put_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    int y, x;
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            uint16_t v = g_fb[y][x];
            uint16_t c = (uint16_t)((v >> 8) | (v << 8));
            unsigned char px[3];
            px[0] = (unsigned char)(((c >> 11) & 0x1Fu) << 3);
            px[1] = (unsigned char)(((c >> 5)  & 0x3Fu) << 2);
            px[2] = (unsigned char)((c & 0x1Fu) << 3);
            fwrite(px, 1, 3, f);
        }
    fclose(f);
}

static void flush_frame(void)
{
    int y, x;
    vg_finish();
    for (y = 0; y < H; y++) {
        vg_rowfn(g_row, y);
        for (x = 0; x < W; x++) g_fb[y][x] = g_row[x];
    }
}

int main(void)
{
    const uint16_t CYAN = 0x1F7Fu, AMBER = 0x00FDu, GREEN = 0xE007u;
    int i;

    vg_set_bg(0x0000u);
    vg_begin();

    /* 1. a starburst: every slope, steep through shallow, both directions */
    for (i = 0; i < TRIG_FULL; i += 8) {          /* 32 evenly spaced spokes */
        int cx = 184, cy = 120, r = 100;
        vg_line(cx, cy,
                cx + (int)((icos(i) * r) >> 16),
                cy + (int)((isin(i) * r) >> 16), CYAN);
    }

    /* 2. exact horizontal and vertical - the cases a span formula gets wrong */
    vg_line(20, 250, 348, 250, AMBER);
    vg_line(20, 254, 348, 254, AMBER);
    vg_line(184, 240, 184, 300, AMBER);

    /* 3. clipping: segments that start or end well off every edge */
    vg_line(-200, 270, 200, 290, GREEN);
    vg_line(200, 290, 600, 310, GREEN);
    vg_line(100, -80, 140, 320, GREEN);

    /* 4. a wireframe box in perspective - what the game actually draws */
    { int t = 300, b = 430, l = 60, r = 308, il = 130, ir = 238, it = 340, ib = 390;
      vg_line(l,b, r,b, CYAN); vg_line(l,b, il,it, CYAN);
      vg_line(r,b, ir,it, CYAN); vg_line(il,it, ir,it, CYAN);
      vg_line(il,ib, ir,ib, CYAN); vg_line(l,b, il,ib, CYAN);
      vg_line(r,b, ir,ib, CYAN); (void)t; }

    flush_frame();
    put_ppm("out_vg.ppm");
    printf("vector test pattern: %u segment(s) dropped\n", vg_overflow());
    return 0;
}
