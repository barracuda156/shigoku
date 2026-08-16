# sixel_sayaka — vendored SIXEL encoder

The colour reductor and SIXEL writer from **sayaka** by Tetsuya Isaki
(<https://github.com/isaki68k/sayaka>), upstream `e19f9b8` ("ver 3.8.7").

**License: BSD-2-Clause**, GPL-compatible. sayaka ships no separate LICENSE
file; the per-file copyright headers *are* the licence, and every vendored
file retains its header verbatim. `missing_endian.h` additionally carries
Y.Sugahara's copyright.

`src/tui/sixel.cpp` is the only thing in the tree that links this unit; see
`src/tui/sixel.hpp` for the wrapper's contract.

## What is here

| file | upstream | notes |
| --- | --- | --- |
| `image.c` | `src/image.c` | reductor: adaptive palette + error diffusion; **loaders stripped** |
| `image_sixel.c` | `src/image_sixel.c` | the SIXEL writer |
| `image.h` | `src/image.h` | public types + entry points |
| `image_priv.h` | `src/image_priv.h` | verbatim |
| `missing_endian.h` | `src/missing_endian.h` | verbatim — byte-order normalisation, big-endian and OS X already handled |
| `sixel_shim.h` | — | **ours**, not upstream (see below) |

Not vendored: every image loader (blurhash/bmp/gif/ico/jpeg/jxl/mag/png/pnm/
tiff/webp/ypic/stb), the ASCII renderer, and sayaka's `common.h`/`header.h`/
`config.h` — those drag in that project's networking, JSON and diagnostics
layers. We hand the reductor pixels we decoded ourselves, so no loader path is
reachable. Nothing here depends on stb, and the unit needs no configure step.

## Deviations from upstream

Each is marked with a `shigoku:` comment at the site.

1. `#include "common.h"` → `#include "sixel_shim.h"` in `image.c`,
   `image_sixel.c` and `image.h` — one line each. `sixel_shim.h` supplies only
   what those TUs reference: the integer typedefs, the compiler-attribute and
   utility macros, a `PUTD` replacing sayaka's `util.c`, the `HAVE_*` flags
   `missing_endian.h` expects from `config.h`, and a no-op diag facade.
2. `image.c`: `image_parse_color()`, the `loader[]` table,
   `image_get_loaderinfo()`, `image_match()` and `image_read()` deleted.
3. `image.h`: declarations of the deleted functions removed;
   `image_create()` promoted here from `image_priv.h`.
4. `image.c`: `free(ir->colorhash)` in `image_reduct()` is no longer gated on
   `SIXELV`. `image_calc_adaptive_palette()` allocates it unconditionally and
   the default adaptive colour mode always reaches that path, so upstream's
   gate leaks 64KB per call in a build without `-DSIXELV` (which is how
   upstream builds it for the `sayaka` binary, `Makefile:17`).

`SIXELV` is never defined for this build, so the fixed-256/xterm-256 palettes,
the simple reductor, the non-SFL diffusion kernels and the enum-to-string
helpers all compile out — as they do in upstream's own `sayaka` binary.

## Re-vendoring

Copy the six upstream files, re-apply the four deviations above, then rebuild
and run `ctest -R sixel_tests`. The encoder is deterministic: identical input
bytes give an identical stream, which those tests assert.
