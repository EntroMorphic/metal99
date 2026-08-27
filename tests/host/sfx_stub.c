/*
 * Audio stubs for the host harnesses.
 *
 * sfx.c reaches straight into the ES8311 over I2C and I2S0 over GDMA - there
 * is nothing to simulate on a desktop and nothing worth simulating: what the
 * host tests exercise is the GAME, and whether a sound was requested is not a
 * property any of them assert.
 *
 * Play calls are COUNTED rather than discarded, so a test can check that
 * firing and killing actually request audio if it ever wants to. Nothing does
 * yet, and the counter costs nothing.
 */
#include <stdint.h>
#include "sfx.h"

const sfx_clip SFX[] = { { 0, 0u }, { 0, 0u } };
const uint32_t SFX_COUNT = 2u;

uint32_t sfx_stub_plays[2];

int  sfx_init(void)            { return 0; }
void sfx_volume(uint8_t v)     { (void)v; }
void sfx_service(void)         { }
void sfx_play(uint32_t clip)   { if (clip < 2u) sfx_stub_plays[clip]++; }
