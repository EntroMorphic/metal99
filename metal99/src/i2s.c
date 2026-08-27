/* I2S0 TX master + GDMA channel 1. See i2s.h for the clock arithmetic. */
#include <stddef.h>
#include "i2s.h"
#include "gdma.h"
#include "vec.h"   /* VEC_ALIGN */
#include "io.h"

/* ------------------------------------------------------------- I2S0 regs */
#define I2S0                 0x6000F000u
#define I2S_TX_CONF          REG32(I2S0 + 0x24u)
#define I2S_TX_CONF1         REG32(I2S0 + 0x2Cu)
#define I2S_TX_CLKM_CONF     REG32(I2S0 + 0x34u)
#define I2S_TX_CLKM_DIV_CONF REG32(I2S0 + 0x3Cu)
#define I2S_TX_TDM_CTRL      REG32(I2S0 + 0x54u)

#define TX_RESET        (1u << 0)
#define TX_FIFO_RESET   (1u << 1)
#define TX_START        (1u << 2)
#define TX_MONO         (1u << 5)
#define TX_BIG_ENDIAN   (1u << 7)
#define TX_UPDATE       (1u << 8)
#define TX_STOP_EN      (1u << 13)
#define TX_WS_IDLE_POL  (1u << 17)
#define TX_TDM_EN       (1u << 19)   /* frame structure - without it, no WS */
#define TX_PDM_EN       (1u << 20)
#define TX_CHAN_MOD_S   24

#define TX_TDM_WS_WIDTH_S  0
#define TX_BCK_DIV_NUM_S   7
#define TX_BITS_MOD_S     13
#define TX_HALF_SAMPLE_BITS_S 18
#define TX_TDM_CHAN_BITS_S    24
#define TX_MSB_SHIFT    (1u << 29)

#define TX_CLKM_DIV_NUM_S  0
#define TX_CLK_ACTIVE   (1u << 26)
#define I2S_CLK_EN      (1u << 29)   /* module clock enable - NOT optional */
#define TX_CLK_SEL_S      27

#define TX_CLKM_DIV_Z_S    0
#define TX_CLKM_DIV_Y_S    9
#define TX_CLKM_DIV_X_S   18
#define TX_CLKM_DIV_YN1 (1u << 27)

#define TX_TDM_CHAN0_EN (1u << 0)
#define TX_TDM_CHAN1_EN (1u << 1)
#define TX_TDM_TOT_CHAN_NUM_S 16

/* ------------------------------------------------------- system + GDMA */
#define SYSTEM_PERIP_CLK_EN0 REG32(0x600C0000u + 0x018u)
#define SYSTEM_PERIP_RST_EN0 REG32(0x600C0000u + 0x020u)
#define SYSTEM_I2S0_BIT      (1u << 4)
#define SYSTEM_PERIP_CLK_EN1 REG32(0x600C0000u + 0x01Cu)
#define SYSTEM_PERIP_RST_EN1 REG32(0x600C0000u + 0x024u)
#define SYSTEM_DMA_BIT       (1u << 6)

/* Channel 1: channel 0's offsets plus 0xC0 (CH0 OUT_CONF0 0x60, CH1 0x120). */
#define GDMA_BASE             0x6003F000u
#define GDMA_OUT_CONF0_CH1    REG32(GDMA_BASE + 0x120u)
#define GDMA_OUT_INT_RAW_CH1  REG32(GDMA_BASE + 0x128u)
#define GDMA_OUT_INT_CLR_CH1  REG32(GDMA_BASE + 0x134u)
#define GDMA_OUT_LINK_CH1     REG32(GDMA_BASE + 0x140u)
#define GDMA_OUT_PERI_SEL_CH1 REG32(GDMA_BASE + 0x168u)
/*
 * Bit positions taken from gdma.c, which drives channel 0 and works. The first
 * version of this file guessed OUTLINK_START at bit 29; it is bit 21, so the
 * engine was never started at all and the descriptor's owner bit stayed set -
 * which is exactly the symptom gdma.c already documents.
 */
#define OUT_RST               (1u << 0)
#define OUT_EOF_MODE          (1u << 3)
#define OUT_DATA_BURST_EN     (1u << 5)
#define OUTLINK_STOP          (1u << 20)
#define OUTLINK_START         (1u << 21)
#define OUTLINK_ADDR_MASK     0xFFFFFu
#define PERI_I2S0             3u

/* ----------------------------------------------------------------- pins */
#define PIN_MCLK 16u
#define PIN_BCK   9u
#define PIN_WS   45u
#define PIN_DOUT  8u
#define SIG_BCK_OUT  22u
#define SIG_MCLK_OUT 23u
#define SIG_WS_OUT   24u
#define SIG_SD_OUT   25u

#define GPIO_BASE          0x60004000u
#define GPIO_ENABLE_W1TS   REG32(GPIO_BASE + 0x24u)
#define GPIO_ENABLE1_W1TS  REG32(GPIO_BASE + 0x30u)
#define GPIO_OUT_W1TS      REG32(GPIO_BASE + 0x08u)
#define GPIO_OUT_W1TC      REG32(GPIO_BASE + 0x0Cu)
#define GPIO_OUT1_W1TS     REG32(GPIO_BASE + 0x14u)
#define GPIO_OUT1_W1TC     REG32(GPIO_BASE + 0x18u)
#define GPIO_FUNC_OUT(n)   REG32(GPIO_BASE + 0x554u + 4u * (uint32_t)(n))
#define GPIO_IN0           REG32(GPIO_BASE + 0x3Cu)
#define GPIO_IN1           REG32(GPIO_BASE + 0x40u)
#define IO_MUX_GPIO(n)     REG32(0x60009004u + 4u * (uint32_t)(n))
#define IOMUX_MCU_SEL_S    12
#define IOMUX_FUN_DRV_S    10
#define IOMUX_FUN_IE       (1u << 9)

/*
 * A LOOPING DESCRIPTOR CHAIN: one descriptor whose `next` is itself.
 *
 * GDMA re-reads the descriptor each time round, so the owner bit has to be put
 * back - the hardware clears it when it finishes with one. Without that the
 * chain stops after a single pass and the sound is a 20 ms click, which is
 * indistinguishable from "nothing works" if you are not expecting it.
 */
static gdma_desc VEC_ALIGN g_desc;
/*
 * SHADOW OF TX_CONF, and it is not a convenience.
 *
 * Every write to that register used to be a read-modify-write, and TX_UPDATE
 * is a self-clearing REQUEST bit that lives in it. So each RMW read the
 * register while UPDATE was still pending, ORed in whatever it wanted, and
 * wrote UPDATE straight back - re-asserting the very bit it was waiting on.
 * The bit could never clear, the configuration never latched, and the
 * transmitter ran its clocks with no frame sync. Measured as TX_UPDATE stuck
 * set indefinitely while IDF's identical-looking code clears in cycles.
 *
 * The rule now: hardware is never read to decide what to write. The intended
 * value lives here, is written whole, and UPDATE is pulsed separately so it is
 * never fed back into anything.
 */
static uint32_t g_txconf;
static uint32_t g_frames;
static uint32_t g_bytes;
static uint32_t g_rearms;

/* FUN_IE is set on outputs deliberately: it lets the pad be READ BACK, which
 * is how i2s_dbg_pad_edges tells "the clock is running" from "the clock is
 * configured". Without it, silence has no diagnosis. */
/*
 * Write the shadow, then request a latch, then wait for the request to clear.
 *
 * The wait is bounded but not fatal: TX_UPDATE self-clears in a handful of
 * cycles once it is not being fed back, and if a future revision changes that
 * a stuck bit should not stop the transmitter from being started - the earlier
 * version returned an error here and left TX_START unwritten, turning a
 * partly-working transmitter into a dead one.
 */
static void tx_apply(void)
{
    uint32_t guard = 0u;
    I2S_TX_CONF = g_txconf;
    I2S_TX_CONF = g_txconf | TX_UPDATE;
    while ((I2S_TX_CONF & TX_UPDATE) != 0u) if (++guard > 10000u) break;
}

static void route(uint32_t gpio, uint32_t sig)
{
    IO_MUX_GPIO(gpio) = (1u << IOMUX_MCU_SEL_S) | (3u << IOMUX_FUN_DRV_S)
                      | IOMUX_FUN_IE;
    if (gpio < 32u) GPIO_ENABLE_W1TS  = (1u << gpio);
    else            GPIO_ENABLE1_W1TS = (1u << (gpio - 32u));
    GPIO_FUNC_OUT(gpio) = sig & 0x1FFu;
}

int i2s_init(void)
{
    /*
     * The GDMA PERIPHERAL clock, not just I2S0's.
     *
     * gdma_init() turns this on, but it also binds channel 0 to SPI2 and an
     * audio path has no business depending on the display transport having
     * been initialised first. The write is idempotent, so doing it here costs
     * nothing and removes the ordering dependency. Leaving it out entirely is
     * what made the first attempt silent: the descriptor's owner bit was still
     * set afterwards, meaning GDMA had never run at all.
     */
    SYSTEM_PERIP_CLK_EN1 = SYSTEM_PERIP_CLK_EN1 | SYSTEM_DMA_BIT;
    SYSTEM_PERIP_RST_EN1 = SYSTEM_PERIP_RST_EN1 & ~SYSTEM_DMA_BIT;

    SYSTEM_PERIP_CLK_EN0 = SYSTEM_PERIP_CLK_EN0 | SYSTEM_I2S0_BIT;
    SYSTEM_PERIP_RST_EN0 = SYSTEM_PERIP_RST_EN0 | SYSTEM_I2S0_BIT;
    SYSTEM_PERIP_RST_EN0 = SYSTEM_PERIP_RST_EN0 & ~SYSTEM_I2S0_BIT;

    I2S_TX_CONF = TX_RESET | TX_FIFO_RESET;
    I2S_TX_CONF = 0u;
    g_txconf = 0u;

    /*
     * MCLK = XTAL / 10 = 4.000 MHz. clk_sel 0 is XTAL; the fractional fields
     * X/Y/Z stay zero because the division is exact, which is the whole reason
     * the sample rate is 15625 and not 16000.
     */
    I2S_TX_CLKM_DIV_CONF = 0u;
    /*
     * I2S_CLK_EN (bit 29) is the module's master clock gate and it is separate
     * from both SYSTEM_I2S0_CLK_EN and TX_CLK_ACTIVE. Without it every divider
     * is configured correctly and no clock leaves the chip: measured MCLK=0
     * edges, WS=0, BCLK=20 over 2 ms where 2000 were expected.
     */
    I2S_TX_CLKM_CONF = (10u << TX_CLKM_DIV_NUM_S)
                     | (0u  << TX_CLK_SEL_S)
                     | TX_CLK_ACTIVE
                     | I2S_CLK_EN;

    /*
     * BCLK = MCLK / 8 = 500 kHz. 16-bit slots, WS half-period 16 BCLK, so
     * LRCK = 500k/32 = 15625 Hz. bits_mod 15 is "16 bits" - the field is
     * width-minus-one.
     */
    /*
     * THREE width fields, not one, and missing any of them is silent.
     *
     *   bits_mod       how many bits of audio data per slot   (16)
     *   tdm_chan_bits  how wide the SLOT is                   (16)
     *   half_sample    BCKs per WS half-period                (16)
     *
     * Only bits_mod was set at first. The slot width defaulted to 1, so the
     * frame was two 1-bit slots and WS never toggled at anything like the
     * sample rate - measured as WS=0 edges over 2 ms while BCLK ran perfectly.
     * All three are width-minus-one.
     */
    I2S_TX_CONF1 = (7u  << TX_BCK_DIV_NUM_S)      /* MCLK/BCLK div - 1 */
                 | (15u << TX_BITS_MOD_S)
                 | (15u << TX_TDM_CHAN_BITS_S)
                 | (15u << TX_HALF_SAMPLE_BITS_S)
                 | (15u << TX_TDM_WS_WIDTH_S)
                 | TX_MSB_SHIFT;                  /* Philips I2S: 1-bit delay */

    /* Two channels, both enabled, stereo frames. */
    I2S_TX_TDM_CTRL = TX_TDM_CHAN0_EN | TX_TDM_CHAN1_EN
                    | (1u << TX_TDM_TOT_CHAN_NUM_S);   /* total - 1 */

    /*
     * NO TX_STOP_EN.
     *
     * It halts the transmitter whenever the FIFO runs dry, which sounds like
     * safety and is actually a deadlock here: WS and the data line stop, so
     * the peripheral stops pulling, so GDMA never drains the descriptor, so
     * the FIFO stays dry. Measured exactly that - BCLK running at 2000 edges
     * with WS at 0 and the descriptor's owner bit never cleared.
     *
     * Free-running is also the right behaviour for audio: an underrun should
     * be a moment of silence in a continuing stream, not a stopped clock the
     * codec has to resynchronise to.
     */
    /*
     * TX_TDM_EN IS NOT OPTIONAL AND NOT A DEFAULT.
     *
     * It selects the frame structure the transmitter emits. Without it the
     * clocks run - MCLK and BCLK both measured leaving the chip - and there is
     * no frame at all: word select never toggles, so the peripheral never asks
     * GDMA for data, so the descriptor is never consumed. Every symptom in the
     * previous commit follows from this one missing bit.
     *
     * PDM is explicitly disabled alongside it, because the two are separate
     * enables and the vendor driver clears one while setting the other rather
     * than trusting the reset state.
     */
    g_txconf = TX_WS_IDLE_POL | TX_TDM_EN | (0u << TX_CHAN_MOD_S);
    g_txconf &= ~TX_PDM_EN;
    tx_apply();

    route(PIN_MCLK, SIG_MCLK_OUT);
    route(PIN_BCK,  SIG_BCK_OUT);
    route(PIN_WS,   SIG_WS_OUT);
    route(PIN_DOUT, SIG_SD_OUT);

    GDMA_OUT_CONF0_CH1 = OUT_RST;
    GDMA_OUT_CONF0_CH1 = 0u;
    GDMA_OUT_CONF0_CH1 = OUT_DATA_BURST_EN | OUT_EOF_MODE;
    GDMA_OUT_PERI_SEL_CH1 = PERI_I2S0;
    return I2S_OK;
}

int i2s_play_loop(const int16_t *frames, uint32_t nframes)
{
    uint32_t bytes = nframes * I2S_CHANNELS * 2u;

    if (frames == NULL || nframes == 0u) return I2S_E_HANG;
    if (bytes > GDMA_MAX_XFER) return I2S_E_HANG;   /* one descriptor's worth */

    i2s_stop();

    g_desc.dw0    = GDMA_DW0(bytes, bytes, 1);
    g_desc.buffer = frames;
    g_desc.next   = &g_desc;                        /* loop to itself */
    g_frames      = nframes;
    g_bytes       = bytes;
    g_rearms      = 0u;

    GDMA_OUT_INT_CLR_CH1 = 0xFFFFFFFFu;
    /*
     * TWO writes: address first, then START. gdma.c diagnosed this the hard
     * way on channel 0 - with one combined write the engine stays parked, the
     * descriptor is never fetched, and the peripheral happily transmits
     * whatever was already in its FIFO with no error anywhere.
     */
    GDMA_OUT_LINK_CH1 = (uint32_t)(uintptr_t)&g_desc & OUTLINK_ADDR_MASK;
    GDMA_OUT_LINK_CH1 = OUTLINK_START
                      | ((uint32_t)(uintptr_t)&g_desc & OUTLINK_ADDR_MASK);


    /*
     * LATCH THE CONFIG, WAIT FOR IT, THEN START - three steps, not one write.
     *
     * TX_UPDATE is a self-clearing request to move the shadow registers into
     * the working set. Asserting it in the same write as TX_START asks the
     * transmitter to begin from a configuration that has not landed yet. IDF
     * sets update, spins until it clears, and only then starts; spi2_sync()
     * does exactly the same dance for SPI_UPDATE, and skipping it there
     * silently transferred with the PREVIOUS configuration.
     */
    /*
     * TX_UPDATE then TX_START, but NOT spinning on update to clear.
     *
     * IDF spins on it. Here it never clears - it reads back set indefinitely -
     * and spinning simply returned an error before TX_START was ever written,
     * which turned a partly-working transmitter into a dead one. Whatever the
     * bit means on this silicon, waiting for it is not viable, and a short
     * delay gets the same latch with the clocks demonstrably running.
     */
    /* Reset the transmitter and its FIFO, then latch, then start - each a
     * whole-register write from the shadow, never a read-back. */
    I2S_TX_CONF = g_txconf | TX_RESET | TX_FIFO_RESET;
    I2S_TX_CONF = g_txconf;
    tx_apply();

    g_txconf |= TX_START;
    tx_apply();
    return I2S_OK;
}

void i2s_stop(void)
{
    g_txconf &= ~TX_START;
    tx_apply();
    GDMA_OUT_LINK_CH1 = OUTLINK_STOP;
}

/*
 * RE-ARM THE LOOP.
 *
 * GDMA clears the descriptor's owner bit when it finishes with it, and nothing
 * in hardware puts it back - a self-referencing `next` makes the engine come
 * round again, find the buffer marked as the CPU's, and stop. The result is
 * one buffer of audio and then silence, which from the outside is
 * indistinguishable from never having started.
 *
 * Software owns that re-arm. Call this often enough that a buffer's worth of
 * audio has not elapsed: at 15625 Hz a 1000-frame buffer is 64 ms, so 60 Hz
 * polling has four times the margin it needs.
 */
uint32_t i2s_service(void)
{
    if ((g_desc.dw0 & (1u << 31)) != 0u) return 0u;   /* still playing */

    g_desc.dw0 = GDMA_DW0(g_bytes, g_bytes, 1);
    GDMA_OUT_INT_CLR_CH1 = 0xFFFFFFFFu;
    GDMA_OUT_LINK_CH1 = (uint32_t)(uintptr_t)&g_desc & OUTLINK_ADDR_MASK;
    GDMA_OUT_LINK_CH1 = OUTLINK_START
                      | ((uint32_t)(uintptr_t)&g_desc & OUTLINK_ADDR_MASK);
    g_rearms++;
    return 1u;
}

uint32_t i2s_rearms(void)     { return g_rearms; }
uint32_t i2s_frames_out(void) { return g_frames; }

static uint32_t pad_level(uint32_t g)
{
    return (g < 32u) ? ((GPIO_IN0 >> g) & 1u) : ((GPIO_IN1 >> (g - 32u)) & 1u);
}

uint32_t i2s_dbg_pad_edges(uint32_t gpio, uint32_t us)
{
    uint32_t e = 0u, last, cur;
    uint32_t t0 = cpu_cycles(), span = (CPU_HZ / 1000000u) * us;
    last = pad_level(gpio);
    while (cpu_cycles() - t0 < span) {
        cur = pad_level(gpio);
        if (cur != last) { e++; last = cur; }
    }
    return e;
}

uint32_t i2s_dbg_tx_conf(void)  { return I2S_TX_CONF; }
uint32_t i2s_dbg_out_link(void) { return GDMA_OUT_LINK_CH1; }

/*
 * Prove i2s_dbg_pad_edges can SEE a signal on this pin before its silence is
 * believed. Drives the pad from software at a known rate and measures it with
 * the same code path - the tescan lesson, applied to a pin in the upper bank
 * that nothing has ever verified reads back.
 */
uint32_t i2s_dbg_pad_selftest(uint32_t gpio, uint32_t toggles)
{
    uint32_t i, e, save = IO_MUX_GPIO(gpio), savefn = GPIO_FUNC_OUT(gpio);

    IO_MUX_GPIO(gpio) = (1u << IOMUX_MCU_SEL_S) | (3u << IOMUX_FUN_DRV_S)
                      | IOMUX_FUN_IE;
    GPIO_FUNC_OUT(gpio) = 256u;                 /* plain GPIO output */
    if (gpio < 32u) GPIO_ENABLE_W1TS  = (1u << gpio);
    else            GPIO_ENABLE1_W1TS = (1u << (gpio - 32u));

    e = 0u;
    { uint32_t last = pad_level(gpio);
      for (i = 0u; i < toggles; i++) {
          if (gpio < 32u) { if (i & 1u) GPIO_OUT_W1TC = (1u << gpio);
                            else        GPIO_OUT_W1TS = (1u << gpio); }
          else            { if (i & 1u) GPIO_OUT1_W1TC = (1u << (gpio - 32u));
                            else        GPIO_OUT1_W1TS = (1u << (gpio - 32u)); }
          { uint32_t cur = pad_level(gpio);
            if (cur != last) { e++; last = cur; } }
      } }

    IO_MUX_GPIO(gpio) = save;                   /* hand the pin back */
    GPIO_FUNC_OUT(gpio) = savefn;
    return e;
}
uint32_t i2s_dbg_desc_dw0(void) { return g_desc.dw0; }
