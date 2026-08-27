/*
 * GRID VOID - a wireframe vector game.
 *
 * Ported from the game hidden in inlikeflynn.io (Konami code, or type FLYNN),
 * which runs on three.js. Not ported line for line: three.js is 594 KB and
 * wants a JavaScript engine, WebGL and a GPU, against a 19 KB firmware with no
 * libc. What carries over is the game - a Tron grid receding into fog,
 * wireframe craft rising out of it, tracers streaking to the vanishing point -
 * and that is a VECTOR game, which is what this hardware can actually do well.
 *
 * PERSPECTIVE, in integers. A point at world (x, y, z) with the camera at
 * height CAM_Y looking along +z lands at
 *
 *     sx = CX      + (x * FOCAL) / z
 *     sy = HORIZON + ((CAM_Y - y) * FOCAL) / z
 *
 * One divide per point, no floating point, and z never reaches zero because
 * nothing is drawn nearer than Z_NEAR. Depth is what makes a flat grid read as
 * a floor: the same divide that shrinks distant things also drags them toward
 * the horizon.
 */
#include <stddef.h>
#include "app.h"
#include "vg.h"
#include "tile.h"
#include "sfx.h"
#include "io.h"
#include "trig.h"
#include "sh8601.h"
#include "spi2.h"

#define CX       (SH8601_WIDTH / 2)
#define HORIZON  150
#define FOCAL    260
#define CAM_Y    48

#define Z_NEAR   40
#define Z_FAR    560         /* the fog line. Further than this and rungs pile
                              * onto the horizon: perspective crowds them by 1/z,
                              * so a deep range spends most of its lines in the
                              * top few rows and leaves the floor bare. */
#define CELL     52          /* world units between grid lines */
#define LANES    8           /* longitudinal lines each side of centre */
/*
 * 20, and the count is a picture decision rather than a budget one.
 *
 * Rungs are evenly spaced in the WORLD, so the near ones are always sparse on
 * screen - that is perspective working. With 12 the nearest landed at z=87,
 * two thirds down the panel, and the bottom third had no floor at all. 20 puts
 * the nearest at Z_NEAR itself, which projects BELOW the panel and is clipped,
 * so the floor runs off the bottom edge the way a floor should.
 */
#define RUNGS    20          /* transverse lines in flight at once */

#define MAX_FOE  8
#define FOE_R    26          /* world half-size of a craft            */
#define HIT_PX   34          /* tap this near a craft's centre to hit  */
#define TRACER_F 6           /* frames a tracer stays lit              */

/* Colours through sh8601_rgb565, never hand-written hex: the panel takes RGB565
 * BYTE-SWAPPED, so a literal that looks like cyan is not. The first cut used
 * raw constants and rendered the whole void in purple. */
static uint16_t C_GRID, C_HOT;

static int32_t g_scroll;     /* how far the grid has flowed toward us     */

typedef struct { int32_t x, y, z; int alive, spin; } foe;
static foe      g_foe[MAX_FOE];
static int      g_score, g_lives, g_wave, g_combo;
static int      g_tracer, g_tx, g_ty;      /* live tracer, and where it went */
static uint32_t g_seed = 0x1234567u;
static int      g_spawn;

/* xorshift: a game needs unpredictable spawns, not good randomness, and this
 * is three instructions with no state beyond a word. */
static uint32_t rnd(void)
{
    g_seed ^= g_seed << 13; g_seed ^= g_seed >> 17; g_seed ^= g_seed << 5;
    return g_seed;
}

static int projx(int x, int z) { return CX + (x * FOCAL) / z; }
static int projy(int y, int z) { return HORIZON + ((CAM_Y - y) * FOCAL) / z; }

/* One grid line along z, at world x. Drawn as a few chords rather than one
 * segment: perspective is not linear in z, so a single straight line from near
 * to far would bow away from where the rungs actually cross it. */
static void lane(int x)
{
    int i, z0 = Z_NEAR, steps = 6;
    for (i = 1; i <= steps; i++) {
        int z1 = Z_NEAR + ((Z_FAR - Z_NEAR) * i * i) / (steps * steps);
        vg_line(projx(x, z0), projy(0, z0), projx(x, z1), projy(0, z1), C_GRID);
        z0 = z1;
    }
}

static void grid(void)
{
    int k, i;
    for (k = -LANES; k <= LANES; k++) lane(k * CELL);

    /*
     * Rungs flow toward the viewer and wrap, EVENLY SPACED IN THE WORLD.
     *
     * That is the whole of perspective here: projy is HORIZON + k/z, so equal
     * steps in z come out bunched near the horizon and spread apart near the
     * viewer. Gaps measured across the screen run 2, 2, 2, 4, 5, 7, 9, 15, 24,
     * 47 pixels - which is what a receding floor looks like.
     *
     * THIS FILE USED TO STEP 1/z EVENLY, and said so in a comment that
     * described the bug as the intent: "stepping 1/z evenly puts them at even
     * distances on SCREEN, which is what reads as motion". Even screen spacing
     * is a ladder, not a floor - every gap measured 22 to 26 pixels, top to
     * bottom, with no depth cue at all.
     *
     * The reason it got there is recorded too: an earlier cut spaced them in z,
     * found the near floor sparse, and flattened the perspective to fill it.
     * Sparse near-field IS what perspective does. The fix for a thin floor is
     * more rungs, not less depth - RUNGS is the knob.
     */
    for (i = 0; i < RUNGS; i++) {
        int32_t span = (int32_t)(Z_FAR - Z_NEAR);
        int32_t step = span / RUNGS;
        int32_t off  = (int32_t)(((g_scroll % TRIG_ONE) * step) / TRIG_ONE);
        /* Subtract the scroll so rungs travel TOWARD the viewer. Modulo of a
         * value that can go negative is normalised, not left to chance. */
        int32_t p    = (((int32_t)i * step - off) % span + span) % span;
        int32_t z    = (int32_t)Z_NEAR + p;
        if (z < Z_NEAR) z = Z_NEAR;
        if (z > Z_FAR)  z = Z_FAR;
        vg_line(projx(-LANES * CELL, (int)z), projy(0, (int)z),
                projx( LANES * CELL, (int)z), projy(0, (int)z), C_GRID);
    }
}

/*
 * VECTOR DIGITS. Seven segments, drawn as strokes.
 *
 * The bitmap font exists and is good, but it belongs to gfx's run model and
 * this scene never touches gfx. Drawing the HUD as line segments keeps one
 * renderer for the whole frame - and stroked numerals are what a vector
 * machine actually did, so it looks right rather than looking like a bitmap
 * pasted over a vector scene.
 *
 *      --0--        segment order: top, upper-left, upper-right,
 *     1|   |2        middle, lower-left, lower-right, bottom
 *      --3--
 *     4|   |5
 *      --6--
 */
static const uint8_t SEG7[10] = {
    0x77u, 0x24u, 0x5Du, 0x6Du, 0x2Eu, 0x6Bu, 0x7Bu, 0x25u, 0x7Fu, 0x6Fu
};

static void digit(int d, int x, int y, int w, int h, uint16_t c)
{
    int m = y + h / 2, r = x + w, b = y + h;
    uint8_t s = SEG7[d % 10];
    if (s & 0x01u) vg_line(x, y, r, y, c);
    if (s & 0x02u) vg_line(x, y, x, m, c);
    if (s & 0x04u) vg_line(r, y, r, m, c);
    if (s & 0x08u) vg_line(x, m, r, m, c);
    if (s & 0x10u) vg_line(x, m, x, b, c);
    if (s & 0x20u) vg_line(r, m, r, b, c);
    if (s & 0x40u) vg_line(x, b, r, b, c);
}

static void number(int v, int x, int y, int w, int h, int digits, uint16_t c)
{
    int i, div = 1;
    for (i = 1; i < digits; i++) div *= 10;
    for (i = 0; i < digits; i++) { digit((v / div) % 10, x, y, w, h, c);
                                  x += w + 6; div /= 10; }
}

/*
 * SHATTER. A craft that simply disappears gives the player nothing back - the
 * shot and the kill need to be the same event on screen. Shards fly outward
 * and fade by shortening, which is cheap and reads instantly.
 */
#define MAX_BURST 4
#define BURST_F   9
typedef struct { int x, y, r, age; } burst_t;
static burst_t g_burst[MAX_BURST];

static void burst(int x, int y, int r)
{
    int i, oldest = 0;
    for (i = 0; i < MAX_BURST; i++) {
        if (g_burst[i].age <= 0) { oldest = i; break; }
        if (g_burst[i].age > g_burst[oldest].age) oldest = i;
    }
    g_burst[oldest].x = x; g_burst[oldest].y = y;
    g_burst[oldest].r = (r < 8 ? 8 : r); g_burst[oldest].age = BURST_F;
}

static void draw_bursts(void)
{
    int i, k;
    for (i = 0; i < MAX_BURST; i++) {
        int age = g_burst[i].age;
        if (age <= 0) continue;
        for (k = 0; k < 6; k++) {
            int a  = (k * (TRIG_FULL / 6)) + age * 5;
            int r0 = g_burst[i].r + (BURST_F - age) * 6;
            int r1 = r0 + age;                     /* shard shortens as it ages */
            vg_line(g_burst[i].x + (int)((icos(a) * r0) >> 16),
                    g_burst[i].y + (int)((isin(a) * r0) >> 16),
                    g_burst[i].x + (int)((icos(a) * r1) >> 16),
                    g_burst[i].y + (int)((isin(a) * r1) >> 16), C_HOT);
        }
        g_burst[i].age--;
    }
}

static void spawn(void)
{
    int i;
    for (i = 0; i < MAX_FOE; i++) if (!g_foe[i].alive) {
        g_foe[i].x = (int32_t)(rnd() % (uint32_t)(LANES * CELL)) - (LANES * CELL) / 2;
        /*
         * Height bounded so a craft is still ON SCREEN when it arrives.
         *
         * projy = HORIZON + (CAM_Y - y) * FOCAL / z, so anything above eye
         * level climbs as it nears: spawned at y=80 a craft sits at screen -53
         * by the time it breaches - off the top, unhittable, and it still takes
         * a life. Losing to something you cannot see or shoot is not
         * difficulty. [8, 52] keeps both extremes inside the panel at Z_NEAR.
         */
        g_foe[i].y = (int32_t)(rnd() % 45u) + 8;
        g_foe[i].z = Z_FAR;
        g_foe[i].spin = (int)(rnd() & 255u);
        g_foe[i].alive = 1;
        return;
    }
}

static int g_audio_ready;

static void game_init(void)
{
    int i;
    g_scroll = 0;
    /*
     * ONCE. game_init also runs on death, and bringing the codec up again
     * mid-game would stop the DMA ring, re-run 29 I2C writes and pop the
     * amplifier - all to arrive back where it already was.
     */
    if (!g_audio_ready) {
        int arc = sfx_init();
        g_audio_ready = (arc == 0);
        con_puts("  sfx_init rc="); con_dec((int32_t)arc); con_puts("\r\n");
    }

    C_GRID = sh8601_rgb565(0, 110, 170);
    C_HOT  = sh8601_rgb565(90, 230, 255);
    for (i = 0; i < MAX_FOE; i++) g_foe[i].alive = 0;
    for (i = 0; i < MAX_BURST; i++) g_burst[i].age = 0;
    g_score = 0; g_lives = 3; g_wave = 1; g_combo = 0;
    g_tracer = 0; g_spawn = 40;
}

/* A craft: a spinning wireframe diamond. Cheap, reads clearly at any depth,
 * and the spin is what makes it look alive rather than pasted on. */
static void craft(const foe *e)
{
    int cx = projx((int)e->x, (int)e->z);
    int cy = projy((int)e->y, (int)e->z);
    int r  = (FOE_R * FOCAL) / (int)e->z;
    int a  = e->spin, i;
    int px = 0, py = 0, fx = 0, fy = 0;

    if (r < 2) r = 2;
    for (i = 0; i <= 4; i++) {
        int ang = a + i * (TRIG_FULL / 4);
        int x = cx + (int)((icos(ang) * r) >> 16);
        int y = cy + (int)((isin(ang) * (r / 2)) >> 16);
        if (i == 0) { fx = x; fy = y; } else vg_line(px, py, x, y, C_HOT);
        px = x; py = y;
    }
    (void)fx; (void)fy;
    /* Cross through the CENTRE, not through a vertex - hung off the first
     * vertex it swung around with the spin and read as a glitch rather than a
     * craft. A bare diamond reads as a blob; the cross gives it a nose. */
    vg_line(cx, cy - r, cx, cy + r, C_HOT);
    vg_line(cx - r, cy, cx + r, cy, C_HOT);
}

static void game_event(const ui_event *e)
{
    int i, best = -1, bestd = 0x7FFFFFF;

    /* PRESS, not TAP: a shooter must fire the instant a finger lands. Waiting
     * for TAP would add the whole tap window - up to UI_TAP_MS - between aiming
     * and firing, and would swallow the shot entirely if the finger slid. */
    if (e->kind != UI_PRESS) return;

    g_tracer = TRACER_F; g_tx = (int)e->x; g_ty = (int)e->y;
    sfx_play(SFX_FIRE);

    /*
     * Nearest craft to the shot, in SCREEN space - the player aims at what they
     * can see, not at world coordinates.
     *
     * The hit radius is the LARGER of a minimum and the craft's drawn radius.
     * A flat radius was smaller than the craft up close: at z=80 a craft draws
     * 84 px across and the box was 34, so a tap landing squarely on it missed.
     * If it looks hit, it is hit.
     */
    for (i = 0; i < MAX_FOE; i++) {
        int dx, dy, d, r, lim;
        if (!g_foe[i].alive) continue;
        dx = projx((int)g_foe[i].x, (int)g_foe[i].z) - g_tx;
        dy = projy((int)g_foe[i].y, (int)g_foe[i].z) - g_ty;
        d  = dx * dx + dy * dy;
        r  = (FOE_R * FOCAL) / (int)g_foe[i].z;
        lim = (r > HIT_PX ? r : HIT_PX);
        if (d <= lim * lim && d < bestd) { bestd = d; best = i; }
    }

    if (best >= 0) {
        burst(projx((int)g_foe[best].x, (int)g_foe[best].z),
              projy((int)g_foe[best].y, (int)g_foe[best].z),
              (FOE_R * FOCAL) / (int)g_foe[best].z);
        g_foe[best].alive = 0;
        g_combo++;
        /* Nearer craft are worth less: letting one close is the easy shot. */
        g_score += 10 * g_combo;
        sfx_play(SFX_KILL);
    } else {
        g_combo = 0;                            /* a miss breaks the chain */
    }
}

static int game_frame(uint32_t f)
{
    int i;

    /* Wrapped, not left to overflow. The modulo below copes with a negative
     * g_scroll, but a counter that silently goes negative after 27 hours is a
     * thing to reason about at 3am rather than prevent here. */
    g_scroll = (g_scroll + TRIG_ONE / 90) % TRIG_ONE;

    /* ---- advance the world ---- */
    /* Spawn cadence against crossing time: a craft takes (Z_FAR-Z_NEAR)/speed
     * frames to arrive, so this keeps roughly three or four in flight and
     * tightens as waves climb. Sparse fields are not difficulty, they are
     * waiting. */
    if (--g_spawn <= 0) { spawn(); g_spawn = 44 - (g_wave * 3);
                          if (g_spawn < 16) g_spawn = 16; }
    for (i = 0; i < MAX_FOE; i++) {
        if (!g_foe[i].alive) continue;
        g_foe[i].z -= 2 + g_wave;
        g_foe[i].spin = (g_foe[i].spin + 3) & (TRIG_FULL - 1);
        if (g_foe[i].z <= Z_NEAR) {             /* breach */
            g_foe[i].alive = 0;
            g_combo = 0;
            if (g_lives > 0) g_lives--;
        }
    }
    /*
     * Restart AFTER the loop, never inside it.
     *
     * game_init() clears g_foe, g_lives and g_wave - and calling it from within
     * the loop that is walking g_foe left the walk iterating freshly reset
     * entries with a stale index, then the wave check below ran against a
     * half-reset world. A function that reinitialises a structure must not be
     * called by the loop that owns it.
     */
    /*
     * RESTART, THEN FALL THROUGH AND DRAW. It used to `return vg_present()`
     * here, which presented without vg_begin/vg_finish - so the walk from the
     * PREVIOUS frame was still finished, every row took vg_rowfn's
     * already-past early return, and the restart frame went out as pure
     * background. A blank frame between death and the new grid, on every
     * death. Presenting a scene nobody drew is not a present; it is a clear.
     */
    if (g_lives == 0) game_init();

    if ((f % 900u) == 899u && g_wave < 9) g_wave++;
    if (g_tracer > 0) g_tracer--;

    /* ---- draw ---- */
    vg_set_bg(0x0000u);
    vg_begin();
    grid();
    for (i = 0; i < MAX_FOE; i++) if (g_foe[i].alive) craft(&g_foe[i]);
    draw_bursts();

    /* Tracer: from the gun at the bottom centre to where the shot landed. */
    if (g_tracer > 0)
        vg_line(CX, SH8601_HEIGHT - 1, g_tx, g_ty, C_HOT);

    /* Reticle. Four ticks, not a cross - a solid cross over a vanishing point
     * reads as part of the grid. */
    { int cx = CX, cy = HORIZON + 120, r = 14, g = 5;
      vg_line(cx - r, cy, cx - g, cy, C_HOT);
      vg_line(cx + g, cy, cx + r, cy, C_HOT);
      vg_line(cx, cy - r, cx, cy - g, C_HOT);
      vg_line(cx, cy + g, cx, cy + r, C_HOT); }

    /* ---- HUD ---- */
    number(g_score, 12, 10, 12, 20, 5, C_HOT);
    for (i = 0; i < g_lives; i++)
        vg_line(SH8601_WIDTH - 14 - i * 14, 12, SH8601_WIDTH - 8 - i * 14, 30, C_HOT);
    number(g_wave, SH8601_WIDTH - 30, 40, 10, 16, 1, C_GRID);

    vg_finish();
    /*
     * TILE-GRANULAR PRESENT. Measured on this exact scene, on hardware:
     *
     *     full repaint         24.5 ms   100%  of pixels,  1 span
     *     vg_present (rows)    18.9 ms   88.7% of pixels,  1.6 spans
     *     tile_present 8x8     15.5 ms   27.6% of pixels, 29 spans
     *                          (20.9 ms worst over a 60 s soak)
     *
     * Spans got 29x more numerous and cost almost nothing: 11.8 us each,
     * measured by sending the same 448 rows split 1 to 64 ways
     * (apps/spancost.c). Pixels are what cost.
     *
     * AND THAT LAST LINE UNDOES A THEORY THIS FILE USED TO STATE. vg_present
     * needed VG_MIN_RUN padding because short spans put debris on the glass -
     * measured, reproducible, and fixed by making spans taller. tile_present
     * runs 29 spans of EIGHT rows and is clean. Both were observed on this
     * panel, and no account of the transport explains both. So the padding
     * constants on the vg path encode a belief that is now known to be
     * incomplete, and nobody should build on either one. See DESIGN.md 11.4.
     */
    /*
     * Refill the audio ring BEFORE presenting. Presenting is the long pole -
     * 15 ms of transmit - and the DMA keeps reading throughout it. Servicing
     * after would leave the mixer a shrinking slice of the frame to work in,
     * and an underrun is audible where a late refill is not.
     */
    sfx_service();
    return tile_present(vg_rowfn);
}

/*
 * 36 Hz, through elision - and the honest accounting of why it is not more.
 *
 * A frame now sends only the rows carrying something this frame or last (see
 * vg_present): 21% fewer rows over a 6000-frame soak, and a typical frame
 * falls from 24.3 ms to ~19 ms. The worst frame does not move. It cannot: when
 * the scene changes enough that this frame's rows and last frame's rows
 * together cover the screen, the work IS a full repaint, plus what elision
 * costs to decide that. Measured at ~25.5 ms against 24.3 before.
 *
 * A fixed cadence has to fit the WORST frame, not the typical one, so 40 Hz
 * held before elision only because every frame was uniformly expensive, and
 * 36 is what holds now with late=0 over 65 s. That is the trade taken with
 * eyes open: a slightly slower guaranteed cadence, in exchange for a fifth of
 * the pixels never leaving the chip and a duty cycle that drops from 97% to
 * about 75% on typical frames - which is the only thing available to reduce
 * the beam-crossing that makes moving wireframes shimmer, since this board
 * routes no TE line (apps/tescan.c).
 *
 * WHAT WOULD ACTUALLY RAISE IT is elide_mark_rect instead of elide_mark.
 * vg marks full-width rows; above the horizon a craft occupies a handful of
 * columns and the rest of that row is sent for nothing. Narrow marks would cut
 * the worst case directly. They were left out deliberately: mixed x-extents
 * stop elide coalescing, split one span into several, and every extra boundary
 * is another chance for the leak in spi2.h - which is the bug that cost this
 * project weeks. Measure that before trading it away.
 *
 * Bridging short gaps between marked runs was tried, to spend rows saving span
 * setups. 3.1 spans per frame with it and without; worst frame unchanged. It
 * was removed rather than kept as a plausible story with no effect.
 */
const app_t APP = { "gridvoid", 40u, game_init, game_frame, game_event };
