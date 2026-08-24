#include "gdma.h"
#include "io.h"

/* ---- GDMA, channel 0. Channel stride is 0xC0. ---- */
#define GDMA_BASE            0x6003F000u
#define GDMA_OUT_CONF0_CH0   REG32(GDMA_BASE + 0x60u)
#define GDMA_OUT_INT_RAW_CH0 REG32(GDMA_BASE + 0x68u)
#define GDMA_OUT_INT_CLR_CH0 REG32(GDMA_BASE + 0x74u)
#define GDMA_OUT_LINK_CH0    REG32(GDMA_BASE + 0x80u)
#define GDMA_OUT_PERI_SEL_CH0 REG32(GDMA_BASE + 0xA8u)

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

    /* 2. Reset channel 0 TX. */
    GDMA_OUT_CONF0_CH0 = OUT_RST;
    GDMA_OUT_CONF0_CH0 = 0u;

    /* 3. Burst on both descriptor fetch and data, EOF on the descriptor's
     *    suc_eof rather than a byte count. */
    GDMA_OUT_CONF0_CH0 = OUTDSCR_BURST_EN | OUT_DATA_BURST_EN | OUT_EOF_MODE;

    /* 4. Bind this channel to SPI2. */
    GDMA_OUT_PERI_SEL_CH0 = PERI_SPI2;

    /* 5. Drain SPI2's FIFOs. DMA_TX_ENA is deliberately NOT set here: it
     *    switches the SPI data path wholesale, and commands still go via the
     *    64-byte FIFO. spi2_xfer()/spi2_xfer_dma() each assert the mode they
     *    need, so the two paths cannot silently interfere. */
    SPI_DMA_CONF = SPI_DMA_AFIFO_RST | SPI_BUF_AFIFO_RST;
    SPI_DMA_CONF = 0u;

    GDMA_OUT_INT_CLR_CH0 = 0xFFFFFFFFu;
}

void gdma_start(const gdma_desc *first)
{
    GDMA_OUT_INT_CLR_CH0 = 0xFFFFFFFFu;
    /* Only the low 20 bits of the descriptor address go in the register; the
     * engine implies the rest from internal SRAM's fixed upper address. */
    GDMA_OUT_LINK_CH0 = OUTLINK_START
                      | ((uint32_t)(uintptr_t)first & OUTLINK_ADDR_MASK);
}

int gdma_wait(void)
{
    uint32_t guard = 0u;
    while ((GDMA_OUT_INT_RAW_CH0 & INT_OUT_TOTAL_EOF) == 0u) {
        if (++guard > GDMA_SPIN_LIMIT) return GDMA_E_HANG;
    }
    GDMA_OUT_INT_CLR_CH0 = 0xFFFFFFFFu;
    return GDMA_OK;
}
