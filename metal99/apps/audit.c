/*
 * AUDIT - read every register in the audio path back and print it beside what
 * it should be. Silent: the amplifier is never enabled.
 *
 * Seven rounds of "change something, have a listen" produced four real fixes
 * and no working audio, and each round cost a person their evening. The
 * decoded samples are now PROVEN bit-identical to ffmpeg's, so the fault is
 * downstream of the I2S peripheral - which is a region made entirely of
 * register bits, and register bits can be read.
 *
 * Expected values come from the vendor driver's {12288000, 48000} coefficient
 * row and from the ESP32-S3 TRM, not from what this project happens to write:
 * comparing the code against itself would pass no matter what.
 *
 * APP=audit ./metal99/build.sh
 */
#include <stdint.h>
#include "app.h"
#include "io.h"
#include "i2c.h"
#include "es8311.h"
#include "i2s.h"
#include "sfx.h"

#define ES 0x18u

#define I2S0                 0x6000F000u
#define R32(a)               REG32(a)

typedef struct { uint8_t reg; uint8_t want; const char *what; } expect;

/*
 * The 48 kHz configuration, derived from the vendor's coefficient row
 * {12288000, 48000, pre_div 1, mult 1, adc_div 1, dac_div 1, fs_mode 0,
 *  lrck 0x00ff, bclk_div 4, adc_osr 0x10, dac_osr 0x10} and its register
 * writer. 0xFF in `want` means "no fixed expectation, just show it".
 */
static const expect EXPECT[] = {
    { 0x00u, 0x80u, "RESET     slave, out of reset" },
    { 0x01u, 0x3Fu, "CLK1      all six block clocks on" },
    { 0x02u, 0x00u, "CLK2      pre_div 1, pre_mult 1" },
    { 0x03u, 0x10u, "CLK3      fs_mode 0 | adc_osr 0x10" },
    { 0x04u, 0x10u, "CLK4      0x10 | dac_osr 0x10   <- rate-specific" },
    { 0x05u, 0x00u, "CLK5      adc_div 1, dac_div 1" },
    { 0x06u, 0x03u, "CLK6      bclk_div-1 = 3, sclk NOT inverted" },
    { 0x07u, 0x00u, "CLK7      lrck high byte" },
    { 0x08u, 0xFFu, "CLK8      lrck low byte" },
    { 0x09u, 0x0Cu, "SDPIN     16-bit, I2S format, not muted" },
    { 0x0Au, 0x0Cu, "SDPOUT    16-bit, I2S format" },
    { 0x0Bu, 0x00u, "SYS0B" },
    { 0x0Cu, 0x00u, "SYS0C" },
    { 0x0Du, 0x01u, "SYS0D     analog powered up" },
    { 0x0Eu, 0x02u, "SYS0E" },
    { 0x10u, 0x1Fu, "SYS10" },
    { 0x11u, 0x7Fu, "SYS11" },
    { 0x12u, 0x00u, "SYS12     DAC enabled" },
    { 0x13u, 0x10u, "SYS13" },
    { 0x14u, 0x1Au, "SYS14" },
    { 0x31u, 0x00u, "DAC31     NOT muted  <- defaults muted" },
    { 0x32u, 0xFFu, "DAC32     volume (whatever we set)" },
    { 0x37u, 0x08u, "DAC37" },
    { 0x44u, 0x08u, "GPIO44    DAC data from serial port" }
};

static void codec_dump(void)
{
    uint32_t i;
    int bad = 0;

    con_puts("\r\n== ES8311 registers ==\r\n");
    for (i = 0u; i < (uint32_t)(sizeof EXPECT / sizeof EXPECT[0]); i++) {
        uint8_t got = 0u;
        int rc = i2c_read(ES, EXPECT[i].reg, &got, 1u);
        con_puts("  0x"); con_hex32(EXPECT[i].reg);
        con_puts(" = 0x");  con_hex32(got);
        if (rc != 0) { con_puts("  READ FAILED"); bad++; }
        else if (EXPECT[i].want != 0xFFu && got != EXPECT[i].want) {
            con_puts("  WANT 0x"); con_hex32(EXPECT[i].want);
            con_puts("  <<<< MISMATCH"); bad++;
        }
        con_puts("  "); con_puts(EXPECT[i].what); con_puts("\r\n");
    }
    con_puts("  mismatches: "); con_dec((int32_t)bad); con_puts("\r\n");
}

static void i2s_dump(void)
{
    con_puts("\r\n== I2S0 TX ==\r\n");
    con_puts("  TX_CONF      0x"); con_hex32(R32(I2S0 + 0x24u));
    con_puts("   want bit2 START, bit19 TDM_EN, bit20 PDM clear\r\n");
    con_puts("  TX_CONF1     0x"); con_hex32(R32(I2S0 + 0x2Cu));
    con_puts("   bck_div-1=7, bits_mod=15, chan_bits=15, half=15, ws=15, MSB_SHIFT\r\n");
    con_puts("  TX_CLKM_CONF 0x"); con_hex32(R32(I2S0 + 0x34u));
    con_puts("   div_num=13, clk_sel=2 (PLL160), CLK_ACTIVE, CLK_EN\r\n");
    con_puts("  TX_CLKM_DIV  0x"); con_hex32(R32(I2S0 + 0x3Cu));
    con_puts("   x=47 y=0 z=1  (fraction 1/48)\r\n");
    con_puts("  TX_TDM_CTRL  0x"); con_hex32(R32(I2S0 + 0x54u));
    con_puts("   chan0+chan1 enabled, tot_chan_num-1 = 1\r\n");
}

static void clocks(void)
{
    con_puts("\r\n== measured on the pads (2 ms) ==\r\n");
    con_puts("  MCLK  gpio16 "); con_dec((int32_t)i2s_dbg_pad_edges(16u, 2000u));
    con_puts("   12.288 MHz aliases; nonzero is what matters\r\n");
    con_puts("  BCLK  gpio9  "); con_dec((int32_t)i2s_dbg_pad_edges(9u, 2000u));
    con_puts("   want ~6144  (1.536 MHz)\r\n");
    con_puts("  WS    gpio45 "); con_dec((int32_t)i2s_dbg_pad_edges(45u, 2000u));
    con_puts("   want ~192   (48 kHz)\r\n");
    con_puts("  DOUT  gpio8  "); con_dec((int32_t)i2s_dbg_pad_edges(8u, 2000u));
    con_puts("   nonzero while audio is queued\r\n");
}

static void aud_init(void)
{
    con_puts("\r\naudit: reading the audio path back. SILENT - amp stays off.\r\n");
    i2c_init();
    { int rc = sfx_init();
      con_puts("  sfx_init rc="); con_dec((int32_t)rc); con_puts("\r\n"); }
    es8311_amp(0);                      /* whatever sfx_init did, undo it */

    codec_dump();
    i2s_dump();
    sfx_play(SFX_FIRE);                 /* queue audio so DOUT has traffic */
    sfx_service();
    clocks();
    con_puts("\r\naudit: done\r\n");
}

static int aud_frame(uint32_t f) { (void)f; sfx_service(); return 0; }

const app_t APP = { "audit", 10u, aud_init, aud_frame, 0 };
