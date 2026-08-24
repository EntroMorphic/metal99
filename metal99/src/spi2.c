#include "spi2.h"
#include "io.h"
#include "vec.h"
#include "gdma.h"
#include <stddef.h>

/* ---------------------------------------------------------------- bases */
#define SYSTEM_PERIP_CLK_EN0  REG32(0x600C0018u)
#define SYSTEM_PERIP_RST_EN0  REG32(0x600C0020u)
#define SYSTEM_SPI2_BIT       (1u << 6)

#define GPIO_ENABLE_W1TS      REG32(0x60004024u)   /* GPIO 0..31  */
#define GPIO_ENABLE1_W1TS     REG32(0x60004030u)   /* GPIO 32..48 */
#define GPIO_FUNC_OUT_SEL(n)  REG32(0x60004554u + 4u * (uint32_t)(n))
#define IO_MUX_GPIO(n)        REG32(0x60009004u + 4u * (uint32_t)(n))

#define SPI2_BASE   0x60024000u
#define SPI_CMD     REG32(SPI2_BASE + 0x00u)
#define SPI_CTRL    REG32(SPI2_BASE + 0x08u)
#define SPI_CLOCK   REG32(SPI2_BASE + 0x0Cu)
#define SPI_USER    REG32(SPI2_BASE + 0x10u)
#define SPI_USER1   REG32(SPI2_BASE + 0x14u)
#define SPI_USER2   REG32(SPI2_BASE + 0x18u)
#define SPI_MS_DLEN REG32(SPI2_BASE + 0x1Cu)
#define SPI_MISC    REG32(SPI2_BASE + 0x20u)
#define SPI_W(i)    REG32(SPI2_BASE + 0x98u + 4u * (uint32_t)(i))
#define SPI_DMA_CONF REG32(SPI2_BASE + 0x30u)
#define SPI_SLAVE    REG32(SPI2_BASE + 0xE0u)
#define SPI_CLK_GATE REG32(SPI2_BASE + 0xE8u)
#define SPI_DMA_INT_CLR       REG32(SPI2_BASE + 0x38u)
#define SPI_OUTFIFO_EMPTY_ERR_CLR (1u << 1)
#define SPI_DMA_TX_ENA_BIT    (1u << 28)
#define SPI_RX_AFIFO_RST_BIT  (1u << 29)
#define SPI_BUF_AFIFO_RST_BIT (1u << 30)
#define SPI_DMA_AFIFO_RST_BIT (1u << 31)

/* ---------------------------------------------------------------- fields */
#define CMD_USR            (1u << 24)
#define CMD_UPDATE         (1u << 23)

#define USER_DOUTDIN       (1u << 0)
#define USER_CK_OUT_EDGE   (1u << 9)
#define USER_USR_MOSI      (1u << 27)
#define USER_USR_MISO      (1u << 28)
#define USER_USR_DUMMY     (1u << 29)
#define USER_USR_ADDR      (1u << 30)
#define USER_USR_COMMAND   (1u << 31)

/* NOTE: fwrite_quad is in SPI_USER, NOT SPI_CTRL. Only fread_quad lives in
 * CTRL. Setting it in the wrong register fails silently - the transfer still
 * completes, just on one line. */
#define USER_FWRITE_QUAD   (1u << 13)
#define CTRL_D_POL         (1u << 19)
#define CTRL_Q_POL         (1u << 18)

#define MISC_CS0_DIS       (1u << 0)
#define MISC_CS1_DIS       (1u << 1)
#define MISC_CS2_DIS       (1u << 2)
#define MISC_CK_IDLE_EDGE  (1u << 29)
#define MISC_CS_KEEP_ACTIVE (1u << 30)

#define CLKG_CLK_EN        (1u << 0)
#define CLKG_MST_CLK_ACTIVE (1u << 1)
#define CLKG_MST_CLK_SEL   (1u << 2)

#define IOMUX_MCU_SEL_S    12
#define IOMUX_FUN_DRV_S    10
#define IOMUX_FUN_IE       (1u << 9)

/* -------------------------------------------------------------- pin map */
/* Board wires CLK/CS opposite to the IO_MUX defaults for these pins, so every
 * signal must go through the GPIO matrix. */
#define PIN_CS   12
#define PIN_CLK  11
#define PIN_D0    4
#define PIN_D1    5
#define PIN_D2    6
#define PIN_D3    7

#define SIG_FSPICLK  101
#define SIG_FSPIQ    102   /* D1 */
#define SIG_FSPID    103   /* D0 */
#define SIG_FSPIHD   104   /* D3 */
#define SIG_FSPIWP   105   /* D2 */
#define SIG_FSPICS0  110

/* ----------------------------------------------------------------- pins */
static void route_pin(uint32_t gpio, uint32_t signal)
{
    /* IO_MUX: hand the pad to the GPIO matrix (function 1), strongest drive,
     * input buffer on (QSPI lines are bidirectional even if we only write). */
    IO_MUX_GPIO(gpio) = (1u << IOMUX_MCU_SEL_S)
                      | (3u << IOMUX_FUN_DRV_S)
                      | IOMUX_FUN_IE;

    /* Drive the pad. Two banks: 1u << gpio would be undefined for gpio >= 32,
     * and this chip has pins up to 48. */
    if (gpio < 32u) {
        GPIO_ENABLE_W1TS  = (1u << gpio);
    } else {
        GPIO_ENABLE1_W1TS = (1u << (gpio - 32u));
    }

    /* Matrix: peripheral signal -> pad. OEN_SEL=0 lets the peripheral own the
     * output-enable; no inversion. */
    GPIO_FUNC_OUT_SEL(gpio) = signal & 0x1FFu;
}

/* Bounded. An unbounded spin here hangs the whole device if a bad clock config
 * stops the SPI clock - which is exactly what happened during the Phase 0
 * red-team, with no diagnostic at all. Generous: ~50 ms at 20 MHz. */
#define SPI2_SPIN_LIMIT 1000000u

static int spi2_sync(void)
{
    /* ESP32-S3 latches configuration only when SPI_UPDATE is set; it
     * self-clears once applied. Skipping this silently transfers with the
     * PREVIOUS configuration. */
    uint32_t guard = 0u;
    SPI_CMD = CMD_UPDATE;
    while ((SPI_CMD & CMD_UPDATE) != 0u) {
        if (++guard > SPI2_SPIN_LIMIT) return SPI2_E_HANG;
    }
    return SPI2_OK;
}

/* ----------------------------------------------------------------- init */
void spi2_init(void)
{
    /* 1. Ungate the peripheral clock, then pulse its reset. Order matters:
     *    a reset applied while gated does not take. */
    SYSTEM_PERIP_CLK_EN0 = SYSTEM_PERIP_CLK_EN0 | SYSTEM_SPI2_BIT;
    SYSTEM_PERIP_RST_EN0 = SYSTEM_PERIP_RST_EN0 | SYSTEM_SPI2_BIT;
    SYSTEM_PERIP_RST_EN0 = SYSTEM_PERIP_RST_EN0 & ~SYSTEM_SPI2_BIT;

    /* 2. Internal clock gate. MST_CLK_SEL: 0 = XTAL, 1 = APB (PLL-derived).
     *    The ROM leaves the PLL OFF (CPU measured at 20 MHz = XTAL/2), so APB
     *    is starved - selecting it gave a ~2.1 MHz bus. XTAL is a steady
     *    40 MHz, which is exactly the rate the SH8601 wants. */
    SPI_CLK_GATE = CLKG_CLK_EN | CLKG_MST_CLK_ACTIVE;   /* MST_CLK_SEL = 0 */

    /* 3. Pins. */
    route_pin(PIN_CLK, SIG_FSPICLK);
    route_pin(PIN_CS,  SIG_FSPICS0);
    route_pin(PIN_D0,  SIG_FSPID);
    route_pin(PIN_D1,  SIG_FSPIQ);
    route_pin(PIN_D2,  SIG_FSPIWP);
    route_pin(PIN_D3,  SIG_FSPIHD);

    /* 4. Clock: source is XTAL @ 40 MHz and the panel runs at 40 MHz, so
     *    bypass the divider entirely with CLK_EQU_SYSCLK. */
    SPI_CLOCK = (1u << 31);  /* CLK_EQU_SYSCLK: f_spi = f_source = 40 MHz */

    /* 5. SPI mode 0: clock idles low, data launched on the falling edge so it
     *    is stable at the rising edge the panel samples on. */
    SPI_MISC  = MISC_CS1_DIS | MISC_CS2_DIS;   /* CS0 enabled, CK_IDLE_EDGE=0 */
    SPI_USER  = USER_USR_MOSI;                 /* MOSI only; no cmd/addr/dummy */
    SPI_USER1 = 0u;
    SPI_USER2 = 0u;
    SPI_CTRL  = CTRL_D_POL | CTRL_Q_POL;       /* reset defaults: idle high  */

    /* 6. Clear state the ROM may have left behind. IDF's master init does the
     *    same; without it a stale DMA or slave-mode bit would change how our
     *    FIFO transfers behave, with no diagnostic. */
    SPI_SLAVE    = 0u;
    SPI_DMA_CONF = 0u;

    /* 7. Latch. Configuration written above does not take effect until
     *    SPI_UPDATE is pulsed. */
    (void)spi2_sync();
}

/* ------------------------------------------------------------- transfer */
int spi2_xfer(const uint8_t *data, uint32_t len, int quad, int keep_cs)
{

    /* Report rather than silently dropping: a caller that mis-chunks would
     * otherwise get a partially-blank panel and no clue why. */
    if (data == NULL)                       return SPI2_E_NULL;
    if (len == 0u || len > SPI2_FIFO_BYTES) return SPI2_E_LEN;
    if (((uintptr_t)data & 15u) != 0u)      return SPI2_E_ALIGN;

    /* Load the FIFO with VECTOR stores - no scalar byte packing.
     *
     * SPI_W0 is at 0x60024098: 8-byte aligned but NOT 16, so EE.VST.128 cannot
     * target it. EE.VST.L/H.64 need only 8-byte alignment, so one 128-bit
     * register goes out as two 64-bit stores. Verified on hardware that vector
     * stores do reach peripheral address space.
     *
     * Rounds up to whole vectors; MS_DLEN below bounds what is actually sent. */
    {
        volatile uint32_t *w = (volatile uint32_t *)(uintptr_t)(SPI2_BASE + 0x98u);
        const uint8_t *src = data;
        uint32_t vectors = (len + (uint32_t)VEC_BYTES - 1u) / (uint32_t)VEC_BYTES;
        __asm__ __volatile__ (
            "1:                            \n"
            "  ee.vld.128.ip  q3, %1, 16   \n"
            "  ee.vst.l.64.ip q3, %0, 8    \n"
            "  ee.vst.h.64.ip q3, %0, 8    \n"
            "  addi.n         %2, %2, -1   \n"
            "  bnez           %2, 1b       \n"
            : "+a"(w), "+a"(src), "+a"(vectors) : : "memory");
    }

    /* CK_OUT_EDGE stays 0 and CK_IDLE_EDGE stays 0: that pair IS SPI mode 0.
     * Both are load-bearing by omission, so do not "tidy" them away. */
    SPI_DMA_CONF = 0u;               /* FIFO path: DMA must be off */
    SPI_CTRL = CTRL_D_POL | CTRL_Q_POL;
    SPI_MISC = MISC_CS1_DIS | MISC_CS2_DIS | (keep_cs ? MISC_CS_KEEP_ACTIVE : 0u);
    SPI_USER = USER_USR_MOSI | (quad ? USER_FWRITE_QUAD : 0u);
    SPI_MS_DLEN = (len * 8u) - 1u;   /* hardware wants bits-minus-one */

    if (spi2_sync() != SPI2_OK) return SPI2_E_HANG;

    SPI_CMD = CMD_USR;
    {
        uint32_t guard = 0u;
        while ((SPI_CMD & CMD_USR) != 0u) {
            if (++guard > SPI2_SPIN_LIMIT) return SPI2_E_HANG;
        }
    }
    return SPI2_OK;
}

int spi2_write(const uint8_t *data, uint32_t len, int quad)
{
    if (data == NULL) return SPI2_E_NULL;
    if (len == 0u)    return SPI2_E_LEN;

    while (len > 0u) {
        uint32_t n = (len > SPI2_FIFO_BYTES) ? (uint32_t)SPI2_FIFO_BYTES : len;
        int last = (n == len);
        int rc = spi2_xfer(data, n, quad, last ? 0 : 1);
        if (rc != SPI2_OK) return rc;
        data += n;
        len  -= n;
    }
    return SPI2_OK;
}

int spi2_dma_start(const struct gdma_desc *chain, uint32_t len, int quad, int keep_cs)
{
    if (chain == NULL) return SPI2_E_NULL;
    if (len == 0u)     return SPI2_E_LEN;
    if (!gdma_desc_addr_ok(chain)) return SPI2_E_ALIGN;

    /* Mirror IDF's spi_hal_hw_prepare_tx(): AFIFO reset, clear the latched
     * outfifo-empty error, enable DMA TX. */
    SPI_DMA_CONF = SPI_DMA_TX_ENA_BIT | SPI_BUF_AFIFO_RST_BIT | SPI_DMA_AFIFO_RST_BIT;
    SPI_DMA_CONF = SPI_DMA_TX_ENA_BIT;
    SPI_DMA_INT_CLR = SPI_OUTFIFO_EMPTY_ERR_CLR;

    SPI_CTRL = CTRL_D_POL | CTRL_Q_POL;
    SPI_MISC = MISC_CS1_DIS | MISC_CS2_DIS | (keep_cs ? MISC_CS_KEEP_ACTIVE : 0u);
    SPI_USER = USER_USR_MOSI | (quad ? USER_FWRITE_QUAD : 0u);
    SPI_MS_DLEN = (len * 8u) - 1u;

    /* ORDER: DMA link first, then apply config, then trigger. Applying config
     * before starting the link left the engine parked on the first transfer -
     * see docs/DESIGN.md 6.6i. */
    gdma_start((const gdma_desc *)chain);
    if (spi2_sync() != SPI2_OK) { SPI_DMA_CONF = 0u; return SPI2_E_HANG; }
    SPI_CMD = CMD_USR;
    return SPI2_OK;
}

int spi2_dma_finish(void)
{
    uint32_t guard = 0u;
    while ((SPI_CMD & CMD_USR) != 0u) {
        if (++guard > SPI2_SPIN_LIMIT) { SPI_DMA_CONF = 0u; return SPI2_E_HANG; }
    }
    if (gdma_wait() != GDMA_OK) { SPI_DMA_CONF = 0u; return SPI2_E_HANG; }
    SPI_DMA_CONF = 0u;
    return SPI2_OK;
}

int spi2_xfer_dma(const struct gdma_desc *chain, uint32_t len, int quad, int keep_cs)
{
    int rc = spi2_dma_start(chain, len, quad, keep_cs);
    if (rc != SPI2_OK) return rc;
    return spi2_dma_finish();
}

int spi2_set_clock(uint32_t mhz)
{
    uint32_t gate = CLKG_CLK_EN | CLKG_MST_CLK_ACTIVE;

    if (mhz == 40u) {
        gate |= 0u;                       /* MST_CLK_SEL = 0 -> XTAL 40 MHz */
    } else if (mhz == 80u) {
        gate |= CLKG_MST_CLK_SEL;         /* MST_CLK_SEL = 1 -> APB          */
    } else {
        return SPI2_E_LEN;
    }
    SPI_CLK_GATE = gate;
    SPI_CLOCK    = (1u << 31);            /* CLK_EQU_SYSCLK: no division     */
    return spi2_sync();
}
