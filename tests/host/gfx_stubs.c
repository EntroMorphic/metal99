/*
 * Host stubs for the layers below gfx: elide records what it was told to mark,
 * and captures the rowfn so a test can render a row and assert actual pixels.
 * gfx.c itself is the real firmware source - the point is to test the model,
 * not a reimplementation of it.
 */
#include <stdint.h>
#include <stddef.h>
#include "elide.h"
#include "sh8601.h"
#include "gfx_stubs.h"

mark_t   g_marks[MAX_MARKS];
int      g_nmarks;
static void (*g_rowfn)(uint16_t *, int);

void stub_reset(void) { g_nmarks = 0; }

void stub_render(int y, uint16_t *row)
{
    if (g_rowfn) g_rowfn(row, y);
}

/*
 * STUBBED AT THE TRANSPORT, NOT AT elide.
 *
 * The first version replaced elide_flush() outright, so the tests covered gfx's
 * marking and nothing below it - and elide is where dirty rows become
 * SPANS, where extents get unioned, and where the rolling resync adds rows
 * nobody asked for. A bug in any of those produces exactly the symptom this
 * harness is meant to catch, and it was invisible.
 *
 * Now the real elide.c is linked and only sh8601_write_span_x is stubbed, so a
 * recorded mark is a span that would actually have been transmitted.
 */
int sh8601_write_span_x(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                        void (*rowfn)(uint16_t *row, int y))
{
    g_rowfn = rowfn;
    if (g_nmarks < MAX_MARKS) {
        g_marks[g_nmarks].x0 = (int)x0; g_marks[g_nmarks].y0 = (int)y0;
        g_marks[g_nmarks].x1 = (int)x1; g_marks[g_nmarks].y1 = (int)y1;
        g_nmarks++;
    }
    return 0;
}

int sh8601_write_span(uint16_t y0, uint16_t y1, void (*rowfn)(uint16_t *, int))
{
    return sh8601_write_span_x(0u, y0, 367u, y1, rowfn);
}

static sh8601_stats g_ss;
const sh8601_stats *sh8601_last_frame(void) { return &g_ss; }

uint32_t cpu_cycles(void) { return 0u; }

int marked(int y, int *x0, int *x1)
{
    int i, found = 0;
    for (i = 0; i < g_nmarks; i++) {
        if (y < g_marks[i].y0 || y > g_marks[i].y1) continue;
        if (!found) { *x0 = g_marks[i].x0; *x1 = g_marks[i].x1; found = 1; }
        else { if (g_marks[i].x0 < *x0) *x0 = g_marks[i].x0;
               if (g_marks[i].x1 > *x1) *x1 = g_marks[i].x1; }
    }
    return found;
}
