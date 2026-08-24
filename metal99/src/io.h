/* Direct MMIO. ISO C99: a volatile pointer dereference, nothing more. */
#ifndef IO_H
#define IO_H
#include <stdint.h>

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))

/* USB-Serial-JTAG (this board has no UART bridge; the console is native USB) */
#define USJ_EP1      REG32(0x60038000u)
#define USJ_EP1_CONF REG32(0x60038004u)
#define USJ_WR_DONE       (1u << 0)
#define USJ_IN_EP_DATA_FREE (1u << 1)

/* UART0 on GPIO43/44, in case a wire is attached */
#define UART0_FIFO   REG32(0x60000000u)
#define UART0_STATUS REG32(0x6000001Cu)

/* MEASURED 2026-08-24: the ROM leaves the CPU at 20 MHz (XTAL/2). If the PLL
 * is ever enabled this MUST change or every delay silently shortens by the
 * same factor. */
#define CPU_HZ 20000000u

uint32_t cpu_cycles(void);
void     delay_us(uint32_t us);
void     delay_ms(uint32_t ms);

void con_putc(char c);
void con_puts(const char *s);
void con_hex32(uint32_t v);
void con_dec(int32_t v);

#endif
