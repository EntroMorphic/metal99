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

    printf("%s (%d failure%s)\n", g_fails ? "FAILED" : "OK",
           g_fails, g_fails == 1 ? "" : "s");
    return g_fails != 0;
}
