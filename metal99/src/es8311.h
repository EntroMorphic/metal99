/*
 * ES8311 audio codec, over the I2C bus the touch controller already uses.
 *
 * The ESP32 is the I2S MASTER and drives MCLK, BCLK and WS; the codec is the
 * slave and only has to be told what to expect. So this file configures a
 * receiver, not a clock source - every rate decision lives in i2s.c, and the
 * two must agree or the codec will happily decode nonsense at the wrong speed.
 *
 * WHAT IS VERIFIABLE HERE, and it matters because nothing audible happens
 * until I2S works too: every register written can be read back. es8311_init
 * checks each one, so a codec that is wired up but not listening is
 * distinguishable from one that never got the write - which is the ambiguity
 * that made panel bring-up so slow, and the I2C bus does not have to repeat it.
 *
 * Board wiring (Waveshare pin_config.h, confirmed against the BSP):
 *   MCLK 16   BCLK 9   WS 45   DOUT 8   DIN 10   PA enable 46
 */
#ifndef ES8311_H
#define ES8311_H

#include <stdint.h>

#define ES8311_I2C_ADDR 0x18u        /* 7-bit; vendor headers quote 0x30 as 8-bit */

#define ES8311_OK          0
#define ES8311_E_ABSENT  (-1)        /* no ACK at the address        */
#define ES8311_E_ID      (-2)        /* answered, but not an ES8311  */
#define ES8311_E_IO      (-3)        /* an I2C transfer failed       */
#define ES8311_E_VERIFY  (-4)        /* a register did not read back */

/*
 * Bring the codec up for 16 kHz, 16-bit playback with MCLK at 256*fs.
 *
 * 16 kHz because that is what the sound effects are stored at: 1 second of
 * mono costs 16 KB of SRAM, and SRAM is the whole budget - the mask ROM loads
 * this image into RAM and there is no XIP to stream from.
 *
 * Returns ES8311_OK, or a negative code identifying WHICH stage failed.
 */
int es8311_init(void);

/* 0 = mute, 255 = maximum. The DAC's own scale, not decibels. */
int es8311_volume(uint8_t level);

/* Speaker amplifier enable, GPIO46. Off during init so the codec's power-up
 * transient does not reach the speaker. */
void es8311_amp(int on);

/* Reads the amplifier enable pin BACK. 1 = driven high = amp on. */
uint32_t es8311_amp_level(void);

/* Chip ID read during init: 0x8311 on the real part. */
uint16_t es8311_chip_id(void);

#endif /* ES8311_H */
