/* Sound effects: MP3 decoded on the fly, mixed into the I2S ring. See sfx.h. */
#include <stddef.h>
#include "sfx.h"
#include "i2s.h"
#include "es8311.h"
#include "vec.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO
#include "minimp3/minimp3.h"

/*
 * A buffer must outlast the gap between sfx_service() calls, which is one
 * video frame - 25 ms at 40 Hz. At 48 kHz, 1000 stereo frames is 4000 bytes
 * (just under the 4095 a single GDMA descriptor carries) and 20.8 ms. Four of
 * them give 83 ms, which is three video frames of slack.
 *
 * Sized in MILLISECONDS on purpose. This was once expressed in samples, and a
 * change of sample rate silently halved it in time and underran every frame.
 */
#define HALF_FRAMES 1000u

static int16_t VEC_ALIGN g_buf[HALF_FRAMES * I2S_RING_BUFFERS * I2S_CHANNELS];

/*
 * TWO VOICES, not four.
 *
 * Each carries a decoder (6.5 KB) and a decoded-frame buffer (4.6 KB), so a
 * voice costs 11 KB against PCM playback's zero. Two covers what this game
 * actually overlaps - a shot and a kill - and costs 22 KB. Four would cost 44
 * and buy simultaneity nothing asks for.
 */
#define VOICES 2u

typedef struct {
    mp3dec_t        dec;
    const uint8_t  *src;        /* remaining MP3 bytes            */
    uint32_t        left;
    int16_t         pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    uint32_t        have;       /* decoded samples per channel    */
    uint32_t        pos;        /* consumed so far                */
    uint32_t        chans;
    int             active;
} voice;

static voice g_voice[VOICES];
static uint8_t g_vol = 50u;   /* TEST: quarter level - see if distortion follows loudness */
static int g_up;
static uint32_t g_fills, g_starved, g_decodes, g_decode_fail;

int sfx_init(void)
{
    uint32_t i;
    int rc;

    for (i = 0u; i < VOICES; i++) g_voice[i].active = 0;
    for (i = 0u; i < (uint32_t)(sizeof g_buf / sizeof g_buf[0]); i++) g_buf[i] = 0;

    rc = es8311_init();
    if (rc != ES8311_OK) return rc;
    rc = i2s_init();
    if (rc != I2S_OK)    return rc;

    /*
     * 0x94, six dB below 0xA8 - half the amplitude.
     *
     * The register is logarithmic (value/2 - 95.5 dB), so halving loudness is
     * subtracting 12 from the value, not dividing it by two. Treating it as a
     * linear 0..255 fraction once put it at -55 dB and produced silence.
     */
    (void)es8311_volume(0x9Fu);          /* about -16 dB */
    es8311_amp(1);

    rc = i2s_play_ring(g_buf, HALF_FRAMES);
    if (rc != I2S_OK) return rc;

    g_up = 1;
    return 0;
}

void sfx_volume(uint8_t v)  { g_vol = v; }
uint32_t sfx_fills(void)    { return g_fills; }
uint32_t sfx_starved(void)  { return g_starved; }
uint32_t sfx_decodes(void)  { return g_decodes; }
uint32_t sfx_decode_fail(void) { return g_decode_fail; }

void sfx_play(uint32_t clip)
{
    uint32_t i, pick = 0u, best = 0u;

    if (!g_up || clip >= SFX_COUNT) return;

    /* Free voice if there is one, else the one furthest through its clip. */
    for (i = 0u; i < VOICES; i++) {
        if (!g_voice[i].active) { pick = i; break; }
        if (g_voice[i].pos > best) { best = g_voice[i].pos; pick = i; }
    }
    mp3dec_init(&g_voice[pick].dec);
    g_voice[pick].src    = SFX[clip].mp3;
    g_voice[pick].left   = SFX[clip].len;
    g_voice[pick].have   = 0u;
    g_voice[pick].pos    = 0u;
    g_voice[pick].chans  = 2u;
    g_voice[pick].active = 1;
}

/*
 * Pull the next MP3 frame for a voice. Returns 0 when the clip is finished.
 *
 * mp3dec_decode_frame reports frame_bytes even when it produces no samples -
 * that is how it skips ID3 tags and resyncs after damage - so consuming
 * frame_bytes unconditionally is what keeps the stream advancing. Treating
 * "no samples" as end-of-clip would truncate every file with a tag on the
 * front, which is most of them.
 */
static int decode_next(voice *v)
{
    mp3dec_frame_info_t info;
    int n;

    for (;;) {
        if (v->left == 0u) return 0;
        n = mp3dec_decode_frame(&v->dec, v->src, (int)v->left, v->pcm, &info);
        if (info.frame_bytes <= 0) { g_decode_fail++; return 0; }
        v->src  += info.frame_bytes;
        v->left -= (uint32_t)info.frame_bytes;
        if (n > 0) {
            v->have  = (uint32_t)n;
            v->pos   = 0u;
            v->chans = (uint32_t)info.channels;
            g_decodes++;
            return 1;
        }
        /* no samples this frame - a tag or a resync; keep going */
    }
}

void sfx_service(void)
{
    int16_t *half;
    uint32_t f, v;
    int idx;

    if (!g_up) return;
    if (i2s_underruns()) g_starved++;

    while ((idx = i2s_ring_claim(&half)) >= 0) {
        for (f = 0u; f < HALF_FRAMES; f++) {
            int32_t l = 0, r = 0;

            for (v = 0u; v < VOICES; v++) {
                voice *vo = &g_voice[v];
                if (!vo->active) continue;
                if (vo->pos >= vo->have && !decode_next(vo)) { vo->active = 0; continue; }
                if (vo->chans == 2u) {
                    l += vo->pcm[vo->pos * 2u];
                    r += vo->pcm[vo->pos * 2u + 1u];
                } else {
                    int32_t m = vo->pcm[vo->pos];
                    l += m; r += m;
                }
                vo->pos++;
            }

            /* Volume as a 0..255 fraction of unity. Clipped, not wrapped:
             * wrapping turns a loud moment into what sounds like a fault. */
            l = (l * (int32_t)g_vol) >> 8;
            r = (r * (int32_t)g_vol) >> 8;
            if (l >  32767) l =  32767;
            if (l < -32768) l = -32768;
            if (r >  32767) r =  32767;
            if (r < -32768) r = -32768;
            half[f * 2u]      = (int16_t)l;
            half[f * 2u + 1u] = (int16_t)r;
        }
        g_fills++;
        i2s_ring_release(idx);
    }
}

uint32_t sfx_selftest(void)
{
    /* Decode the first frame of clip 0 and check it produces samples at the
     * rate the I2S is clocked for. A decoder that silently yields 44100 would
     * play everything 9% slow, which is audible and easy to misattribute. */
    mp3dec_t d;
    mp3dec_frame_info_t info;
    static int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    const uint8_t *p;
    uint32_t left;
    int n;

    if (SFX_COUNT == 0u) return 1u;
    mp3dec_init(&d);
    p = SFX[0].mp3; left = SFX[0].len;
    for (;;) {
        if (left == 0u) return 2u;
        n = mp3dec_decode_frame(&d, p, (int)left, pcm, &info);
        if (info.frame_bytes <= 0) return 3u;
        p += info.frame_bytes; left -= (uint32_t)info.frame_bytes;
        if (n > 0) break;
    }
    if ((uint32_t)info.hz != I2S_RATE_HZ) return 4u;
    return 0u;
}

/*
 * The first decoded samples, verbatim.
 *
 * Everything else about this decoder has been checked EXCEPT what it actually
 * produces: rc=0 says it ran, info.hz says the file is 48 kHz, fail=0 says no
 * frame was rejected. None of that says the audio is right. Printing raw
 * samples lets the host compare them against ffmpeg's decode of the same file
 * and settle whether the fault is upstream or downstream of this point.
 */
void sfx_dbg_first_samples(void (*emit)(int32_t v), uint32_t n)
{
    /* Skip past the leading silence - blaster.mp3 opens with 0.42 s of it, so
     * the first frames are all zeros and prove nothing. 20 frames of 1152
     * samples at 48 kHz is 0.48 s, comfortably into the signal. */
    uint32_t skip = 20u;
    mp3dec_t d;
    mp3dec_frame_info_t info;
    static int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    const uint8_t *p;
    uint32_t left, i;
    int got;

    if (SFX_COUNT == 0u) return;
    mp3dec_init(&d);
    p = SFX[0].mp3; left = SFX[0].len;
    for (;;) {
        if (left == 0u) return;
        got = mp3dec_decode_frame(&d, p, (int)left, pcm, &info);
        if (info.frame_bytes <= 0) return;
        p += info.frame_bytes; left -= (uint32_t)info.frame_bytes;
        if (got > 0) { if (skip == 0u) break; skip--; }
    }
    for (i = 0u; i < n && i < (uint32_t)(got * info.channels); i++) emit(pcm[i]);
}
