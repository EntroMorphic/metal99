/* Minimal <arm_neon.h> stand-in so NeoGPU's headers can be PARSED on x86 for
 * struct-layout measurement. Defines only the vector typedefs needed for
 * sizeof(); it implements no operations and is not for building anything.
 * Needed because hs_core.h includes <arm_neon.h> directly rather than going
 * through the project's own hs_neonCompat.h. */
#ifndef METAL99_COMPAT_ARM_NEON_H
#define METAL99_COMPAT_ARM_NEON_H
#include <stdint.h>
typedef float    float32x4_t __attribute__((vector_size(16)));
typedef int32_t  int32x4_t   __attribute__((vector_size(16)));
typedef uint32_t uint32x4_t  __attribute__((vector_size(16)));
typedef int16_t  int16x8_t   __attribute__((vector_size(16)));
typedef uint8_t  uint8x16_t  __attribute__((vector_size(16)));
typedef int8_t   int8x16_t   __attribute__((vector_size(16)));
typedef float    float32x2_t __attribute__((vector_size(8)));
#endif
