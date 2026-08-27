/*
 * I2S0 transmit, master, feeding the ES8311. Pure ISO C99.
 *
 * The ESP32-S3's I2S has NO CPU-accessible data FIFO - samples reach it only
 * through GDMA. So this owns GDMA channel 1, in the same way spi2 owns channel
 * 0, and plays from a ring the CPU refills.
 *
 * THE CLOCK IS CHOSEN TO BE EXACT, not to hit a round sample rate:
 *
 *     XTAL          40.000 MHz
 *     / 10          4.000 MHz  MCLK   (the codec wants 256*fs)
 *     / 8            500  kHz  BCLK
 *     / 32         15.625 kHz  LRCK   = the sample rate
 *
 * Every divider is an integer. Asking for exactly 16 kHz needs 40/4.096 =
 * 9.7656, and the fractional divider that requires is three more registers of
 * encoding to get a rate 2.3% different from this one - inaudible on a laser
 * and an explosion. If a future caller genuinely needs 16 kHz, the fractional
 * path is where to spend that complexity, not here.
 *
 * The codec does not care: in slave mode it follows MCLK and LRCK, and all its
 * coefficient row asserts is that MCLK/LRCK is 256. That still holds.
 *
 * Pins (Waveshare): MCLK 16, BCLK 9, WS 45, DOUT 8.
 */
#ifndef I2S_H
#define I2S_H

#include <stdint.h>

#define I2S_RATE_HZ   15625u
#define I2S_CHANNELS  2u             /* the codec expects stereo frames */

#define I2S_OK          0
#define I2S_E_HANG    (-1)

/*
 * Bring up I2S0 TX and its GDMA channel. Call AFTER es8311_init() - the codec
 * should be configured and listening before clocks start arriving, so it never
 * sees a partially-configured bit clock.
 */
int i2s_init(void);

/*
 * Hand over a buffer to play, looping forever until the next call.
 *
 * `frames` counts STEREO frames: 2 int16 samples each. The buffer must stay
 * valid and unmodified while it plays - GDMA reads it directly - and must live
 * in internal SRAM, which everything in this image does.
 *
 * Looping rather than one-shot because the first thing worth proving is that
 * clocks and data reach the speaker at all, and a continuous tone is audible
 * in a way a 20 ms blip is not.
 */
int i2s_play_loop(const int16_t *frames, uint32_t nframes);

/*
 * Put the loop's descriptor back in the DMA's hands. Returns 1 if it re-armed.
 *
 * MUST be called faster than the buffer drains - hardware clears the owner bit
 * and never restores it, so an unattended loop plays exactly once. At 15625 Hz
 * a 1000-frame buffer is 64 ms.
 */
uint32_t i2s_service(void);
uint32_t i2s_rearms(void);

/*
 * Continuous playback over a two-descriptor ring: `buf` holds 2*frames_half
 * stereo frames, and the hardware alternates halves.
 *
 * i2s_ring_claim() returns the half the DMA has finished with, or NULL if both
 * are still in flight; fill it and call i2s_ring_release(). That pairing is
 * what a mixer needs and a single looping descriptor cannot give.
 */
int      i2s_play_ring(int16_t *buf, uint32_t frames_half);
int16_t *i2s_ring_claim(void);
void     i2s_ring_release(void);

/* Stop the transmitter and let the line idle. */
void i2s_stop(void);

/* Frames the DMA has consumed since i2s_play_loop - for checking that data is
 * actually moving when nothing is audible. */
uint32_t i2s_frames_out(void);

/*
 * DIAGNOSTICS for the case that matters: configured, and silent.
 *
 * pad_edges counts transitions on a pin over `us` microseconds - the clock
 * lines are routed with their input buffers enabled so they can be read back.
 * A BCLK that is configured but not running looks identical to one that is,
 * from the register side; from the pad it does not.
 *
 * desc_dw0's owner bit (31) is cleared by hardware when GDMA finishes with the
 * descriptor. Still set means the DMA never ran at all.
 */
uint32_t i2s_dbg_pad_edges(uint32_t gpio, uint32_t us);
uint32_t i2s_dbg_tx_conf(void);
uint32_t i2s_dbg_desc_dw0(void);
uint32_t i2s_dbg_out_link(void);
uint32_t i2s_dbg_pad_selftest(uint32_t gpio, uint32_t toggles);

#endif /* I2S_H */
