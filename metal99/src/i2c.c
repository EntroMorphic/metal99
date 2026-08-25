#include "i2c.h"
#include <stddef.h>
#include "io.h"

/* ---------------------------------------------------------------- bases */
#define SYSTEM_PERIP_CLK_EN0 REG32(0x600C0018u)
#define SYSTEM_PERIP_RST_EN0 REG32(0x600C0020u)
#define SYSTEM_I2C0_BIT      (1u << 7)     /* SYSTEM_I2C_EXT0_CLK_EN */

#define GPIO_BASE            0x60004000u
#define GPIO_ENABLE_W1TS     REG32(GPIO_BASE + 0x24u)
#define GPIO_PIN(n)          REG32(GPIO_BASE + 0x74u + 4u * (uint32_t)(n))
#define GPIO_FUNC_OUT_SEL(n) REG32(GPIO_BASE + 0x554u + 4u * (uint32_t)(n))
#define GPIO_FUNC_IN_SEL(s)  REG32(GPIO_BASE + 0x154u + 4u * (uint32_t)(s))
#define IO_MUX_GPIO(n)       REG32(0x60009004u + 4u * (uint32_t)(n))

#define PAD_DRIVER           (1u << 2)     /* open drain */
#define GPIO_SIG_IN_SEL      (1u << 7)     /* take the input via the matrix */
#define IOMUX_MCU_SEL_S      12
#define IOMUX_FUN_DRV_S      10
#define IOMUX_FUN_IE         (1u << 9)
#define IOMUX_FUN_PU         (1u << 8)

/* Board wiring, from the Waveshare BSP. There is NO touch reset pin, exactly
 * as there is none for the panel. */
#define PIN_SCL 14
#define PIN_SDA 15
#define SIG_I2CEXT0_SCL 89
#define SIG_I2CEXT0_SDA 90

#define I2C_BASE             0x60013000u
#define I2C_SCL_LOW          REG32(I2C_BASE + 0x00u)
#define I2C_CTR              REG32(I2C_BASE + 0x04u)
#define I2C_SR               REG32(I2C_BASE + 0x08u)
#define I2C_TO               REG32(I2C_BASE + 0x0Cu)
#define I2C_FIFO_CONF        REG32(I2C_BASE + 0x18u)
#define I2C_DATA             REG32(I2C_BASE + 0x1Cu)
#define I2C_INT_RAW          REG32(I2C_BASE + 0x20u)
#define I2C_INT_CLR          REG32(I2C_BASE + 0x24u)
#define I2C_SDA_HOLD         REG32(I2C_BASE + 0x30u)
#define I2C_SDA_SAMPLE       REG32(I2C_BASE + 0x34u)
#define I2C_SCL_HIGH         REG32(I2C_BASE + 0x38u)
#define I2C_SCL_START_HOLD   REG32(I2C_BASE + 0x40u)
#define I2C_SCL_RSTART_SETUP REG32(I2C_BASE + 0x44u)
#define I2C_SCL_STOP_HOLD    REG32(I2C_BASE + 0x48u)
#define I2C_SCL_STOP_SETUP   REG32(I2C_BASE + 0x4Cu)
#define I2C_FILTER_CFG       REG32(I2C_BASE + 0x50u)
#define I2C_CLK_CONF         REG32(I2C_BASE + 0x54u)
#define I2C_COMD(i)          REG32(I2C_BASE + 0x58u + 4u * (uint32_t)(i))

/* CTR */
#define CTR_SDA_FORCE_OUT    (1u << 0)
#define CTR_SCL_FORCE_OUT    (1u << 1)
#define CTR_SAMPLE_SCL_LEVEL (1u << 2)
#define CTR_RX_FULL_ACK_LVL  (1u << 3)
#define CTR_MS_MODE          (1u << 4)
#define CTR_TRANS_START      (1u << 5)
#define CTR_CLK_EN           (1u << 8)
#define CTR_FSM_RST          (1u << 10)
#define CTR_CONF_UPGATE      (1u << 11)
#define CLK_SCLK_ACTIVE      (1u << 21)

/* SR / INT */
#define SR_BUS_BUSY          (1u << 4)
#define SR_RXFIFO_CNT_S      8
#define SR_RXFIFO_CNT_M      0x3Fu
#define INT_ARBITRATION_LOST (1u << 5)
#define INT_TRANS_COMPLETE   (1u << 7)
#define INT_TIME_OUT         (1u << 8)
#define INT_NACK             (1u << 10)
#define INT_SCL_ST_TO        (1u << 13)
#define INT_SCL_MAIN_ST_TO   (1u << 14)

#define FIFO_RXFIFO_RST      (1u << 0)
#define FIFO_TXFIFO_RST      (1u << 1)
#define FIFO_NONFIFO_EN      (1u << 10)

/*
 * WRITING ZERO TO A REGISTER IS NOT NEUTRAL.
 *
 * I2C_FIFO_CONF resets to RXFIFO_WM_THRHD=11, TXFIFO_WM_THRHD=4, FIFO_PRT_EN=1.
 * Clearing it wholesale to pulse the two reset bits also cleared FIFO_PRT_EN,
 * which controls the FIFO pointers: one byte went out, the pointers stopped
 * advancing, and the transaction stalled with the rest stuck in the FIFO and
 * SCL_ST_TO raised. A probe survived it - one byte is all a probe sends - so
 * the bus looked healthy right up until the first real read.
 *
 * The same mistake as the missing SCLK_ACTIVE a few lines up, and worth naming
 * as a class: a bare literal into a register whose reset value carries meaning
 * clobbers whatever the author did not know was there.
 */
#define FIFO_DEFAULTS        ((11u << 0) | (4u << 5) | (1u << 14))

/* Command opcodes, from the S3 TRM via hal/i2c_ll.h. */
#define OP_RSTART 6u
#define OP_WRITE  1u
#define OP_READ   3u
#define OP_STOP   2u
#define CMD(op, ack_check, ack_val, n) \
    (((op) << 11) | ((ack_val) << 10) | ((ack_check) << 8) | ((n) & 0xFFu))

/* Bounded, for the same reason every spin in spi2.c is: a wedged bus must not
 * take the render loop with it. ~40 ms at 160 MHz, far beyond a 6-byte read at
 * 400 kHz (~150 us). */
#define I2C_SPIN_LIMIT 400000u

static void route_pin(uint32_t gpio, uint32_t sig)
{
    /* Open drain with a pull-up, input buffer on. I2C needs all three, which is
     * what makes this different from the SPI pins: SDA is bidirectional and
     * both lines are released high rather than driven. The board carries its
     * own pull-ups; the internal ones are belt and braces. */
    IO_MUX_GPIO(gpio) = (1u << IOMUX_MCU_SEL_S)
                      | (2u << IOMUX_FUN_DRV_S)
                      | IOMUX_FUN_IE | IOMUX_FUN_PU;
    GPIO_PIN(gpio)    = PAD_DRIVER;
    GPIO_ENABLE_W1TS  = (1u << gpio);
    GPIO_FUNC_OUT_SEL(gpio) = sig & 0x1FFu;
    /* BIT 7 IS LOAD-BEARING. GPIO_SIG_IN_SEL routes the peripheral's input
     * through the GPIO matrix; without it the controller reads the IO_MUX
     * direct path instead of the pad, never sees SDA, and therefore never sees
     * an ACK - which presents as "no device at 0x38" with a perfectly healthy
     * bus. spi2.c does not need this because SPI2 only ever drives. */
    GPIO_FUNC_IN_SEL(sig)   = (gpio & 0x3Fu) | GPIO_SIG_IN_SEL;
}

#define GPIO_OUT_W1TS  REG32(GPIO_BASE + 0x08u)
#define GPIO_OUT_W1TC  REG32(GPIO_BASE + 0x0Cu)
#define GPIO_IN_REG    REG32(GPIO_BASE + 0x3Cu)
#define SIG_GPIO_OUT   256u

/*
 * BUS RECOVERY: clock a stuck slave until it lets go of SDA.
 *
 * This is why touch bring-up was intermittent, and the symptom was thoroughly
 * misleading. A reset - every flash cycle is one - can land in the middle of a
 * byte the FT3168 is transmitting. The controller is then still driving SDA low
 * for a bit it will never get to finish, because the master that was clocking
 * it has vanished. SDA stays low forever.
 *
 * To a master that then starts up, a permanently-low SDA reads as an ACK from
 * everything: a scan reported EIGHT devices at 0x08, 0x0A, 0x0B, 0x0C... which
 * looks like a bus full of hardware and is really a bus full of nothing. Other
 * boots showed zero devices, or worked perfectly - depending only on where in a
 * transaction the previous reset happened to land.
 *
 * The fix is the standard one and belongs in init, not in a retry loop: take
 * the pins as plain open-drain GPIO, and if SDA is low, pulse SCL up to nine
 * times. Nine because a byte plus its ACK is nine bits, so that is the most a
 * slave can still be holding. Each pulse lets it shift out one more bit; when
 * it reaches the end it releases. Then a manual STOP puts it back in idle.
 */
static uint32_t g_lines_before, g_lines_after, g_recover_pulses;

/* Bit 0 = SCL level, bit 1 = SDA level. Both should read 1 on an idle bus. */
uint32_t i2c_dbg_lines_before(void) { return g_lines_before; }
uint32_t i2c_dbg_lines_after(void)  { return g_lines_after; }
uint32_t i2c_dbg_pulses(void)       { return g_recover_pulses; }

static uint32_t read_lines(void)
{
    uint32_t v = GPIO_IN_REG;
    return (((v >> PIN_SCL) & 1u) << 0) | (((v >> PIN_SDA) & 1u) << 1);
}

static void i2c_bus_recover(void)
{
    int i;

    IO_MUX_GPIO(PIN_SCL) = (1u << IOMUX_MCU_SEL_S) | (2u << IOMUX_FUN_DRV_S)
                         | IOMUX_FUN_IE | IOMUX_FUN_PU;
    IO_MUX_GPIO(PIN_SDA) = (1u << IOMUX_MCU_SEL_S) | (2u << IOMUX_FUN_DRV_S)
                         | IOMUX_FUN_IE | IOMUX_FUN_PU;
    GPIO_PIN(PIN_SCL) = PAD_DRIVER;          /* open drain */
    GPIO_PIN(PIN_SDA) = PAD_DRIVER;
    GPIO_FUNC_OUT_SEL(PIN_SCL) = SIG_GPIO_OUT;
    GPIO_FUNC_OUT_SEL(PIN_SDA) = SIG_GPIO_OUT;
    GPIO_ENABLE_W1TS = (1u << PIN_SCL) | (1u << PIN_SDA);
    GPIO_OUT_W1TS    = (1u << PIN_SCL) | (1u << PIN_SDA);   /* release both */
    delay_us(10u);
    g_lines_before = read_lines();

    /*
     * ALWAYS nine clocks, not only when SDA reads low.
     *
     * Conditioning on SDA was the obvious reading of "the bus is stuck" and it
     * is not sufficient: a slave stranded part-way through a byte may happen to
     * be holding a bit that is HIGH, so SDA looks idle while the device is still
     * waiting for clocks it will never otherwise get. Boot-to-boot this showed
     * up as touch working perfectly, or reporting absent, with nothing in the
     * firmware differing between the two - only where the previous reset landed
     * inside a transaction.
     *
     * Nine clocks unconditionally is harmless on a healthy bus: with SDA
     * released, an idle slave sees no START and ignores them entirely.
     */
    for (i = 0; i < 9; i++) {
        GPIO_OUT_W1TC = (1u << PIN_SCL);     /* drive SCL low  */
        delay_us(5u);
        GPIO_OUT_W1TS = (1u << PIN_SCL);     /* release: pulled up */
        delay_us(5u);
    }

    /* STOP: SDA rises while SCL is high. Leaves any slave that was mid-byte in
     * a defined idle rather than half-way through one. */
    GPIO_OUT_W1TC = (1u << PIN_SDA); delay_us(5u);
    GPIO_OUT_W1TS = (1u << PIN_SCL); delay_us(5u);
    GPIO_OUT_W1TS = (1u << PIN_SDA); delay_us(5u);
    g_recover_pulses = (uint32_t)i;
    g_lines_after = read_lines();
}

/*
 * Full peripheral configuration. Separated from i2c_init so that recovery can
 * restore ALL of it.
 *
 * FSM_RST alone was not enough: it resets the state machine, and the timing and
 * mode registers around it are not guaranteed to survive in a state the next
 * transaction can use. So a single NACK could leave the controller unable to
 * complete anything, and every subsequent retry failed for a reason that had
 * nothing to do with the device - which reads exactly like a part that is not
 * there.
 */
static void i2c_configure(void)
{

    SYSTEM_PERIP_CLK_EN0 = SYSTEM_PERIP_CLK_EN0 | SYSTEM_I2C0_BIT;
    SYSTEM_PERIP_RST_EN0 = SYSTEM_PERIP_RST_EN0 | SYSTEM_I2C0_BIT;
    SYSTEM_PERIP_RST_EN0 = SYSTEM_PERIP_RST_EN0 & ~SYSTEM_I2C0_BIT;

    /* Pins BEFORE the peripheral is told to drive them. */
    route_pin(PIN_SCL, SIG_I2CEXT0_SCL);
    route_pin(PIN_SDA, SIG_I2CEXT0_SDA);

    /* Exactly what IDF's i2c_ll_master_init sets, and nothing else.
     * SAMPLE_SCL_LEVEL was in here and is not in IDF's: it moves SDA sampling
     * to the falling edge, which is a good way to miss an ACK. */
    I2C_CTR = CTR_CLK_EN | CTR_MS_MODE | CTR_SDA_FORCE_OUT | CTR_SCL_FORCE_OUT;

    /*
     * SCLK_ACTIVE (bit 21) IS THE WHOLE THING.
     *
     * Source = XTAL 40 MHz, divider 1 - the PLL is up by now, but XTAL is the
     * steady reference and spi2 makes the same choice for the same reason. The
     * first version wrote (0 << 20) | 0, which is zero, which leaves SCLK_ACTIVE
     * clear: the state machine has no clock, never drives a bit, and every
     * address on the bus reads as absent. An i2c_scan() found nothing at all,
     * which is what pointed here - a wiring or address fault would have left
     * SOMETHING answering.
     */
    I2C_CLK_CONF = CLK_SCLK_ACTIVE | (0u << 20) | 0u;

    /*
     * 400 kHz from 40 MHz XTAL. half_cycle = 40e6 / 400e3 / 2 = 50.
     *
     * TRANSCRIBED from IDF's i2c_ll_master_cal_bus_freq and
     * i2c_ll_master_set_bus_timing, including which fields are written minus
     * one, because they were GUESSED the first time and the guess was wrong in
     * a way that still half-worked. sda_sample was set to half_cycle/8 = 6 when
     * the formula is half_cycle/2 = 25, which sampled SDA long before the data
     * was valid: an address probe would ACK or not more or less at random, and
     * a bus scan reported seven devices at nonsense addresses.
     *
     * The hardware states its own assumption, and it is worth keeping in view:
     *
     *     scl_wait_high (23) < sda_sample (25) < scl_high (27)
     *
     * Those three are 4 cycles apart at 400 kHz. There is no room to improvise.
     */
    I2C_SCL_LOW          = 50u - 1u;              /* scl_low  - 1            */
    I2C_SCL_HIGH         = (23u << 9) | 27u;      /* wait_high << 9 | high   */
    I2C_SDA_HOLD         = 12u - 1u;              /* half/4   - 1            */
    I2C_SDA_SAMPLE       = 25u - 1u;              /* half/2   - 1            */
    I2C_SCL_RSTART_SETUP = 50u - 1u;
    I2C_SCL_STOP_SETUP   = 50u - 1u;
    I2C_SCL_START_HOLD   = 50u - 1u;
    I2C_SCL_STOP_HOLD    = 50u - 1u;
    I2C_FILTER_CFG       = (1u << 3) | 1u;        /* SCL and SDA filters on  */
    I2C_TO               = (1u << 5) | 10u;       /* enable, ~10 bus cycles  */

    I2C_FIFO_CONF = FIFO_DEFAULTS | FIFO_RXFIFO_RST | FIFO_TXFIFO_RST;
    I2C_FIFO_CONF = FIFO_DEFAULTS;             /* FIFO mode, pointers enabled */
    I2C_INT_CLR   = 0xFFFFFFFFu;

    /* S3 latches I2C config only on CONF_UPGATE - the same trap SPI_UPDATE is,
     * and skipping it silently transacts with the previous timing. */
    I2C_CTR = I2C_CTR | CTR_CONF_UPGATE;
}

void i2c_init(void)
{
    /* Clock the bus clear before touching the peripheral: a slave left
     * mid-byte by the last reset is still waiting for the rest of it. */
    i2c_bus_recover();
    i2c_configure();
}

/* Run the command list already loaded into COMD0.. and wait for it. */
static void i2c_configure(void);

static uint32_t g_last_int, g_last_sr;

uint32_t i2c_dbg_int(void) { return g_last_int; }
uint32_t i2c_dbg_sr(void)  { return g_last_sr; }

/*
 * Put the controller back in a usable state after a failed transaction.
 *
 * Without this ONE failure wedges the bus for good: SR keeps BUS_BUSY set,
 * every later bus_idle() times out, and the diagnostics then report an empty
 * bus - which is how the first debugging round ended up reading the scan's
 * status instead of the read's and chasing the wrong register.
 */
static void i2c_recover(void)
{
    I2C_CTR = I2C_CTR | CTR_FSM_RST;
    I2C_INT_CLR = 0xFFFFFFFFu;
    /* Re-apply everything, not just the FSM. See i2c_configure(). */
    i2c_configure();
}

static int i2c_run(void)
{
    uint32_t guard = 0u, st;

    I2C_INT_CLR = 0xFFFFFFFFu;
    I2C_CTR = I2C_CTR | CTR_CONF_UPGATE;
    I2C_CTR = I2C_CTR | CTR_TRANS_START;

    for (;;) {
        st = I2C_INT_RAW;
        g_last_int = st; g_last_sr = I2C_SR;
        if ((st & INT_NACK) != 0u)             { i2c_recover(); return I2C_E_NACK; }
        if ((st & INT_ARBITRATION_LOST) != 0u) { i2c_recover(); return I2C_E_ARB; }
        if ((st & (INT_TIME_OUT | INT_SCL_ST_TO | INT_SCL_MAIN_ST_TO)) != 0u)
                                               { i2c_recover(); return I2C_E_TIMEOUT; }
        if ((st & INT_TRANS_COMPLETE) != 0u)     return I2C_OK;
        if (++guard > I2C_SPIN_LIMIT)          { i2c_recover(); return I2C_E_TIMEOUT; }
    }
}

static int bus_idle(void)
{
    uint32_t guard = 0u;
    while ((I2C_SR & SR_BUS_BUSY) != 0u) {
        if (++guard > I2C_SPIN_LIMIT) return I2C_E_BUSY;
    }
    return I2C_OK;
}

static void fifo_reset(void)
{
    I2C_FIFO_CONF = FIFO_DEFAULTS | FIFO_RXFIFO_RST | FIFO_TXFIFO_RST;
    I2C_FIFO_CONF = FIFO_DEFAULTS;
}

/*
 * Which addresses answer. An instrument, not a feature: when touch_init()
 * reports nothing at 0x38, this separates "the bus is dead" from "something is
 * there but not where expected", which are different bugs with different fixes.
 * Skips the reserved ranges at both ends.
 */
uint32_t i2c_scan(uint8_t *found, uint32_t max)
{
    uint32_t n = 0u, a;
    for (a = 0x08u; a <= 0x77u && n < max; a++) {
        /* Settle between probes. Back to back, a probe that NACKs is followed
         * by an FSM reset and the next transaction can report a completion the
         * bus never produced - which made this scan invent eight devices on a
         * bus carrying two. A diagnostic that lies is worse than none. */
        if (i2c_probe((uint8_t)a) == I2C_OK) found[n++] = (uint8_t)a;
        delay_us(200u);
    }
    return n;
}

int i2c_probe(uint8_t addr)
{
    int rc = bus_idle();
    if (rc != I2C_OK) return rc;
    fifo_reset();

    I2C_DATA = (uint32_t)((addr << 1) | 0u);
    I2C_COMD(0) = CMD(OP_RSTART, 0u, 0u, 0u);
    I2C_COMD(1) = CMD(OP_WRITE,  1u, 0u, 1u);   /* ack_check: NACK => absent */
    I2C_COMD(2) = CMD(OP_STOP,   0u, 0u, 0u);
    return i2c_run();
}

int i2c_read(uint8_t addr, uint8_t reg, uint8_t *buf, uint32_t n)
{
    uint32_t i;
    int rc;

    if (buf == NULL || n == 0u || n > (uint32_t)I2C_MAX_READ) return I2C_E_LEN;
    rc = bus_idle();
    if (rc != I2C_OK) return rc;
    fifo_reset();

    /* Address and register go in the TX FIFO before the list runs. */
    I2C_DATA = (uint32_t)((addr << 1) | 0u);
    I2C_DATA = (uint32_t)reg;
    I2C_DATA = (uint32_t)((addr << 1) | 1u);

    I2C_COMD(0) = CMD(OP_RSTART, 0u, 0u, 0u);
    I2C_COMD(1) = CMD(OP_WRITE,  1u, 0u, 2u);            /* addr+W, reg      */
    I2C_COMD(2) = CMD(OP_RSTART, 0u, 0u, 0u);            /* repeated START   */
    I2C_COMD(3) = CMD(OP_WRITE,  1u, 0u, 1u);            /* addr+R           */
    if (n > 1u) {
        I2C_COMD(4) = CMD(OP_READ, 0u, 0u, n - 1u);      /* ACK all but last */
        I2C_COMD(5) = CMD(OP_READ, 0u, 1u, 1u);          /* NACK the last    */
        I2C_COMD(6) = CMD(OP_STOP, 0u, 0u, 0u);
    } else {
        I2C_COMD(4) = CMD(OP_READ, 0u, 1u, 1u);
        I2C_COMD(5) = CMD(OP_STOP, 0u, 0u, 0u);
    }

    rc = i2c_run();
    if (rc != I2C_OK) return rc;

    /* Trust the FIFO count, not the requested length: a short answer must read
     * as short rather than as stale bytes left from a previous transaction. */
    if (((I2C_SR >> SR_RXFIFO_CNT_S) & SR_RXFIFO_CNT_M) < n) return I2C_E_TIMEOUT;
    for (i = 0u; i < n; i++) buf[i] = (uint8_t)(I2C_DATA & 0xFFu);
    return I2C_OK;
}

int i2c_write1(uint8_t addr, uint8_t reg, uint8_t val)
{
    int rc = bus_idle();
    if (rc != I2C_OK) return rc;
    fifo_reset();

    I2C_DATA = (uint32_t)((addr << 1) | 0u);
    I2C_DATA = (uint32_t)reg;
    I2C_DATA = (uint32_t)val;
    I2C_COMD(0) = CMD(OP_RSTART, 0u, 0u, 0u);
    I2C_COMD(1) = CMD(OP_WRITE,  1u, 0u, 3u);
    I2C_COMD(2) = CMD(OP_STOP,   0u, 0u, 0u);
    return i2c_run();
}
