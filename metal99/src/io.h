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

/*
 * CPU frequency, RUNTIME not compile-time.
 *
 * The ROM leaves the CPU at 20 MHz (XTAL/2), measured. Switching to the PLL
 * changes it by up to 12x, and every delay and every telemetry figure is
 * derived from this number - so a stale constant would silently misreport all
 * of them, including the SH8601's 120 ms sleep-out MINIMUM. clk_set_cpu()
 * updates it as part of the switch; the two cannot drift apart.
 */
extern uint32_t g_cpu_hz;
#define CPU_HZ (g_cpu_hz)
#define CPU_HZ_BOOT 20000000u

uint32_t cpu_cycles(void);
void     delay_us(uint32_t us);
void     delay_ms(uint32_t ms);

void con_putc(char c);
void con_puts(const char *s);
void con_hex32(uint32_t v);
void con_dec(int32_t v);

#endif
