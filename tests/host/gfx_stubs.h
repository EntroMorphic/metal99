#ifndef GFX_STUBS_H
#define GFX_STUBS_H
#include <stdint.h>
#define MAX_MARKS 4096
typedef struct { int x0, y0, x1, y1; } mark_t;
extern mark_t g_marks[MAX_MARKS];
extern int    g_nmarks;
void stub_reset(void);
void stub_render(int y, uint16_t *row);
#endif
int marked(int y, int *x0, int *x1);
