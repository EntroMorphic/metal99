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

#define GDMA_SPIN_LIMIT      2000000u

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
    GDMA_OUT_CONF0_CH0 = OUTDSCR_BURST_EN | OUT_DATA_BURST_EN | OUT_EOF_MODE;

    /* 5. Bind this channel to SPI2. */
    GDMA_OUT_PERI_SEL_CH0 = PERI_SPI2;

    /* 6. Drain SPI2's FIFOs. DMA_TX_ENA is deliberately NOT set here: it
     *    switches the SPI data path wholesale, and commands still go via the
     *    64-byte FIFO. spi2_xfer()/spi2_xfer_dma() each assert the mode they
     *    need, so the two paths cannot silently interfere. */
    SPI_DMA_CONF = SPI_DMA_AFIFO_RST | SPI_BUF_AFIFO_RST;
    SPI_DMA_CONF = 0u;

    GDMA_OUT_INT_CLR_CH0 = 0xFFFFFFFFu;

    /* Prime the descriptor FSM. The first START after OUT_RST is swallowed -
     * the engine stays PARKED and the transfer silently does nothing. Issuing
     * a STOP here leaves the FSM in a state where the next START takes. */
    GDMA_OUT_LINK_CH0 = OUTLINK_STOP;
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
