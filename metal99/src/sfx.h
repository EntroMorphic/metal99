/*
 * Sound effects: a tiny mixer over the I2S loop.
 *
 * Clips are 8-bit mono at I2S_RATE_HZ, baked into the image by tools/mksfx.py.
 * They are NOT streamed - the mask ROM loads this whole image into SRAM and
 * there is no XIP, so audio competes directly with the tile hashes and the
 * segment pool. Two effects cost about 34 KB. Music, at 131 seconds, would
 * cost 2 MB and needs a flash reader; that is a separate project.
 *
 * MIXING, because a shot and a kill overlap constantly in this game. Voices
 * are summed into the same buffer the DMA is looping over, so playback costs
 * one pass per active voice per refill and nothing at all when silent.
 */
#ifndef SFX_H
#define SFX_H

#include <stdint.h>

typedef struct { const int16_t *pcm; uint32_t len; } sfx_clip;

extern const sfx_clip SFX[];
extern const uint32_t SFX_COUNT;

/* Indices into SFX[], in the order tools/mksfx.py emits them. */
#define SFX_FIRE 0u
#define SFX_KILL 1u

#define SFX_VOICES 4u        /* simultaneous effects before the oldest is stolen */

/*
 * Bring up the codec and I2S, then start the silent mixing loop.
 * Returns 0 on success; a negative value means audio is unavailable and every
 * other call here becomes a no-op, so an app never has to check again.
 */
int  sfx_init(void);

/* Start `clip` on a free voice. Silently ignored if audio failed to init. */
void sfx_play(uint32_t clip);

/*
 * Refill the playing buffer. MUST be called every frame - the DMA loops over
 * a fixed buffer and this is what writes the next stretch of audio into it
 * before the hardware reads it again.
 */
void sfx_service(void);

/* 0..255. Applied to the mix, not to the codec - so it can change per frame
 * without touching I2C. */
void sfx_volume(uint8_t v);

/*
 * Halves the mixer has actually filled. Zero while the game runs means the ring
 * never hands one over - the failure that silence looks like from outside.
 */
uint32_t sfx_fills(void);

#endif /* SFX_H */
