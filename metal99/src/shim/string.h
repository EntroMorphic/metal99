/* Three functions, because three are what the vendored decoder uses.
 *
 * It was two until the compiler found memmove - which is the shim working as
 * intended: the list grows only when something fails to build, never on
 * speculation about what might be wanted later. */
#ifndef SHIM_STRING_H
#define SHIM_STRING_H
#include <stddef.h>
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
void *memmove(void *dst, const void *src, size_t n);
#endif
