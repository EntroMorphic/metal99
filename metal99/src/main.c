#include <stddef.h>
#include "io.h"
#include "spi2.h"
#include "sh8601.h"

static void colorbars(uint16_t *row, int y)
{
    static uint16_t bars[5];
    static int init = 0;
    int band, x;
    if (!init) {
        bars[0] = sh8601_rgb565(255, 0, 0);
        bars[1] = sh8601_rgb565(0, 255, 0);
        bars[2] = sh8601_rgb565(0, 0, 255);
        bars[3] = sh8601_rgb565(255, 255, 255);
        bars[4] = sh8601_rgb565(0, 0, 0);
        init = 1;
    }
    band = (y * 5) / SH8601_HEIGHT;
    for (x = 0; x < SH8601_WIDTH; x++) row[x] = bars[band];
}

/*
 * COLD-START PROOF.
 *
 * Everything so far has ridden on the previous firmware's initialisation. To
 * show sh8601_init() genuinely brings the panel up, first force it DOWN
 * (display off + sleep in), then bring it back with our own sequence only.
 *
 * Observable cycle:  bars -> dark -> bars
 * The second "bars" is only possible if our init worked.
 */
/* Solid magenta - unmistakably different from the bars. */
static void magenta(uint16_t *row, int y)
{
    int x;
    (void)y;
    for (x = 0; x < SH8601_WIDTH; x++) row[x] = sh8601_rgb565(255, 0, 255);
}

/*
 * ISOLATION TEST: no sleep at all.
 *
 * The panel retains its framebuffer across CPU resets, so a stale image looks
 * exactly like a working one. Alternating two very different frames makes any
 * static screen a definite failure.
 *
 *   animates      -> init + draw are fine; the sleep/wake cycle was the problem
 *   static bars   -> our init broke drawing (ghost from an earlier flash)
 */
void app_entry(void)
{
    int rc, i;

    con_puts("\r\n=== metal99 : init + redraw, NO sleep ===\r\n");
    con_puts("expect BARS <-> MAGENTA every 3s. static = failure.\r\n");

    spi2_init();

    rc = sh8601_init();
    con_puts("sh8601_init rc="); con_dec((int32_t)rc); con_puts("\r\n");

    for (i = 0; ; i++) {
        void (*fn)(uint16_t *, int) = ((i & 1) == 0) ? colorbars : magenta;
        rc = sh8601_write_frame(fn);
        con_puts(((i & 1) == 0) ? "  BARS    rc=" : "  MAGENTA rc=");
        con_dec((int32_t)rc); con_puts("\r\n");
        delay_ms(3000u);
    }
}
