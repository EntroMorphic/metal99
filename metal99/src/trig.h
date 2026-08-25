/*
 * Fixed-point trig for the vector renderer and the game. See trig.c.
 *
 * Angles are BINARY: 256 units to a full turn. Results are Q16, where 65536 is
 * 1.0 - so a point at radius r and angle a is
 *
 *     x = cx + ((icos(a) * r) >> 16)
 *     y = cy + ((isin(a) * r) >> 16)
 */
#ifndef TRIG_H
#define TRIG_H

#include <stdint.h>

#define TRIG_FULL  256        /* units in a full turn */
#define TRIG_ONE   65536      /* Q16 1.0              */

int32_t isin(int32_t a);
int32_t icos(int32_t a);

#endif /* TRIG_H */
