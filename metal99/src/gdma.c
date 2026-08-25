#include <stddef.h>
#include "gdma.h"
#include "io.h"

/* ---- GDMA, channel 0. Channel stride is 0xC0. ---- */
#define GDMA_BASE            0x6003F000u
#define GDMA_OUT_CONF0_CH0   REG32(GDMA_BASE + 0x60u)
#define GDMA_OUT_INT_RAW_CH0 REG32(GDMA_BASE + 0x68u)
#define GDMA_OUT_INT_CLR_CH0 REG32(GDMA_BASE + 0x74u)
#define GDMA_OUT_LINK_CH0    REG32(GDMA_BASE + 0x80u)
#define GDMA_OUT_PERI_SEL_CH0 REG32(GDMA_BASE + 0xA8u)
#define GDMA_MISC_CONF       REG32(GDMA_BASE + 0x3C8u)

#define MISC_AHBM_RST_INTER  (1u << 0)
#define MISC_CLK_EN          (1u << 4)

#define OUT_RST              (1u << 0)
#define OUT_EOF_MODE         (1u << 3)
#define OUTDSCR_BURST_EN     (1u << 4)
#define OUT_DATA_BURST_EN    (1u << 5)

#define OUTLINK_STOP         (1u << 20)
#define OUTLINK_START        (1u << 21)
#define OUTLINK_ADDR_MASK    0xFFFFFu

#define INT_OUT_DONE         (1u << 0)
#define INT_OUT_EOF          (1u << 1)
#define INT_OUT_TOTAL_EOF    (1u << 3)

#define PERI_SPI2            0u        /* SOC_GDMA_TRIG_PERIPH_SPI2 */

/* ---- system clock gating ---- */
#define SYSTEM_PERIP_CLK_EN1 REG32(0x600C001Cu)
#define SYSTEM_PERIP_RST_EN1 REG32(0x600C0024u)
#define SYSTEM_DMA_BIT       (1u << 6)

/* ---- SPI2 side ---- */
#define SPI2_BASE            0x60024000u
#define SPI_DMA_CONF         REG32(SPI2_BASE + 0x30u)
#define SPI_DMA_TX_ENA       (1u << 28)
#define SPI_BUF_AFIFO_RST    (1u << 30)
#define SPI_DMA_AFIFO_RST    (1u << 31)

/* 10x the longest legitimate wait; see the note in spi2.c. */
#define GDMA_SPIN_LIMIT      200000u

static gdma_desc g_prime;    /* absorbs the swallowed first start; see below */

void gdma_init(void)
{
    /* 1. Ungate and reset the DMA engine. */
    SYSTEM_PERIP_CLK_EN1 = SYSTEM_PERIP_CLK_EN1 | SYSTEM_DMA_BIT;
    SYSTEM_PERIP_RST_EN1 = SYSTEM_PERIP_RST_EN1 | SYSTEM_DMA_BIT;
    SYSTEM_PERIP_RST_EN1 = SYSTEM_PERIP_RST_EN1 & ~SYSTEM_DMA_BIT;

    /* 2. GDMA has its OWN module clock gate, separate from the system one
     *    above, plus an internal AHB master reset. Without these the engine
     *    accepts configuration but the first transfer silently does nothing -
     *    raw status 0 while SPI ships stale AFIFO contents. */
    GDMA_MISC_CONF = GDMA_MISC_CONF | MISC_CLK_EN;
    GDMA_MISC_CONF = GDMA_MISC_CONF | MISC_AHBM_RST_INTER;
    GDMA_MISC_CONF = GDMA_MISC_CONF & ~MISC_AHBM_RST_INTER;

    /* 3. Reset channel 0 TX, then let the FSM settle before configuring it.
     *    HYPOTHESIS UNDER TEST: the first transfer after OUT_RST leaves the
     *    descriptor FSM parked and swallows the first START. */
    GDMA_OUT_CONF0_CH0 = OUT_RST;
    GDMA_OUT_CONF0_CH0 = 0u;

    /* 4. Burst on both descriptor fetch and data, EOF on the descriptor's
     *    suc_eof rather than a byte count. */
    /* HYPOTHESIS: OUTDSCR_BURST_EN prefetches descriptors in bursts. Our
     * descriptors are 12 bytes, so consecutive entries sit at 12-byte spacing -
     * a burst fetch may straddle them badly on the first chained transfer.
     * Single-descriptor spans (1 and 4 rows) pass; the first CHAIN fails.
     * Data bursting is unaffected and stays on. */
    GDMA_OUT_CONF0_CH0 = OUT_DATA_BURST_EN | OUT_EOF_MODE;

    /* 5. Bind this channel to SPI2. */
    GDMA_OUT_PERI_SEL_CH0 = PERI_SPI2;

    /* 6. Drain SPI2's FIFOs. DMA_TX_ENA is deliberately NOT set here: it
     *    switches the SPI data path wholesale, and commands still go via the
     *    64-byte FIFO. spi2_xfer()/spi2_xfer_dma() each assert the mode they
     *    need, so the two paths cannot silently interfere. */
    SPI_DMA_CONF = SPI_DMA_AFIFO_RST | SPI_BUF_AFIFO_RST;
    SPI_DMA_CONF = 0u;

    GDMA_OUT_INT_CLR_CH0 = 0xFFFFFFFFu;

    GDMA_OUT_LINK_CH0 = 0u;

    /*
     * ABSORB THE SWALLOWED FIRST START.
     *
     * Characterised, not fully explained: the first gdma_start() after init is
     * ignored - the engine stays PARKED and the descriptor is never fetched -
     * while a SECOND start takes, provided the channel is NOT reset in between.
     * Resetting undoes whatever the first arm primes, which is why an earlier
     * reset-then-retry fix did not work.
     *
     * So spend that first start here on a dummy descriptor whose owner bit is
     * CPU (0). The engine fetches it, sees it does not own the buffer, and
     * stops. No SPI transaction is triggered, so nothing reaches the panel and
     * not a single byte moves on the bus.
     *
     * Without this the first real transfer times out both guards before its
     * retry succeeds - a 293 ms frame at boot.
     */
    g_prime.dw0    = 0u;          /* owner = CPU: engine will not transfer */
    g_prime.buffer = &g_prime;
    g_prime.next   = NULL;
    gdma_start(&g_prime);
    GDMA_OUT_LINK_CH0 = OUTLINK_STOP;
    GDMA_OUT_INT_CLR_CH0 = 0xFFFFFFFFu;
}

/*
 * Full channel reset then re-arm.
 *
 * NOT CURRENTLY CALLED, and the comment here used to say "see
 * spi2_dma_finish()" - which does the exact opposite on purpose. Retrying a
 * transfer in place re-sends a band into a CS-held stream where the panel is
 * auto-incrementing through a fixed address window, shifting everything after
 * it; spi2_dma_finish() therefore aborts the span instead. Read that comment
 * before wiring this to anything.
 *
 * Kept because the swallowed-first-start behaviour it was written for is
 * characterised but not explained (DESIGN.md 6.6i), and a channel reset is the
 * obvious first instrument if it ever changes shape. --gc-sections drops it
 * from the image, so it costs nothing to keep.
 */
int gdma_restart(const gdma_desc *first)
{
    GDMA_OUT_CONF0_CH0 = OUT_RST;
    GDMA_OUT_CONF0_CH0 = 0u;
    /* HYPOTHESIS: OUTDSCR_BURST_EN prefetches descriptors in bursts. Our
     * descriptors are 12 bytes, so consecutive entries sit at 12-byte spacing -
     * a burst fetch may straddle them badly on the first chained transfer.
     * Single-descriptor spans (1 and 4 rows) pass; the first CHAIN fails.
     * Data bursting is unaffected and stays on. */
    GDMA_OUT_CONF0_CH0 = OUT_DATA_BURST_EN | OUT_EOF_MODE;
    GDMA_OUT_PERI_SEL_CH0 = PERI_SPI2;
    GDMA_OUT_INT_CLR_CH0 = 0xFFFFFFFFu;
    GDMA_OUT_LINK_CH0 = 0u;
    gdma_start(first);
    return GDMA_OK;
}

void gdma_start(const gdma_desc *first)
{
    /* Clear FIRST. A stale EOF from the previous transfer would make
     * gdma_wait() return immediately on the next one - i.e. succeed without
     * waiting, which is worse than failing. */
    GDMA_OUT_INT_CLR_CH0 = 0xFFFFFFFFu;

    /* OUTLINK_ADDR is a 20-bit field; the engine reconstructs the upper bits
     * for internal SRAM. Descriptors MUST therefore live in internal SRAM -
     * ours are in .bss at 0x3FCAxxxx. A descriptor placed elsewhere would be
     * fetched from the wrong address with no diagnostic. */
    /* TWO writes, address first, then START - not one combined write.
     *
     * Diagnosed rather than guessed: with a single combined write the engine
     * stayed PARKED on the first transfer (link bit 23 set, START/STOP read
     * back 0 as self-clearing triggers) and the descriptor's owner bit was
     * still 1, i.e. never fetched. Meanwhile SPI completed happily and shipped
     * stale AFIFO contents - one garbage frame, no error reported.
     *
     * IDF writes link.addr and link.start as separate register accesses. The
     * address must be latched before START asserts. */
    GDMA_OUT_LINK_CH0 = (uint32_t)(uintptr_t)first & OUTLINK_ADDR_MASK;
    GDMA_OUT_LINK_CH0 = OUTLINK_START
                      | ((uint32_t)(uintptr_t)first & OUTLINK_ADDR_MASK);
}

int gdma_desc_addr_ok(const void *p)
{
    /* Internal SRAM only - see the note in gdma_start(). */
    uint32_t a = (uint32_t)(uintptr_t)p;
    return (a >= 0x3FC88000u) && (a < 0x3FD00000u);
}

static uint32_t g_last_raw;

uint32_t gdma_last_status(void) { return g_last_raw; }

int gdma_wait(void)
{
    uint32_t guard = 0u;
    /* Accept any completion indication. Which flag the engine raises depends
     * on OUT_EOF_MODE and on whether the chain ended, and guessing one and
     * spinning on it is how this timed out the first time. */
    const uint32_t done = INT_OUT_TOTAL_EOF | INT_OUT_EOF | INT_OUT_DONE;

    while (((g_last_raw = GDMA_OUT_INT_RAW_CH0) & done) == 0u) {
        if (++guard > GDMA_SPIN_LIMIT) return GDMA_E_HANG;
    }
    GDMA_OUT_INT_CLR_CH0 = 0xFFFFFFFFu;
    return GDMA_OK;
}

uint32_t gdma_dbg_link(void)  { return GDMA_OUT_LINK_CH0; }
uint32_t gdma_dbg_conf0(void) { return GDMA_OUT_CONF0_CH0; }
