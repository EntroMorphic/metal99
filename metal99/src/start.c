/* Entry point. The ROM bootloader jumps here with a valid stack of its own,
 * so no assembly prologue is needed - we can be plain C from the first
 * instruction. We zero .bss ourselves (nobody else will). */
#include "io.h"

extern uint32_t _bss_start;
extern uint32_t _bss_end;

void app_entry(void);
void wdt_disable_all(void);

void _start(void) __attribute__((section(".entry.text")));
void _start(void)
{
    uint32_t *p;
    wdt_disable_all();          /* before anything that takes time */
    for (p = &_bss_start; p < &_bss_end; p++) *p = 0u;
    app_entry();
    for (;;) { __asm__ __volatile__ ("waiti 0"); }
}
