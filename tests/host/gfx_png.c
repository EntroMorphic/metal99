/*
 * Render metal99 scenes to image files, on the host.
 *
 * WHY. The panel cannot be read back, so for this whole project "what is on the
 * screen" has meant "what a human says is on the screen". That loop is slow, it
 * cannot distinguish a wedged panel from wrong pixels, and it has repeatedly
 * been the limiting instrument - most recently for span-boundary debris, where
 * every automated check reported correct and only an eye disagreed.
 *
 * This does not replace the panel: it cannot see the wire. What it CAN see is
 * the difference between two things that were previously conflated:
 *
 *   ideal  - a full render of the model. What the screen SHOULD show.
 *   panel  - a virtual panel to which ONLY the spans elide actually marked have
 *            been applied, frame after frame. What the screen WOULD show if the
 *            transport were perfect.
 *   diff   - where they disagree. Any pixel here is a MARKING bug: something
 *            changed in the model and nothing told the panel about it.
 *
 * A clean diff with a dirty screen means the fault is below the ledger - the
 * bytes were right and something downstream lost them. That is a different bug
 * from a missed mark, and until now there was no way to tell them apart.
 *
 * Links the real gfx.c and elide.c; only the transport is stubbed.
 *
 *   make -C tests/host png
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "gfx.h"
#include "elide.h"
#include "app.h"
#include "ui.h"
#include "font.h"
#include "gfx_stubs.h"

#define W SH8601_WIDTH
#define H SH8601_HEIGHT

static uint16_t g_panel[H][W];
static uint16_t g_ideal[H][W];
static uint16_t g_row[W];

/* RGB565 wire order (big-endian) back to plain bytes for a PPM. */
static void put_ppm(const char *path, const uint16_t *fb, int mark_diff)
{
    FILE *f = fopen(path, "wb");
    int y, x;
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            uint16_t v = fb[y * W + x];
            uint16_t c = (uint16_t)((v >> 8) | (v << 8));   /* undo wire order */
            unsigned char px[3];
            px[0] = (unsigned char)(((c >> 11) & 0x1Fu) << 3);
            px[1] = (unsigned char)(((c >> 5)  & 0x3Fu) << 2);
            px[2] = (unsigned char)((c & 0x1Fu) << 3);
            if (mark_diff && g_panel[y][x] != g_ideal[y][x]) {
                px[0] = 255; px[1] = 0; px[2] = 255;        /* magenta */
            }
            fwrite(px, 1, 3, f);
        }
    fclose(f);
}

/* Apply exactly the spans elide handed the transport this frame. */
static void panel_apply(void)
{
    int i, y, x;
    for (i = 0; i < g_nmarks; i++)
        for (y = g_marks[i].y0; y <= g_marks[i].y1; y++) {
            stub_render(y, g_row);
            for (x = g_marks[i].x0; x <= g_marks[i].x1; x++)
                g_panel[y][x] = g_row[x];
        }
}

static void render_ideal(void)
{
    int y, x;
    for (y = 0; y < H; y++) {
        stub_render(y, g_row);
        for (x = 0; x < W; x++) g_ideal[y][x] = g_row[x];
    }
}

int main(int argc, char **argv)
{
    int frames = (argc > 1) ? atoi(argv[1]) : 120;
    int f, y, x, bad = 0;

    gfx_init();
    ui_init();
    (void)gfx_present();
    elide_set_resync(0u);          /* no scrubbing: a missed mark must persist */
    if (APP.init) APP.init();

    /* Seed both from a full repaint, as the first present does on hardware. */
    (void)gfx_present();
    render_ideal();
    memcpy(g_panel, g_ideal, sizeof g_panel);

    for (f = 0; f < frames; f++) {
        /* A finger dragging diagonally for the middle third of the run, so the
         * rendered scene exercises press, drag and release rather than showing
         * an idle screen. */
        /* A brief tap early (so TAPS increments and the counter is not always
         * zero), then a finger held down for the rest, so the final image shows
         * the touch readout rather than an idle screen. */
        /* A brief tap early so the counter is not always zero, then one finger
         * dragging, then a SECOND finger joining for the last quarter - so the
         * final image exercises the multi-touch path rather than showing an
         * idle screen. */
        if (f == 10 || f == 11) stub_touch_set(1, 120, 60, 3);
        else if (f > frames / 3) {
            stub_touch_set(1, 40 + (f % 200), 150 + (f / 4), 0);
            if (f > (3 * frames) / 4) stub_touch_set2(220 - (f % 90), 300, 5);
        } else stub_touch_set(0, 0, 0, 0);

        ui_poll(APP.event);
        if (APP.frame) APP.frame((uint32_t)f);

        stub_reset();
        (void)gfx_present();
        panel_apply();
        stub_advance_ms(16u);
    }
    render_ideal();

    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            if (g_panel[y][x] != g_ideal[y][x]) bad++;

    put_ppm("out_ideal.ppm", &g_ideal[0][0], 0);
    put_ppm("out_panel.ppm", &g_panel[0][0], 0);
    put_ppm("out_diff.ppm",  &g_panel[0][0], 1);
    printf("app \"%s\", %d frames, resync off: %d pixel(s) where the panel "
           "disagrees with the model\n", APP.name, frames, bad);
    printf("  out_ideal.ppm  out_panel.ppm  out_diff.ppm (magenta = mismatch)\n");
    return 0;
}
