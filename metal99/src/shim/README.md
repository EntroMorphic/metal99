# shim/

The smallest possible `stdlib.h` and `string.h`, so vendored third-party code
compiles in a freestanding build.

This project has no libc. `build.sh` passes `-nostdinc` and points only at
GCC's own headers, which is deliberate: it is what makes "no libc" a fact the
compiler enforces rather than a claim in a README.

minimp3 includes `<stdlib.h>` and `<string.h>`, and uses exactly two things
from either: `memcpy` and `memset`. So these headers declare two functions and
nothing else. Anything that needs a third will fail to compile, which is the
point — a shim that grows quietly becomes a libc nobody decided to adopt.

The implementations live in `shim.c`.
