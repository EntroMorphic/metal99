/*
 * Input events and hit testing. Pure ISO C99.
 *
 * WHY THIS EXISTS. touch.c reports CONTACTS - where fingers are right now.
 * An interface needs EVENTS - what just happened. The gap between them is not
 * large but it is entirely made of edge cases, and every program would
 * reimplement it and get the same ones wrong:
 *
 *   - a tap is a press AND a release, close together in time AND in space; a
 *     press that wanders 200 px before lifting is a drag, not a tap
 *   - a drag needs the position where the press STARTED, which is gone by the
 *     time the finger has moved
 *   - a long press must fire once, while the finger is still down, not on
 *     release
 *   - contacts must be matched across frames by the controller's tracking id,
 *     not by array position, or two fingers crossing swap identities
 *
 * Written once, here, so an application deals in "the user tapped this
 * rectangle" rather than in coordinates.
 *
 * TIMING comes from cpu_cycles() deltas rather than an absolute millisecond
 * clock. ccount wraps every ~27 s at 160 MHz; unsigned subtraction of two
 * samples is correct across that wrap for any interval shorter than it, and
 * every interval here is well under a second.
 */
#ifndef UI_H
#define UI_H

#include <stdint.h>
#include "touch.h"

/* A press and release within this long, having moved no further than the slop,
 * is a tap. Both bounds matter: time alone calls a fast swipe a tap. */
#define UI_TAP_MS    400u
#define UI_TAP_SLOP  20
#define UI_LONG_MS   600u

typedef enum {
    UI_PRESS = 0,   /* finger down                                        */
    UI_DRAG,        /* moved while down; ax,ay is where it started        */
    UI_RELEASE,     /* finger up - always delivered                       */
    UI_TAP,         /* delivered after RELEASE when it qualified as a tap */
    UI_LONG         /* fired once, while still down                       */
} ui_kind;

typedef struct {
    uint8_t  kind;
    uint8_t  id;      /* controller's tracking id: stable for one contact */
    uint16_t x, y;    /* where the finger is now                          */
    uint16_t ax, ay;  /* anchor - where this contact was first pressed    */
    uint32_t ms;      /* milliseconds since the press                     */
} ui_event;

void ui_init(void);

/*
 * Poll the controller and deliver whatever happened to `cb`. Call once per
 * frame. Costs a GPIO read when nothing is touching (touch.c gates on INT).
 * A NULL callback still advances the state machine.
 */
void ui_poll(void (*cb)(const ui_event *e));

/* Is the event's CURRENT position inside this rectangle? Inclusive. */
int ui_in_rect(const ui_event *e, int x0, int y0, int x1, int y1);

/*
 * Did this event both START and END inside the rectangle? For UI_TAP, this is
 * almost always what a button wants: a press that began on one control and
 * lifted on another should activate neither.
 */
int ui_anchored_in(const ui_event *e, int x0, int y0, int x1, int y1);

#endif /* UI_H */
