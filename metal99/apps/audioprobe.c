/*
 * AUDIOPROBE - is the ES8311 codec actually there, and does it answer?
 *
 * Step one of sound, and deliberately the cheapest one. Making audio work
 * needs an I2S driver written from registers, a codec brought up over I2C, a
 * power amplifier enabled, and PCM to feed it. Only the codec's presence can
 * be tested without any of the rest, so it goes first: if the part does not
 * answer on I2C, nothing built on top of it would have worked either, and the
 * fault would be buried under four layers of new code.
 *
 * The board pairs an ES8311 with the I2S pins in Waveshare's map:
 *
 *     MCLK 16   BCLK 9   WS 45   DOUT 8   DIN 10   PA enable 46
 *
 * The codec sits on the SAME I2C bus as the touch controller, which we already
 * drive - so this costs one app and no new drivers.
 *
 * WHAT COUNTS AS AN ANSWER: registers 0xFD and 0xFE are the chip ID and read
 * 0x83 and 0x11 on a working ES8311. Those are values we can predict in
 * advance rather than rationalise afterwards, which is the standard the panel
 * readback probe had to meet too.
 *
 * APP=audioprobe ./metal99/build.sh
 */
#include <stdint.h>
#include "app.h"
#include "io.h"
#include "i2c.h"
#include "es8311.h"

#define ES8311_ADDR   0x18u   /* 7-bit; the vendor header quotes 0x30, 8-bit */
#define REG_CHIP_ID1  0xFDu
#define REG_CHIP_ID2  0xFEu
#define REG_VERSION   0xFFu

static void scan(void)
{
    uint8_t found[16];
    uint32_t n, i;

    con_puts("\r\naudioprobe: scanning I2C\r\n");
    n = i2c_scan(found, (uint32_t)(sizeof found));
    con_puts("  devices: ");
    for (i = 0u; i < n; i++) { con_hex32(found[i]); con_puts(" "); }
    if (n == 0u) con_puts("(none)");
    con_puts("\r\n");
    con_puts("  expect 0x38 touch (FT3168) and 0x18 codec (ES8311)\r\n");
}

static void identify(void)
{
    uint8_t id1 = 0u, id2 = 0u, ver = 0u;
    int a, b, c;

    if (i2c_probe(ES8311_ADDR) != 0) {
        con_puts("  ES8311 does not ACK at 0x18 - no codec, or wrong address\r\n");
        return;
    }
    a = i2c_read(ES8311_ADDR, REG_CHIP_ID1, &id1, 1u);
    b = i2c_read(ES8311_ADDR, REG_CHIP_ID2, &id2, 1u);
    c = i2c_read(ES8311_ADDR, REG_VERSION,  &ver, 1u);

    con_puts("  ES8311 ACKs. id1="); con_hex32(id1);
    con_puts(" id2=");               con_hex32(id2);
    con_puts(" ver=");               con_hex32(ver);
    con_puts("  rc="); con_dec((int32_t)(a | b | c));
    con_puts("\r\n");

    if (id1 == 0x83u && id2 == 0x11u)
        con_puts("  MATCHES the expected ES8311 chip ID. The part is there.\r\n");
    else
        con_puts("  chip ID does NOT match 0x83/0x11 - something else answered\r\n");
}

static void bringup(void)
{
    int rc;
    con_puts("\r\naudioprobe: bringing the codec up (16 kHz, 16-bit)\r\n");
    rc = es8311_init();
    con_puts("  es8311_init rc="); con_dec((int32_t)rc);
    con_puts("  chip_id=0x"); con_hex32(es8311_chip_id());
    con_puts("\r\n");
    if (rc != ES8311_OK) {
        con_puts("  NOT configured. Nothing above this can work yet.\r\n");
        return;
    }
    /*
     * Every register in the sequence was read back and matched. That means the
     * codec is listening and configured - it does NOT mean anything is
     * audible, because no I2S clock or data is reaching it yet. Distinguishing
     * those two states before writing an I2S driver is the whole point of
     * doing the codec first.
     */
    con_puts("  every register verified by read-back. Codec is configured.\r\n");
    con_puts("  still silent: no MCLK/BCLK/WS and no data - that is i2s.c.\r\n");
}

static void ap_init(void)
{
    scan();
    identify();
    bringup();
    con_puts("audioprobe: done\r\n");
}

static int ap_frame(uint32_t f) { (void)f; return 0; }

const app_t APP = { "audioprobe", 1u, ap_init, ap_frame, 0 };
