#include "io.h"

extern uint32_t _bss_start;
extern uint32_t _bss_end;

/* Xtensa cycle counter - our only timebase, no esp_timer. */
static uint32_t ccount(void)
{
    uint32_t c;
    __asm__ __volatile__ ("rsr %0, ccount" : "=a"(c));
    return c;
}

void app_entry(void)
{
    uint32_t t0, t1;
    int i;

    con_puts("\r\n");
    con_puts("=== metal99: zero-dependency ESP32-S3 ===\r\n");
    con_puts("no esp-idf, no freertos, no rom calls\r\n");

    con_puts("bss_start = "); con_hex32((uint32_t)(uintptr_t)&_bss_start);
    con_puts("  bss_end = "); con_hex32((uint32_t)(uintptr_t)&_bss_end);
    con_puts("\r\n");

    t0 = ccount();
    for (i = 0; i < 1000000; i++) { __asm__ __volatile__ ("" ::: "memory"); }
    t1 = ccount();
    con_puts("1e6 empty iters = "); con_dec((int32_t)(t1 - t0));
    con_puts(" cycles -> cpu approx ");
    con_dec((int32_t)((t1 - t0) / 1000000 * 1));
    con_puts(" cyc/iter\r\n");

    con_puts("alive; heartbeat follows\r\n");
    for (i = 0;; i++) {
        uint32_t s = ccount();
        while ((ccount() - s) < 80000000u) { }   /* ~1s at 80MHz boot clock */
        con_puts("tick "); con_dec(i); con_puts("\r\n");
    }
}
