#include "clk.h"
#include "io.h"

#define SYSTEM_CPU_PER_CONF  REG32(0x600C0010u)
#define SYSTEM_SYSCLK_CONF   REG32(0x600C0060u)

#define CPUPERIOD_SEL_S      0
#define CPUPERIOD_SEL_M      0x3u
#define SOC_CLK_SEL_S        10
#define SOC_CLK_SEL_M        0x3u
#define PRE_DIV_CNT_S        0
#define PRE_DIV_CNT_M        0x3FFu

#define SRC_XTAL 0u
#define SRC_PLL  1u

uint32_t clk_cpu_src(void)
{
    return (SYSTEM_SYSCLK_CONF >> SOC_CLK_SEL_S) & SOC_CLK_SEL_M;
}

int clk_set_cpu_pll(uint32_t mhz)
{
    uint32_t period, v;

    switch (mhz) {
    case 80u:  period = 0u; break;
    case 160u: period = 1u; break;
    case 240u: period = 2u; break;
    default:   return CLK_E_BADFREQ;
    }

    /* Order matters and mirrors what the ROM/IDF sequence does: choose the
     * PLL divider FIRST, clear the pre-divider, and only then move the source.
     * Switching source before the divider is set would briefly run the core
     * at the wrong rate. */
    v = SYSTEM_CPU_PER_CONF;
    v &= ~(CPUPERIOD_SEL_M << CPUPERIOD_SEL_S);
    v |= (period & CPUPERIOD_SEL_M) << CPUPERIOD_SEL_S;
    SYSTEM_CPU_PER_CONF = v;

    v = SYSTEM_SYSCLK_CONF;
    v &= ~(PRE_DIV_CNT_M << PRE_DIV_CNT_S);      /* divide by 1 */
    SYSTEM_SYSCLK_CONF = v;

    v = SYSTEM_SYSCLK_CONF;
    v &= ~(SOC_CLK_SEL_M << SOC_CLK_SEL_S);
    v |= SRC_PLL << SOC_CLK_SEL_S;
    SYSTEM_SYSCLK_CONF = v;

    /* Verify the switch actually took. Writing SOC_CLK_SEL does not guarantee
     * the source changed - if the BBPLL were not running the core would stall
     * instead, and we would never reach here. Reading it back is cheap and
     * turns a silent assumption into a checked one. */
    if (clk_cpu_src() != SRC_PLL) return CLK_E_NOSWITCH;

    /* Keep the timebase honest. Everything downstream - delays, telemetry,
     * the panel's 120 ms sleep-out minimum - reads this. */
    g_cpu_hz = mhz * 1000000u;
    return CLK_OK;
}

int clk_set_cpu_xtal(void)
{
    uint32_t v = SYSTEM_SYSCLK_CONF;
    v &= ~(SOC_CLK_SEL_M << SOC_CLK_SEL_S);
    v |= SRC_XTAL << SOC_CLK_SEL_S;
    SYSTEM_SYSCLK_CONF = v;
    if (clk_cpu_src() != SRC_XTAL) return CLK_E_NOSWITCH;
    g_cpu_hz = CPU_HZ_BOOT;
    return CLK_OK;
}
