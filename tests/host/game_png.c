/*
 * Render the game app to an image. The app is the real one; only the panel is
 * simulated, so what appears here is what the device would push.
 */
#include <stdio.h>
#include <stdlib.h>
#include "app.h"
#include "ui.h"
#include "sh8601.h"

#define W SH8601_WIDTH
#define H SH8601_HEIGHT

static uint16_t g_fb[H][W];
static uint16_t g_row[W];

/* The panel, as far as the app can tell. */
int sh8601_write_frame(void (*rowfn)(uint16_t *row, int y))
{
    int y, x;
    for (y = 0; y < H; y++) {
        rowfn(g_row, y);
        for (x = 0; x < W; x++) g_fb[y][x] = g_row[x];
    }
    return 0;
}
int sh8601_write_span_x(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                        void (*rowfn)(uint16_t *, int))
{ (void)x0;(void)y0;(void)x1;(void)y1;(void)rowfn; return 0; }

int main(int argc, char **argv)
{
    int frames = (argc > 1) ? atoi(argv[1]) : 1, f, y, x;
    FILE *fp;

    if (APP.init) APP.init();
    for (f = 0; f < frames; f++) {
        /* Fire into the upper middle every so often, where craft live - enough
         * to catch a shatter mid-flight in the rendered frame. */
        if (APP.event && f > 20 && (f % 11) == 0) {
            ui_event e;
            e.kind = UI_PRESS; e.id = 0;
            e.x = (uint16_t)(120 + ((f * 37) % 140));
            e.y = (uint16_t)(60  + ((f * 53) % 90));
            e.ax = e.x; e.ay = e.y; e.ms = 0u;
            APP.event(&e);
        }
        (void)APP.frame((uint32_t)f);
    }

    fp = fopen("out_game.ppm", "wb");
    if (!fp) { perror("out_game.ppm"); return 1; }
    fprintf(fp, "P6\n%d %d\n255\n", W, H);
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            uint16_t v = g_fb[y][x];
            uint16_t c = (uint16_t)((v >> 8) | (v << 8));
            unsigned char px[3];
            px[0] = (unsigned char)(((c >> 11) & 0x1Fu) << 3);
            px[1] = (unsigned char)(((c >> 5)  & 0x3Fu) << 2);
            px[2] = (unsigned char)((c & 0x1Fu) << 3);
            fwrite(px, 1, 3, fp);
        }
    fclose(fp);
    printf("app \"%s\", frame %d -> out_game.ppm\n", APP.name, frames);
    return 0;
}
