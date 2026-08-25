/*
 * The demo application: a scrolling bar, a title, and a touch readout.
 *
 * This is what used to be inline in main.c. It is now an ordinary app - the
 * same shape anything else would take - and it is what tests/host/gfx_png
 * renders when you want to look at a change without a board.
 */
#include <stddef.h>
#include "app.h"
#include "gfx.h"
#include "font.h"
#include "sh8601.h"

#define BAR_H      96
#define BAR_TRAVEL (SH8601_HEIGHT - BAR_H)
#define BLACKCOL   sh8601_rgb565(0, 0, 0)
#define BARCOL     sh8601_rgb565(255, 60, 0)
#define TOUCHCOL   sh8601_rgb565(120, 220, 255)

#define MAXC 2

static int      g_bar_y;
/*
 * Live contacts, kept BY IDENTITY and compacted on release.
 *
 * The first version kept a count and indexed by it: press A, press B, release
 * A, and the count fell to 1 while slot 0 still held A - so the display showed
 * the finger that had LEFT and hid the one still down. A count is not a set,
 * and events arrive per contact, not per slot.
 */
static ui_event g_c[MAXC];
static int      g_nc;
static int      g_taps;

static int find_contact(uint8_t id)
{
    int i;
    for (i = 0; i < g_nc; i++) if (g_c[i].id == id) return i;
    return -1;
}

/* Decimal into a buffer, fixed width. No libc. */
static void u32str(char *b, uint32_t v, int width)
{
    int i;
    for (i = width - 1; i >= 0; i--) { b[i] = (char)('0' + (v % 10u)); v /= 10u; }
    b[width] = '\0';
}

static void fmt_point(char *b, const ui_event *e)
{
    b[0] = (char)('0' + (e->id & 0x0Fu));
    b[1] = ' '; b[2] = 'X'; b[3] = ':';
    u32str(b + 4, (uint32_t)e->x, 3);
    b[7] = ' '; b[8] = 'Y'; b[9] = ':';
    u32str(b + 10, (uint32_t)e->y, 3);
}

static void demo_init(void)
{
    g_bar_y = SH8601_HEIGHT / 2 - BAR_H / 2;
    g_nc    = 0;
    g_taps  = 0;
}

static void demo_event(const ui_event *e)
{
    int i;
    switch (e->kind) {
    case UI_PRESS:
        if (find_contact(e->id) < 0 && g_nc < MAXC) g_c[g_nc++] = *e;
        break;
    case UI_DRAG:
        i = find_contact(e->id);
        if (i >= 0) g_c[i] = *e;
        break;
    case UI_RELEASE:
        i = find_contact(e->id);
        if (i >= 0) {
            for (; i + 1 < g_nc; i++) g_c[i] = g_c[i + 1];   /* compact */
            g_nc--;
        }
        break;
    case UI_TAP:
        /* Anchored: a press that began elsewhere and lifted here counts for
         * nothing, which is what a button wants. */
        if (ui_anchored_in(e, 0, 0, SH8601_WIDTH - 1, 119)) g_taps++;
        break;
    default:
        break;
    }
}

static void demo_frame(uint32_t f)
{
    char b[16];

    g_bar_y = (g_bar_y + ((f < 180u) ? 0 : 4)) % BAR_TRAVEL;

    /* The WHOLE scene, every frame, in z-order. gfx diffs it against what the
     * panel holds, so describing something unchanged transmits nothing. */
    (void)gfx_solid(0u, SH8601_HEIGHT - 1u, BLACKCOL);
    (void)gfx_solid((uint16_t)g_bar_y, (uint16_t)(g_bar_y + BAR_H - 1), BARCOL);
    (void)gfx_text(0, 16u, 8u, "metal99 60Hz", TOUCHCOL, &share_mono_16x32);

    if (g_nc > 0) { fmt_point(b, &g_c[0]);
                    (void)gfx_text(1, 16u, 56u, b, TOUCHCOL, &share_mono_16x32); }
    else          { gfx_text_clear(1); }

    if (g_nc > 1) { fmt_point(b, &g_c[1]);
                    (void)gfx_text(2, 16u, 96u, b, TOUCHCOL, &share_mono_16x32); }
    else          { gfx_text_clear(2); }

    b[0] = 'T'; b[1] = 'A'; b[2] = 'P'; b[3] = 'S'; b[4] = ':';
    u32str(b + 5, (uint32_t)g_taps, 3);
    (void)gfx_text(3, 16u, 400u, b, TOUCHCOL, &share_mono_16x32);
}

const app_t APP = { "demo", demo_init, demo_frame, demo_event };
