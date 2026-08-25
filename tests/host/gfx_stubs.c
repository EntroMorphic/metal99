/*
 * Host stubs for the layers below gfx: elide records what it was told to mark,
 * and captures the rowfn so a test can render a row and assert actual pixels.
 * gfx.c itself is the real firmware source - the point is to test the model,
 * not a reimplementation of it.
 */
#include <stdint.h>
#include <stddef.h>
#include "elide.h"
#include "gfx_stubs.h"

mark_t   g_marks[MAX_MARKS];
int      g_nmarks;
static void (*g_rowfn)(uint16_t *, int);
static elide_stats g_es;

void stub_reset(void) { g_nmarks = 0; }

void stub_render(int y, uint16_t *row)
{
    if (g_rowfn) g_rowfn(row, y);
}

void elide_init(void)  { g_nmarks = 0; }
void elide_reset(void) { }
void elide_set_resync(uint32_t f) { (void)f; }

void elide_mark_rect(int x0, int y0, int x1, int y1)
{
    if (g_nmarks < MAX_MARKS) {
        g_marks[g_nmarks].x0 = x0; g_marks[g_nmarks].y0 = y0;
        g_marks[g_nmarks].x1 = x1; g_marks[g_nmarks].y1 = y1;
        g_nmarks++;
    }
}

void elide_mark(int y0, int y1) { elide_mark_rect(0, y0, 367, y1); }

int elide_flush(void (*rowfn)(uint16_t *row, int y))
{
    g_rowfn = rowfn;
    return 0;
}

const elide_stats *elide_last(void) { return &g_es; }

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
