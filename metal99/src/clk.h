/*
 * CPU / PLL clock control. Pure ISO C99.
 *
 * The ROM hands over at 20 MHz (XTAL/2) with APB also at 20 MHz - measured,
 * not assumed. That caps SPI2 at the 40 MHz XTAL and makes 60 fps arithmetically
 * impossible: wire time alone is 16.49 ms against a 16.67 ms budget.
 *
 * Switching CPU_CLK to the PLL raises APB to 80 MHz, which is what unblocks a
 * faster SPI clock.
 */
#ifndef CLK_H
#define CLK_H

#include <stdint.h>

#define CLK_OK        0
#define CLK_E_BADFREQ (-1)

/* mhz must be 80, 160 or 240. Updates g_cpu_hz on success. */
int clk_set_cpu_pll(uint32_t mhz);

/* Whatever CPU_CLK is currently sourced from: 0 XTAL, 1 PLL, 2 RC_FAST. */
uint32_t clk_cpu_src(void);

#endif /* CLK_H */
