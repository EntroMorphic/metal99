/* ES8311 bring-up. See es8311.h for the wiring and why it is verifiable. */
#include <stddef.h>
#include "es8311.h"
#include "i2c.h"
#include "io.h"

/* ---------------------------------------------------------------- pins */
#define PIN_PA 46u                    /* speaker amplifier enable */

#define GPIO_BASE          0x60004000u
#define GPIO_OUT_W1TS      REG32(GPIO_BASE + 0x08u)
#define GPIO_OUT_W1TC      REG32(GPIO_BASE + 0x0Cu)
#define GPIO_ENABLE1_W1TS  REG32(GPIO_BASE + 0x30u)   /* GPIO 32..48 */
#define GPIO_OUT1_W1TS     REG32(GPIO_BASE + 0x14u)
#define GPIO_OUT1_W1TC     REG32(GPIO_BASE + 0x18u)
#define GPIO_FUNC_OUT(n)   REG32(GPIO_BASE + 0x554u + 4u * (uint32_t)(n))
#define IO_MUX_GPIO(n)     REG32(0x60009004u + 4u * (uint32_t)(n))
#define IOMUX_MCU_SEL_S    12
#define IOMUX_FUN_DRV_S    10
#define SIG_GPIO_OUT       256u

/* ------------------------------------------------------------ registers */
#define R_RESET      0x00u
#define R_CLK1       0x01u
#define R_CLK2       0x02u
#define R_CLK3       0x03u
#define R_CLK4       0x04u
#define R_CLK5       0x05u
#define R_CLK6       0x06u
#define R_CLK7       0x07u
#define R_CLK8       0x08u
#define R_SDPIN      0x09u
#define R_SDPOUT     0x0Au
#define R_SYS0B      0x0Bu
#define R_SYS0C      0x0Cu
#define R_SYS0D      0x0Du
#define R_SYS10      0x10u
#define R_SYS11      0x11u
#define R_SYS12      0x12u
#define R_SYS13      0x13u
#define R_SYS14      0x14u
#define R_ADC16      0x16u
#define R_ADC1B      0x1Bu
#define R_ADC1C      0x1Cu
#define R_DAC31      0x31u
#define R_DAC32      0x32u
#define R_DAC37      0x37u
#define R_GPIO44     0x44u
#define R_CHIP_ID1   0xFDu
#define R_CHIP_ID2   0xFEu

static uint16_t g_id;

uint16_t es8311_chip_id(void) { return g_id; }

/*
 * Every write is read back.
 *
 * Not all registers read back what was written - some have reserved or
 * self-clearing bits - so `verify` says whether this one is expected to. The
 * point is that a codec which ACKs but ignores writes looks exactly like one
 * that is working, right up until there is no sound and four other layers are
 * suspects. Catching it here costs one extra I2C read per register.
 */
static int wr(uint8_t reg, uint8_t val, int verify)
{
    uint8_t back = 0u;
    if (i2c_write1(ES8311_I2C_ADDR, reg, val) != 0) return ES8311_E_IO;
    if (!verify) return ES8311_OK;
    if (i2c_read(ES8311_I2C_ADDR, reg, &back, 1u) != 0) return ES8311_E_IO;
    return (back == val) ? ES8311_OK : ES8311_E_VERIFY;
}

/*
 * 16 kHz from a 4.096 MHz MCLK, which is 256*fs - the ratio the ESP32 side is
 * configured to produce. Taken from the vendor coefficient table's
 * {4096000, 16000} row: pre_div 1, mult 1, adc/dac div 1, fs_mode 0,
 * lrck 0x00ff, bclk_div 4, adc_osr 0x10, dac_osr 0x20.
 *
 * Written as literals rather than a table lookup because there is exactly one
 * rate. A second rate should bring the table with it, not a second set of
 * magic numbers.
 */
static const struct { uint8_t reg, val, verify; } INIT[] = {
    { R_SYS0D,  0xFAu, 1 },   /* hold powered down while we configure       */
    { R_GPIO44, 0x08u, 1 },   /* DAC takes its data from the serial port    */
    { R_CLK1,   0x30u, 1 },   /* clock manager: MCLK from the pad           */
    { R_CLK2,   0x00u, 1 },   /* pre_div 1, pre_mult 1                      */
    { R_CLK3,   0x10u, 1 },   /* fs_mode 0 | adc_osr 0x10                   */
    { R_ADC16,  0x24u, 1 },
    { R_CLK4,   0x20u, 1 },   /* dac_osr 0x20                               */
    { R_CLK5,   0x00u, 1 },   /* adc_div 1, dac_div 1                       */
    { R_CLK7,   0x00u, 1 },   /* lrck high byte                             */
    { R_CLK8,   0xFFu, 1 },   /* lrck low byte                              */
    { R_CLK6,   0x03u, 1 },   /* bclk_div - 1                               */
    { R_SYS0B,  0x00u, 1 },
    { R_SYS0C,  0x00u, 1 },
    { R_SYS10,  0x1Fu, 1 },
    { R_SYS11,  0x7Fu, 1 },
    { R_RESET,  0x80u, 0 },   /* leave reset; slave mode. Self-clearing bits */
    { R_SDPIN,  0x0Cu, 1 },   /* DAC serial port: I2S, 16-bit               */
    { R_SDPOUT, 0x0Cu, 1 },   /* ADC likewise - unused, but left consistent */
    { R_SYS13,  0x10u, 1 },
    { R_ADC1B,  0x0Au, 1 },
    { R_ADC1C,  0x6Au, 1 },
    { R_SYS12,  0x00u, 1 },   /* DAC enabled                                */
    { R_SYS14,  0x1Au, 1 },
    { R_DAC37,  0x08u, 1 },
    { R_DAC32,  0x00u, 1 },   /* start muted - the amp is enabled separately */
    { R_SYS0D,  0x01u, 1 }    /* power up analog                            */
};

void es8311_amp(int on)
{
    /* GPIO46 is above 31, so it lives in the second bank's registers. */
    IO_MUX_GPIO(PIN_PA) = (1u << IOMUX_MCU_SEL_S) | (2u << IOMUX_FUN_DRV_S);
    GPIO_FUNC_OUT(PIN_PA) = SIG_GPIO_OUT;
    GPIO_ENABLE1_W1TS = (1u << (PIN_PA - 32u));
    if (on) GPIO_OUT1_W1TS = (1u << (PIN_PA - 32u));
    else    GPIO_OUT1_W1TC = (1u << (PIN_PA - 32u));
}

int es8311_volume(uint8_t level)
{
    return wr(R_DAC32, level, 1);
}

int es8311_init(void)
{
    uint8_t id1 = 0u, id2 = 0u;
    uint32_t i;

    es8311_amp(0);                    /* silent through the power-up transient */

    if (i2c_probe(ES8311_I2C_ADDR) != 0) return ES8311_E_ABSENT;
    if (i2c_read(ES8311_I2C_ADDR, R_CHIP_ID1, &id1, 1u) != 0) return ES8311_E_IO;
    if (i2c_read(ES8311_I2C_ADDR, R_CHIP_ID2, &id2, 1u) != 0) return ES8311_E_IO;
    g_id = (uint16_t)(((uint16_t)id1 << 8) | id2);
    if (g_id != 0x8311u) return ES8311_E_ID;

    for (i = 0u; i < (uint32_t)(sizeof INIT / sizeof INIT[0]); i++) {
        int rc = wr(INIT[i].reg, INIT[i].val, (int)INIT[i].verify);
        if (rc != ES8311_OK) {
            /* Which register, not just that something failed. Bring-up spent
             * three rounds on the wrong stage of the SPI transport for want of
             * exactly this. */
            con_puts("  es8311: reg 0x"); con_hex32(INIT[i].reg);
            con_puts(" rc="); con_dec((int32_t)rc); con_puts("\r\n");
            return rc;
        }
        delay_us(50u);
    }
    return ES8311_OK;
}
