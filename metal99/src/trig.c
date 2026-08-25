/*
 * Fixed-point trigonometry. Pure ISO C99, no FPU, no libm.
 *
 * BINARY ANGLES: 256 units to a full turn, so wrapping is a mask rather than a
 * modulo and there is no accumulating error when something spins for minutes.
 * A game rotates things constantly; degrees would mean a division per call and
 * a negative-safe remainder that is easy to get wrong.
 *
 * Q16 results: 65536 is 1.0. Multiply a length by the result and shift right
 * 16 to project it.
 *
 * The table is one quadrant, 64 steps, mirrored for the other three.
 * Regenerate with tools/mktrig.py - checked rather than trusted, the same rule
 * that put mkfont.py in tools/.
 */
#include "trig.h"

static const int32_t SINQ[65] = {
         0,   1608,   3216,   4821,   6424,   8022,   9616,  11204,
     12785,  14359,  15924,  17479,  19024,  20557,  22078,  23586,
     25080,  26558,  28020,  29466,  30893,  32303,  33692,  35062,
     36410,  37736,  39040,  40320,  41576,  42806,  44011,  45190,
     46341,  47464,  48559,  49624,  50660,  51665,  52639,  53581,
     54491,  55368,  56212,  57022,  57798,  58538,  59244,  59914,
     60547,  61145,  61705,  62228,  62714,  63162,  63572,  63944,
     64277,  64571,  64827,  65043,  65220,  65358,  65457,  65516,
     65536,
};

int32_t isin(int32_t a)
{
    uint32_t u = (uint32_t)a & 255u;
    switch (u >> 6) {
    case 0:  return  SINQ[u];
    case 1:  return  SINQ[128u - u];       /* mirror about the quarter turn */
    case 2:  return -SINQ[u - 128u];
    default: return -SINQ[256u - u];
    }
}

int32_t icos(int32_t a) { return isin(a + 64); }
