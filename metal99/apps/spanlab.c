/*
 * SPANLAB - a reproduction of the span-boundary debris that holds still.
 *
 * Every previous encounter with this defect was entangled with something else:
 * sub-width marking, text updates, motion, touch. That is why it has survived
 * so long - and why one look at a stale flash was enough to convince me it was
 * fixed. An intermittent bug observed through a panel that cannot be read back
 * is not evidence, it is an anecdote.
 *
 * So: a STATIC frame, drawn as many deliberately separate spans, in colours
 * chosen so that a single leaked pixel is obvious. Nothing moves. Nothing is
 * elided. If debris appears it stays on the glass until the next repaint, and
 * it is in a known place with a known cause.
 *
 * WHAT IT TESTS. The app cycles configurations and reports which one is
 * running, so a single flash answers several questions instead of one:
 *
 * ROUND 1 asked whether span count alone did it: FIFO back-to-back, FIFO with
 * 20 us and 200 us between spans, and the DMA path. All four clean. So spans
 * are not sufficient, and the AFIFO, CS settling time and the FIFO data path
 * are all retired as lone suspects.
 *
 * ROUND 2 goes after the variable every single sighting of this bug has had in
 * common, which I overlooked because span count also correlated: TOUCH. Every
 * report, going back months - the box leaving trails, the text over the orange
 * bar, "artifacts only appear when the orange bar appears WHILE I am touching",
 * and "scrolling without me touching... perfect" - involved a finger on the
 * glass. spanlab round 1 had no touch and was clean.
 *
 * So the modes now add ingredients one at a time, on top of the same static,
 * known-clean 15-span frame:
 *
 * ROUND 3. Rounds 1 and 2 retired, between them: span count, CS settling time,
 * the AFIFO, the FIFO data path, window churn, and touch polling. 23 spans with
 * addresses moving every frame, clean. Meanwhile demo and gridvoid-through-
 * elision both reproduce it. So the fault is in what those do that this does
 * not, and after eliminating the transport there are only three things left.
 *
 * ROUND 4. Rounds 1-3 retired span count, CS settling, the AFIFO, the FIFO
 * data path, window churn, touch polling, the vector rowfn, changing content,
 * and vg-through-elision itself - mode 4 of round 3 ran the real renderer and
 * the real present path and stayed clean.
 *
 * Which leaves the one dimension this harness has never varied, and it is
 * embarrassing in hindsight: IDLE TIME. Every round so far ran at 20 Hz with a
 * ~25 ms frame, so the bus went quiet for ~25 ms between frames. gridvoid at
 * 40 Hz has ~0.5 ms. A fiftyfold difference in how long the panel gets to
 * recover, and spanlab has been sitting at the comfortable end of it the whole
 * time, which would explain every clean result including the ones that made me
 * retire suspects.
 *
 * It also fits every sighting. The orange bar is a big update; a finger adds
 * more; together they collapse the gap. "Artifacts only appear when the orange
 * bar appears WHILE I am touching" is a duty-cycle statement, not a graphics
 * one, and I have been reading it as a graphics one for a long time.
 *
 * So the modes now differ in ONE thing - how long the bus rests afterwards:
 *
 *   0  40 ms idle    deeply comfortable, ~15 Hz
 *   1  20 ms idle
 *   2   5 ms idle
 *   3   0 ms idle    back to back, as hard as this panel can be driven
 *
 * Identical frames, identical spans, identical everything else. If 4 is dirty
 * and 1 is clean, the defect is a rate limit we have been exceeding, and the
 * fix is a floor on the gap rather than anything in the graphics stack.
 *
 * THE PICTURE MUST NOT MOVE, in any mode. An earlier version shifted the band
 * boundaries themselves, which made modes 3 and 4 shake like an earthquake and
 * made leftover pixels indistinguishable from intended motion - a confound of
 * my own manufacture, in a harness whose entire job is to remove them. Now
 * every mode paints the identical frame, and ANY visible change is the defect.
 *
 * Whichever block number first shows colour in a black gap row is the
 * ingredient. If 1 is dirty and 2 is clean, this was never a graphics bug.
 *
 * The mode is shown as that many white blocks down the left edge, drawn in the
 * same frame - no font, no gfx, nothing that could itself be the bug.
 *
 * APP=spanlab ./metal99/build.sh
 */
#include <stdint.h>
#include "app.h"
#include "io.h"
#include "sh8601.h"
#include "spi2.h"
#include "touch.h"
#include "vec.h"
#include "vg.h"

#define W SH8601_WIDTH
#define H SH8601_HEIGHT

#define BANDS      8
#define BAND_ROWS  50            /* 8 x 50 = 400 rows, + gaps, fits in 448   */
#define GAP_ROWS   1             /* keeps adjacent bands from being one span */

#define MODES        4
#define APP_HZ       60u         /* 407 full-width rows is ~15 ms of wire at
                                    80 MHz; 20 Hz leaves room and this test
                                    does not care about cadence             */
#define MODE_FRAMES  60u   /* frames per mode; rate varies by design */

static uint16_t g_col[BANDS];
static touch_state g_ts;
static int         g_vector_fill;   /* rowfn writes through the PIE unit  */
static uint32_t    g_rot;           /* colour rotation, modes 2 and 3     */
static uint32_t    g_shift;      /* rotates band boundaries in modes 2/3 */
static uint16_t g_mode;
static uint16_t g_band_colour;   /* what the current span is painting        */
static int      g_show_mode;     /* draw the mode blocks on this row?        */

/*
 * Saturated, maximally distinguishable, and NEVER black: black debris on black
 * is invisible, and every one of these bleeding into any other is obvious at a
 * glance. Alternating hot/cold also means a single leaked byte changes both
 * the red and blue channels, so it cannot hide in a rounding error.
 */
static void colours(void)
{
    g_col[0] = sh8601_rgb565(255,  40,   0);   /* orange - the historical
                                                  offender in the band test */
    g_col[1] = sh8601_rgb565(  0, 255,  60);   /* green   */
    g_col[2] = sh8601_rgb565(  0,  80, 255);   /* blue    */
    g_col[3] = sh8601_rgb565(255, 255,   0);   /* yellow  */
    g_col[4] = sh8601_rgb565(255,   0, 200);   /* magenta */
    g_col[5] = sh8601_rgb565(  0, 255, 255);   /* cyan    */
    g_col[6] = sh8601_rgb565(255, 255, 255);   /* white   */
    g_col[7] = sh8601_rgb565(120,   0, 255);   /* violet  */
}

/* One flat colour, plus the mode indicator where asked. Deliberately trivial:
 * anything clever here could be the source of a defect and confuse the result. */
static void band_row(uint16_t *row, int y)
{
    int x;
    (void)y;
    if (g_vector_fill) vec_fill16(row, g_band_colour, (uint32_t)(W * 2 / VEC_BYTES));
    else for (x = 0; x < W; x++) row[x] = g_band_colour;
    if (g_show_mode) {
        int m, blocks = (int)g_mode + 1;
        for (m = 0; m < blocks; m++) {
            int x0 = 8 + m * 24;
            for (x = x0; x < x0 + 16 && x < W; x++) row[x] = 0xFFFFu;
        }
    }
}

static void black_row(uint16_t *row, int y)
{
    int x;
    (void)y;
    for (x = 0; x < W; x++) row[x] = 0x0000u;
}

static void lab_init(void)
{
    colours();
    g_mode = 0u;
    g_shift = 0u;
    con_puts("spanlab r4: idle 40ms / 20ms / 5ms / 0ms - all else equal\r\n");
}

static int lab_frame(uint32_t f)
{
    int b, rc, y = 0;
    uint32_t mode = (f / MODE_FRAMES) % MODES;
    static const uint32_t IDLE_MS[MODES] = { 40u, 20u, 5u, 0u };
    int touching = 1;          /* constant across modes: not the variable now */
    int rotate   = 1;          /* content must change or staleness is invisible */
    int moving   = 0;
    g_vector_fill = 1;

    if (mode != g_mode) {
        g_mode = (uint16_t)mode;
        con_puts("spanlab mode "); con_dec((int32_t)mode); con_puts("\r\n");
    }
    sh8601_set_dma(0);

    /*
     * Poll the touch controller and THROW THE RESULT AWAY. The question is not
     * what the finger did, it is whether the I2C transaction itself perturbs
     * anything the panel depends on. Nothing here draws from it.
     */
    if (touching) (void)touch_poll(&g_ts);

    /* Move every band boundary by a few rows each frame, so the window
     * commands differ frame to frame instead of repeating identically. Same
     * span COUNT, same extents, different addresses. */
    (void)moving;
    g_rot = rotate ? ((g_rot + 1u) % BANDS) : 0u;
    g_shift = 0u;
    y = 0;



    for (b = 0; b < BANDS; b++) {
        int y0 = y, y1 = y + BAND_ROWS - 1;
        if (y1 > H - 1) break;

        g_band_colour = g_col[(b + (int)g_rot) % BANDS];
        g_show_mode   = (b == 0);        /* indicator lives in the first band */

        /* Full width, so elide/DMA eligibility is not the variable under test -
         * only how many spans there are and where their windows land. */
        if (moving) {
            int cut = y0 + (int)g_shift;
            rc = sh8601_write_span_x(0u, (uint16_t)y0, (uint16_t)(W - 1),
                                     (uint16_t)(cut - 1), band_row);
            if (rc != SPI2_OK) return rc;
            rc = sh8601_write_span_x(0u, (uint16_t)cut, (uint16_t)(W - 1),
                                     (uint16_t)y1, band_row);
            if (rc != SPI2_OK) return rc;
        } else {
            rc = sh8601_write_span_x(0u, (uint16_t)y0, (uint16_t)(W - 1),
                                     (uint16_t)y1, band_row);
            if (rc != SPI2_OK) return rc;
        }

        y = y1 + 1;

        /* The gap row is its own span, black, and it is where bleed shows up
         * most clearly: any colour here came from a neighbour. */
        if (y <= H - 1 && b != BANDS - 1) {
            rc = sh8601_write_span_x(0u, (uint16_t)y, (uint16_t)(W - 1),
                                     (uint16_t)y, black_row);
            if (rc != SPI2_OK) return rc;
            y += GAP_ROWS;
        }

    }

    /* Everything below the last band, so the frame is fully defined. */
    if (y <= H - 1) {
        rc = sh8601_write_span_x(0u, (uint16_t)y, (uint16_t)(W - 1),
                                 (uint16_t)(H - 1), black_row);
        if (rc != SPI2_OK) return rc;
    }

    /* THE VARIABLE. Everything above this line is identical in all four modes. */
    if (IDLE_MS[mode]) delay_ms(IDLE_MS[mode]);
    return SPI2_OK;
}

const app_t APP = { "spanlab", (uint16_t)APP_HZ, lab_init, lab_frame, 0 };
