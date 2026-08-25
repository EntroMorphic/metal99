/*
 * Assertions for the input event layer.
 *
 * The gap between "where fingers are" and "what just happened" is small and
 * entirely made of edge cases. These are the ones every program would
 * otherwise reimplement and get wrong, so they are pinned here once.
 *
 * Drives the real ui.c against the stubbed controller and a settable clock.
 */
#include <stdio.h>
#include "ui.h"
#include "app.h"
#include "gfx.h"
#include "font.h"
#include "sh8601.h"
#include "elide.h"
#include "gfx_stubs.h"

static int g_fails;
static int n_press, n_drag, n_release, n_tap, n_long;
static ui_event g_lasttap;

static void check(int ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) g_fails++;
}

static void on_event(const ui_event *e)
{
    switch (e->kind) {
    case UI_PRESS:   n_press++;   break;
    case UI_DRAG:    n_drag++;    break;
    case UI_RELEASE: n_release++; break;
    case UI_TAP:     n_tap++; g_lasttap = *e; break;
    case UI_LONG:    n_long++;    break;
    default: break;
    }
}

static void reset(void)
{
    n_press = n_drag = n_release = n_tap = n_long = 0;
    ui_init();
    stub_touch_set(0, 0, 0, 0);
    ui_poll(on_event);
    n_press = n_drag = n_release = n_tap = n_long = 0;
}

/*
 * Which character is rendered at (x, y)?
 *
 * Renders the label's rows, extracts the bit pattern in that glyph cell, and
 * matches it against the font. Reading the SCREEN rather than the app's
 * internals: a test that inspected app state would have agreed with the bug it
 * was supposed to catch, because the state was self-consistent and wrong.
 */
static char glyph_at(int x, int y, const gfx_font *f)
{
    static uint16_t row[SH8601_WIDTH];
    unsigned g;
    int r, c, bpr = f->w / 8;
    for (g = 0; g < f->count; g++) {
        int match = 1;
        for (r = 0; r < (int)f->h && match; r++) {
            stub_render(y + r, row);
            for (c = 0; c < (int)f->w && match; c++) {
                uint8_t bits = f->bits[((g * f->h) + (unsigned)r) * (unsigned)bpr
                                       + (unsigned)(c / 8)];
                int ink_font   = (bits & (0x80u >> (c % 8))) != 0;
                int ink_screen = row[x + c] != row[SH8601_WIDTH - 1]; /* vs bg */
                if (ink_font != ink_screen) match = 0;
            }
        }
        if (match) return (char)(f->first + g);
    }
    return '?';
}

int main(void)
{
    printf("ui_test: metal99 input events\n");

    /* ---- a quick press and release in one place is a TAP ---- */
    reset();
    stub_touch_set(1, 100, 100, 7); ui_poll(on_event);
    stub_advance_ms(50u);
    stub_touch_set(0, 0, 0, 0);     ui_poll(on_event);
    check(n_press == 1,   "press delivers exactly one PRESS");
    check(n_release == 1, "release delivers exactly one RELEASE");
    check(n_tap == 1,     "a quick press and release in place is a TAP");
    check(g_lasttap.ax == 100 && g_lasttap.ay == 100,
          "  the tap carries the anchor, not just the current point");

    /* ---- held too long is NOT a tap ---- */
    reset();
    stub_touch_set(1, 100, 100, 1); ui_poll(on_event);
    stub_advance_ms(UI_TAP_MS + 100u);
    stub_touch_set(0, 0, 0, 0);     ui_poll(on_event);
    check(n_release == 1 && n_tap == 0, "a slow press-release is NOT a tap");

    /* ---- moved too far is NOT a tap, even if quick ---- */
    reset();
    stub_touch_set(1, 100, 100, 2); ui_poll(on_event);
    stub_touch_set(1, 100 + UI_TAP_SLOP + 10, 100, 2); ui_poll(on_event);
    stub_advance_ms(30u);
    stub_touch_set(0, 0, 0, 0);     ui_poll(on_event);
    check(n_drag >= 1, "movement while down delivers DRAG");
    check(n_tap == 0,  "a quick press that WANDERED is not a tap");

    /* ---- drag keeps the anchor ---- */
    reset();
    stub_touch_set(1, 50, 60, 4); ui_poll(on_event);
    stub_touch_set(1, 200, 300, 4); ui_poll(on_event);
    {
        ui_event probe;
        probe.x = 200; probe.y = 300; probe.ax = 50; probe.ay = 60;
        check(ui_in_rect(&probe, 150, 250, 250, 350),
              "ui_in_rect tests where the finger IS");
        check(!ui_anchored_in(&probe, 150, 250, 250, 350),
              "ui_anchored_in rejects a drag that started elsewhere");
        check(ui_anchored_in(&probe, 0, 0, 367, 447),
              "  and accepts one that began and ended inside");
    }

    /* ---- long press fires ONCE, while still down ---- */
    reset();
    stub_touch_set(1, 10, 10, 5); ui_poll(on_event);
    stub_advance_ms(UI_LONG_MS + 50u);
    ui_poll(on_event);
    check(n_long == 1 && n_release == 0,
          "LONG fires while the finger is still down");
    ui_poll(on_event); ui_poll(on_event);
    check(n_long == 1, "  and only once, however many polls follow");

    /* ---- contacts are matched by tracking id, not slot ---- */
    reset();
    stub_touch_set(1, 10, 10, 9);  ui_poll(on_event);
    stub_touch_set(1, 12, 12, 9);  ui_poll(on_event);
    check(n_press == 1, "the same id across frames is ONE contact, not two");
    stub_touch_set(1, 300, 300, 4); ui_poll(on_event);
    check(n_press == 2 && n_release == 1,
          "a different id is a new contact and retires the old one");

    /* ---- a failed controller read releases, but never taps ---- */
    reset();
    stub_touch_set(1, 100, 100, 6); ui_poll(on_event);
    stub_advance_ms(30u);
    stub_touch_fail(1);             ui_poll(on_event);
    stub_touch_fail(0);
    check(n_release == 1, "an I2C failure releases the contact (fail safe)");
    check(n_tap == 0, "  but does NOT fire a tap - a failed read is not a lift");

    /* ---- releasing one of two fingers keeps the RIGHT one on screen ----
     *
     * The demo kept a count and indexed by it: press A, press B, release A, and
     * the count fell to 1 while slot 0 still held A - so the display showed the
     * finger that had LEFT and hid the one still down. A count is not a set.
     * Checked by reading the rendered glyph, not the app's variables. */
    {
        const gfx_font *F = &share_mono_16x32;
        char c1, c2;

        gfx_init();
        ui_init();
        elide_set_resync(0u);
        if (APP.init) APP.init();
        (void)gfx_present();

        stub_touch_set(1, 100, 100, 7);            /* finger 7 down */
        ui_poll(APP.event);
        stub_touch_set2(200, 200, 9);              /* finger 9 down too */
        ui_poll(APP.event);
        APP.frame(0u);
        (void)gfx_present();
        c1 = glyph_at(16, 56, F);
        c2 = glyph_at(16, 96, F);
        check(c1 == '7' && c2 == '9', "two contacts show both ids, in order");

        stub_touch_set(1, 200, 200, 9);            /* finger 7 lifts */
        ui_poll(APP.event);
        APP.frame(1u);
        (void)gfx_present();
        c1 = glyph_at(16, 56, F);
        check(c1 == '9', "releasing the first finger leaves the SECOND on screen");
        /* A cleared region has no ink, which matches the SPACE glyph - the
         * font's only blank one. Not '?': the region is legitimately empty. */
        check(glyph_at(16, 96, F) == ' ', "  and the second line is cleared");
    }

    printf("%s (%d failure%s)\n", g_fails ? "FAILED" : "OK",
           g_fails, g_fails == 1 ? "" : "s");
    return g_fails != 0;
}
