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
uint32_t sfx_fills(void)       { return 0u; }
void sfx_play_pcm(const int16_t *p, uint32_t n) { (void)p; (void)n; }
uint32_t sfx_selftest(void)    { return 0u; }
uint32_t sfx_starved(void)     { return 0u; }
uint32_t sfx_decodes(void)     { return 0u; }
uint32_t sfx_decode_fail(void) { return 0u; }
void sfx_dbg_first_samples(void (*e)(int32_t), uint32_t n) { (void)e; (void)n; }

/*
 * Console stubs. gridvoid reports sfx_init's result over the serial console,
 * which is the right place for it on the device and does not exist here.
 * Printing to stdout instead would bury the harnesses' own output in noise
 * from 6000 frames of gameplay.
 */
#include <stdio.h>
void con_puts(const char *s)  { (void)s; }
void con_dec(int32_t v)       { (void)v; }
void con_hex32(uint32_t v)    { (void)v; }

uint32_t i2s_underruns(void) { return 0u; }
