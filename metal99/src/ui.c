#include <stddef.h>
#include "ui.h"
#include "io.h"

/*
 * One tracked contact. Slots are matched to contacts by the controller's
 * tracking id, NOT by position in the report: two fingers crossing over each
 * other keep their ids but can swap array slots, and matching by slot would
 * hand an application a drag that teleports.
 */
typedef struct {
    int      down;
    uint8_t  id;
    uint16_t ax, ay;      /* anchor */
    uint16_t x, y;        /* last seen */
    uint32_t t0;          /* cpu_cycles at press */
    int      longsent;
} slot;

static slot g_slot[TOUCH_MAX_POINTS];

void ui_init(void)
{
    int i;
    for (i = 0; i < TOUCH_MAX_POINTS; i++) g_slot[i].down = 0;
}

static uint32_t ms_since(uint32_t t0)
{
    /* Unsigned subtraction is correct across the ccount wrap for any interval
     * shorter than ~27 s at 160 MHz. Every interval here is under a second. */
    uint32_t per_ms = CPU_HZ / 1000u;
    return per_ms ? ((cpu_cycles() - t0) / per_ms) : 0u;
}

static int idist(int a, int b) { int d = a - b; return d < 0 ? -d : d; }

static void emit(void (*cb)(const ui_event *), const slot *s, ui_kind k)
{
    ui_event e;
    if (cb == NULL) return;
    e.kind = (uint8_t)k;
    e.id   = s->id;
    e.x    = s->x;  e.y  = s->y;
    e.ax   = s->ax; e.ay = s->ay;
    e.ms   = ms_since(s->t0);
    cb(&e);
}

void ui_poll(void (*cb)(const ui_event *e))
{
    touch_state ts;
    int seen[TOUCH_MAX_POINTS];
    int i, j, ok;

    for (i = 0; i < TOUCH_MAX_POINTS; i++) seen[i] = 0;

    /*
     * A FAILED READ IS NOT A LIFT.
     *
     * Treating an I2C error as "no contacts" releases everything, which is the
     * right fail-safe - a phantom finger stuck down is worse. But the release
     * path also decides whether something was a TAP, and a transient glitch
     * while a finger rested briefly would then fire a tap the user never made,
     * activating a button. Release yes; tap no. We know the contact is gone,
     * we do not know it was lifted.
     */
    ok = (touch_poll(&ts) == TOUCH_OK);
    if (!ok) ts.n = 0u;

    /* Contacts still down: match by id, then PRESS / DRAG / LONG. */
    for (i = 0; i < (int)ts.n; i++) {
        slot *s = NULL;
        for (j = 0; j < TOUCH_MAX_POINTS; j++)
            if (g_slot[j].down && g_slot[j].id == ts.p[i].id) { s = &g_slot[j]; break; }

        if (s == NULL) {                       /* new contact */
            for (j = 0; j < TOUCH_MAX_POINTS; j++)
                if (!g_slot[j].down) { s = &g_slot[j]; break; }
            if (s == NULL) continue;           /* more contacts than slots */
            s->down = 1; s->id = ts.p[i].id;
            s->ax = s->x = ts.p[i].x;
            s->ay = s->y = ts.p[i].y;
            s->t0 = cpu_cycles();
            s->longsent = 0;
            seen[(int)(s - g_slot)] = 1;
            emit(cb, s, UI_PRESS);
            continue;
        }

        seen[(int)(s - g_slot)] = 1;
        if (ts.p[i].x != s->x || ts.p[i].y != s->y) {
            s->x = ts.p[i].x; s->y = ts.p[i].y;
            emit(cb, s, UI_DRAG);
        }
        /* Once, while still down - a long press that only arrived on release
         * would be useless for the thing long presses are usually for. */
        if (!s->longsent && ms_since(s->t0) >= UI_LONG_MS) {
            s->longsent = 1;
            emit(cb, s, UI_LONG);
        }
    }

    /* Contacts that went away: RELEASE, then TAP if it qualified. */
    for (j = 0; j < TOUCH_MAX_POINTS; j++) {
        slot *s = &g_slot[j];
        if (!s->down || seen[j]) continue;
        emit(cb, s, UI_RELEASE);
        if (ok &&
            ms_since(s->t0) <= UI_TAP_MS &&
            idist((int)s->x, (int)s->ax) <= UI_TAP_SLOP &&
            idist((int)s->y, (int)s->ay) <= UI_TAP_SLOP) {
            emit(cb, s, UI_TAP);
        }
        s->down = 0;
    }
}

int ui_in_rect(const ui_event *e, int x0, int y0, int x1, int y1)
{
    return (int)e->x >= x0 && (int)e->x <= x1 &&
           (int)e->y >= y0 && (int)e->y <= y1;
}

int ui_anchored_in(const ui_event *e, int x0, int y0, int x1, int y1)
{
    return ui_in_rect(e, x0, y0, x1, y1) &&
           (int)e->ax >= x0 && (int)e->ax <= x1 &&
           (int)e->ay >= y0 && (int)e->ay <= y1;
}
