#include <stddef.h>
#include "io.h"
#include "spi2.h"
#include "sh8601.h"

/* Five bands: red, green, blue, white, black. Chosen over a solid fill because
 * one image verifies byte order, geometry and the address window at once. */
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

/* Vertical ramp, so a second frame proves we can redraw, not just draw once. */
static void gradient(uint16_t *row, int y)
{
    int x;
    uint8_t v = (uint8_t)((y * 255) / (SH8601_HEIGHT - 1));
    for (x = 0; x < SH8601_WIDTH; x++) {
        uint8_t u = (uint8_t)((x * 255) / (SH8601_WIDTH - 1));
        row[x] = sh8601_rgb565(v, u, (uint8_t)(255u - v));
    }
}

void app_entry(void)
{
    uint32_t t0, ms;
    int rc, i;

    con_puts("\r\n=== metal99 : SH8601 pixel path ===\r\n");
    spi2_init();
    (void)sh8601_brightness(0xFFu);

    for (i = 0; ; i++) {
        void (*fn)(uint16_t *, int) = ((i & 1) == 0) ? colorbars : gradient;

        t0 = cpu_cycles();
        rc = sh8601_write_frame(fn);
        ms = (cpu_cycles() - t0) / (CPU_HZ / 1000u);

        con_puts(((i & 1) == 0) ? "  colorbars " : "  gradient  ");
        con_puts("rc="); con_dec((int32_t)rc);
        con_puts("  frame="); con_dec((int32_t)ms); con_puts(" ms");
        con_puts(rc == SPI2_OK ? "  sent\r\n" : "  FAILED\r\n");

        delay_ms(3000u);
    }
}
