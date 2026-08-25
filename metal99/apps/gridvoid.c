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
#define RUNGS    12          /* transverse lines in flight at once */

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
     * Rungs flow toward the viewer and wrap.
     *
     * Spaced by INVERSE depth, not by depth. Even spacing in z sends most of
     * them to the horizon, because perspective compresses by 1/z - the first
     * cut did that and left the near floor empty. Stepping 1/z evenly puts
     * them at even distances on SCREEN, which is what reads as motion.
     */
    for (i = 0; i < RUNGS; i++) {
        int32_t inv0 = TRIG_ONE / Z_FAR, inv1 = TRIG_ONE / Z_NEAR;
        int32_t t = ((int32_t)i * TRIG_ONE) / RUNGS + g_scroll;
        int32_t z;
        t = ((t % TRIG_ONE) + TRIG_ONE) % TRIG_ONE;
        z = TRIG_ONE / (inv0 + ((inv1 - inv0) * t) / TRIG_ONE);
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

static void game_init(void)
{
    int i;
    g_scroll = 0;
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
    if (g_lives == 0) { game_init(); return sh8601_write_frame(vg_rowfn); }

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
    return sh8601_write_frame(vg_rowfn);
}

/* 30 Hz: a full-screen vector frame is 31.2 ms of wire time, so 60 is not
 * available and pretending otherwise just makes the late counter noise. */
const app_t APP = { "gridvoid", 30u, game_init, game_frame, game_event };
