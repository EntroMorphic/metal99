/*
 * UILAB - a fair test for gfx label marking.
 *
 * The last workaround standing is gone from gfx: labels marked FULL WIDTH for
 * as long as span-boundary debris was unexplained, because a narrow label mark
 * sitting among full-width bar marks split one span into three or four. elide
 * now unions extents whenever that is cheaper than another span, so callers no
 * longer need to pre-widen themselves.
 *
 * That change passes every host test and has NOT been proven on glass. The
 * only app that exercises gfx labels is demo, and demo is a bad witness: it
 * smears at the top, it did so before this change, and its known orange-line
 * residual has muddied results all session. Shipping on tests alone is exactly
 * what cost four rounds of hunting earlier today.
 *
 * So: a screen built to make the answer unambiguous.
 *
 *   - A full-width bar STEPS down the screen. Full-width marks, moving, which
 *     is the neighbour a narrow label mark has to coexist with.
 *   - Labels sit at fixed positions and CHANGE every frame, so label rows are
 *     dirty constantly. One sits inside the bar's path, one clear of it, one
 *     right at the top edge where the smear is reported.
 *   - Static reference rules frame the labels. They are never marked after the
 *     first frame, so ANY change to them is debris by definition - the display
 *     has no legitimate reason to touch those pixels again.
 *
 * AND IT A/Bs ITSELF. Every 4 seconds it flips between narrow label marks (the
 * new behaviour) and full-width ones (the old workaround), on identical
 * content, and says which is live. Comparing two flashes from memory is how
 * this project reached a wrong conclusion twice today; comparing two phases of
 * one build is not.
 *
 *   WIDE indicator block = full-width marks (old workaround)
 *   NARROW indicator block = own-column marks (what we want to keep)
 *
 * APP=uilab ./metal99/build.sh
 */
#include <stdint.h>
#include "app.h"
#include "io.h"
#include "gfx.h"
#include "sh8601.h"
#include "ui.h"

#define W SH8601_WIDTH
#define H SH8601_HEIGHT

static uint16_t C_BG, C_BAR, C_TXT, C_RULE, C_MODE;
static uint32_t g_touches;
static uint16_t g_tx, g_ty;

/* Fixed reference rules. Drawn once, never marked again - so any pixel that
 * changes here is debris, with no innocent explanation available. */
static void rules(void)
{
    (void)gfx_rect(0u,   60u,  (uint16_t)(W - 1), 63u,  C_RULE);
    (void)gfx_rect(0u,   140u, (uint16_t)(W - 1), 143u, C_RULE);
    (void)gfx_rect(0u,   300u, (uint16_t)(W - 1), 303u, C_RULE);
    (void)gfx_rect(0u,   380u, (uint16_t)(W - 1), 383u, C_RULE);
}

static void ui_lab_init(void)
{
    C_BG   = sh8601_rgb565(0, 0, 0);
    C_BAR  = sh8601_rgb565(255, 90, 0);      /* the historical offender */
    C_TXT  = sh8601_rgb565(120, 230, 255);
    C_RULE = sh8601_rgb565(0, 90, 140);
    C_MODE = sh8601_rgb565(255, 255, 255);

    (void)gfx_solid(0u, (uint16_t)(H - 1), C_BG);
    rules();
    con_puts("uilab: wide block = full-width label marks (old workaround)\r\n");
    con_puts("uilab: narrow block = own-column marks (new)\r\n");
}

static void num(int id, uint16_t x, uint16_t y, const char *tag, uint32_t v)
{
    char b[24];
    int i = 0, j;
    while (tag[i] && i < 12) { b[i] = tag[i]; i++; }
    /* Five digits, most significant first. No libc. */
    for (j = 10000; j > 0; j /= 10) { b[i++] = (char)('0' + (int)((v / (uint32_t)j) % 10u)); }
    b[i] = '\0';
    (void)gfx_text(id, x, y, b, C_TXT, &share_mono_16x32);
}

static int ui_lab_frame(uint32_t f)
{
    int full = ((f / (60u * 4u)) & 1u) != 0u;
    uint16_t by;

    gfx_dbg_label_full(full);

    /* The bar: full-width marks, stepping so it is always dirty. */
    by = (uint16_t)(160u + ((f / 2u) % 120u));
    (void)gfx_solid(160u, 279u, C_BG);
    (void)gfx_rect(0u, by, (uint16_t)(W - 1), (uint16_t)(by + 24u), C_BAR);

    /* Mode indicator: a RECT, never a label, so the marker cannot itself be
     * the thing under test. */
    (void)gfx_rect(0u, 8u, full ? 200u : 40u, 24u, C_MODE);

    /* Labels: constantly changing, at heights that put them above the bar, in
     * the bar's path, and hard against the top edge where smear is reported. */
    num(0, 16u, 32u,  "TOP ",  f);
    num(1, 16u, 96u,  "MID ",  f * 7u);
    num(2, 16u, 200u, "BAR ",  f * 13u);
    num(3, 16u, 320u, "LOW ",  g_touches);
    num(4, 16u, 400u, "XY  ",  (uint32_t)g_tx * 1000u + g_ty);

    return gfx_present();
}

static void ui_lab_event(const ui_event *e)
{
    if (e->kind == UI_PRESS) g_touches++;
    g_tx = e->x; g_ty = e->y;
}

const app_t APP = { "uilab", 60u, ui_lab_init, ui_lab_frame, ui_lab_event };
