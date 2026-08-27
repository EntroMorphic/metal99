# minimp3 (vendored)

Upstream: https://github.com/lieff/minimp3
Revision: ea99364f61c14656440e8d77e9c233ccf3124633
Licence: **CC0 1.0 Universal** (public domain) — see `LICENSE`.

Vendored rather than fetched, because this project builds with gcc, ld and
esptool and nothing else. There is no package manager to pin a version with.

## Why this decoder

The sound effects are mixed for full-range playback and lose their character
when resampled and band-limited. Decoding the original MP3 keeps the source
exactly as authored — and costs *less* memory than the PCM we were baking
from it:

| | |
|---|---|
| decoded PCM at 31250 Hz, both effects | 92.0 KB |
| the original MP3s, both files | 68.3 KB |
| minimp3 code + tables | 21.4 KB |
| decoder state, per voice | 6.5 KB |

It also compiles clean under this project's flags —
`-std=c99 -pedantic-errors -Wall -Wextra -Wshadow -Werror` — with **zero
diagnostics**, which is not true of most decade-old C. That is why it was
chosen over libhelix, which additionally carries RealNetworks RPSL terms this
MIT repo would inherit.

## What it needs from us

`memcpy` and `memset`. No `malloc`, no libm, no stdio (`MINIMP3_NO_STDIO`).
It uses single-precision float, which the ESP32-S3 has an FPU for.

## Local changes

None. If that ever stops being true, list them here — a vendored file with
undocumented edits is worse than a fork.
