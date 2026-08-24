/* Entry point. The ROM bootloader jumps here with a valid stack of its own,
 * so no assembly prologue is needed - we can be plain C from the first
 * instruction. We zero .bss ourselves (nobody else will). */
#include "io.h"
#include "vec.h"

extern uint32_t _bss_start;
extern uint32_t _bss_end;

void app_entry(void);
void wdt_disable_all(void);

void _start(void) __attribute__((section(".entry.text")));
void _start(void)
{
    wdt_disable_all();          /* before anything that takes time */

    /* Xtensa gates coprocessors behind CPENABLE; the 128-bit vector unit (PIE)
     * is one of them. Nothing has enabled it bare metal, so any EE.* would trap
     * as Coprocessor Disabled. No context switching here, so enable all of them
     * permanently and never think about it again. */
    __asm__ __volatile__ ("wsr.cpenable %0\n rsync" : : "a"(0xFFu));
    /* Vectorised .bss clear. The linker aligns and pads .bss to whole 128-bit
     * vectors, so this covers it exactly with no scalar tail. Must come AFTER
     * CPENABLE above, or the first EE.* traps. */
    vec_zero(&_bss_start,
             (uint32_t)((uint8_t *)&_bss_end - (uint8_t *)&_bss_start) / VEC_BYTES);
    app_entry();
    for (;;) { __asm__ __volatile__ ("waiti 0"); }
}
