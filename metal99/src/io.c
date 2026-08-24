#include "io.h"

uint32_t g_cpu_hz = CPU_HZ_BOOT;   /* updated by clk_set_cpu() */

/*
 * TELEMETRY MUST NOT PERTURB THE SYSTEM IT MEASURES.
 *
 * This guard was 2,000,000 iterations - about 125 ms PER CHARACTER at 160 MHz
 * once the USB FIFO fills, which it always does when nobody is draining the
 * console. A single 60-character line could stall the CPU for seconds.
 *
 * That artefact was misread three times: as `late=620`, then `late=19`, then as
 * the animation "choking" while being watched without a capture attached. Each
 * time it looked like a graphics problem.
 *
 * 200 iterations is ~12 us per character. If the host is not reading we DROP
 * the byte rather than stalling the render loop. Losing console output is
 * strictly better than distorting the thing being measured.
 */
static void usj_putc(char c)
{
    uint32_t guard = 0;
    while (!(USJ_EP1_CONF & USJ_IN_EP_DATA_FREE)) {
        if (++guard > 200u) return;       /* host not reading: drop, do not stall */
    }
    USJ_EP1 = (uint32_t)(unsigned char)c;
    USJ_EP1_CONF = USJ_WR_DONE;
}

static void uart_putc(char c)
{
    uint32_t guard = 0;
    while (((UART0_STATUS >> 16) & 0x3FFu) > 100u) {
        if (++guard > 200u) return;       /* same reasoning as usj_putc */
    }
    UART0_FIFO = (uint32_t)(unsigned char)c;
}

void con_putc(char c) { usj_putc(c); uart_putc(c); }

void con_puts(const char *s) { while (*s != '\0') con_putc(*s++); }

void con_hex32(uint32_t v)
{
    const char *d = "0123456789ABCDEF";
    int i;
    con_puts("0x");
    for (i = 28; i >= 0; i -= 4) con_putc(d[(v >> i) & 0xFu]);
}

void con_dec(int32_t v)
{
    char b[12];
    int n = 0;
    uint32_t u;
    if (v < 0) { con_putc('-'); u = (uint32_t)(-v); } else { u = (uint32_t)v; }
    do { b[n++] = (char)('0' + (u % 10u)); u /= 10u; } while (u != 0u);
    while (n > 0) con_putc(b[--n]);
}

uint32_t cpu_cycles(void)
{
    uint32_t c;
    __asm__ __volatile__ ("rsr %0, ccount" : "=a"(c));
    return c;
}

/* ccount wraps every 2^32 cycles (~215 s at 20 MHz); unsigned subtraction
 * handles the wrap correctly for any interval shorter than that. */
void delay_us(uint32_t us)
{
    uint32_t start = cpu_cycles();
    uint32_t want  = us * (CPU_HZ / 1000000u);
    while ((cpu_cycles() - start) < want) { }
}

void delay_ms(uint32_t ms)
{
    while (ms > 0u) { delay_us(1000u); ms--; }   /* per-ms, so no overflow */
}
