// sixel_shim.h — the ONLY shigoku-authored file in this vendored unit. It
// stands in for sayaka's common.h/header.h/config.h, which the two vendored
// TUs include for a handful of typedefs, compiler-attribute macros and a
// logging facade — and which otherwise drag in that project's networking,
// JSON and diagnostics layers. Nothing here is sayaka code; it is the minimal
// surface image.c and image_sixel.c actually reference.
//
// Three jobs:
//   1. the integer typedefs and small utility macros (MIN/MAX/countof/...);
//   2. the config.h HAVE_* feature flags missing_endian.h keys off, supplied
//      statically per platform instead of by a configure run;
//   3. a no-op diagnostics facade — the encoder never logs in our build.
//
// PORTABILITY: the HAVE_* block below is the whole endianness story. sayaka's
// missing_endian.h already normalizes BYTE_ORDER/BIG_ENDIAN/LITTLE_ENDIAN and
// the htobeNN() family across Linux/*BSD/macOS; it just needs to be told which
// header this platform keeps them in. The macOS arm covers the legacy PowerPC
// target (machine/endian.h + libkern/OSByteOrder.h exist on 10.4+).

#ifndef shigoku_sixel_shim_h
#define shigoku_sixel_shim_h

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

// --- (2) config.h substitutes for missing_endian.h -------------------------
#if defined(__APPLE__)
#define HAVE_MACHINE_ENDIAN_H
#define HAVE_LIBKERN_OSBYTEORDER_H
#elif defined(__NetBSD__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__DragonFly__)
#define HAVE_SYS_ENDIAN_H
#define HAVE_HTOBE16
#else
#define HAVE_ENDIAN_H
#define HAVE_HTOBE16
#endif

#if defined(__GNUC__)
#define HAVE___BUILTIN_CLZ
#endif

// --- (1) typedefs and utility macros (sayaka header.h) ---------------------
typedef unsigned int uint;
typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;
typedef int8_t int8;
typedef int16_t int16;
typedef int32_t int32;
typedef int64_t int64;

// Some BSD/Darwin <sys/cdefs.h> define these with semantics we do not want;
// take the definitions over, as upstream does.
#undef __predict_true
#undef __predict_false
#if defined(__GNUC__)
#define __predict_true(exp) __builtin_expect((exp) != 0, 1)
#define __predict_false(exp) __builtin_expect((exp) != 0, 0)
#else
#define __predict_true(exp) (exp)
#define __predict_false(exp) (exp)
#endif

#if !defined(__always_inline)
#if defined(__GNUC__)
#define __always_inline __attribute__((__always_inline__))
#else
#define __always_inline
#endif
#endif

#if !defined(__unreachable)
#if defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 5))
#define __unreachable() __builtin_unreachable()
#elif defined(__clang__)
#define __unreachable() __builtin_unreachable()
#else
#define __unreachable() abort()
#endif
#endif

#if !defined(__unused)
#if defined(__GNUC__)
#define __unused __attribute__((__unused__))
#else
#define __unused
#endif
#endif

// Statement-expression MIN/MAX, upstream's definitions verbatim — they
// evaluate each argument exactly once, which the reductor's MAX(cdm, abs(...))
// call sites rely on. This unit is therefore compiled as gnu11, not c11.
#define MAX(a, b)             \
  ({                          \
    __typeof__(a) a_ = (a);   \
    __typeof__(b) b_ = (b);   \
    a_ > b_ ? a_ : b_;        \
  })

#define MIN(a, b)             \
  ({                          \
    __typeof__(a) a_ = (a);   \
    __typeof__(b) b_ = (b);   \
    a_ < b_ ? a_ : b_;        \
  })

#ifndef UNCONST
#define UNCONST(p) ((void *)(uintptr_t)(const void *)(p))
#endif

#ifndef countof
#define countof(x) (sizeof(x) / sizeof(x[0]))
#endif

#ifndef howmany
#define howmany(x, y) (((x) + (y) - 1) / (y))
#endif
#ifndef roundup
#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))
#endif
#ifndef rounddown
#define rounddown(x, y) (((x) / (y)) * (y))
#endif

#define CAN "\x18"
#define ESC "\x1b"
#define CSI ESC "["
#define ESCchar '\x1b'

// Decimal itoa, replacing sayaka's util.c putd_fast/putd_inline pair. Writes
// the digits of `n` at `dst` (no terminator) and returns how many it wrote —
// the contract the SIXEL writer's `p += PUTD(p, x)` idiom depends on.
static inline uint sixel_shim_putd(char *dst, uint n) {
  char tmp[12];
  uint i = 0;
  do {
    tmp[i++] = (char)('0' + (n % 10u));
    n /= 10u;
  } while (n != 0);
  for (uint j = 0; j < i; j++) {
    dst[j] = tmp[i - 1 - j];
  }
  return i;
}
#define PUTD(buf, n) sixel_shim_putd(buf, n)

// --- (3) the no-op diagnostics facade --------------------------------------
// Every call site in the two vendored TUs passes NULL for the diag handle, so
// the struct stays incomplete and the level is a compile-time zero: the two
// surviving `diag_get_level(diag) >= 2` branches fold away entirely.
struct diag;

static inline int diag_get_level(const struct diag *diag) {
  (void)diag;
  return 0;
}

static inline void diag_print(const struct diag *diag, const char *fmt, ...) {
  (void)diag;
  (void)fmt;
}

#define Debug(...) ((void)0)
#define Trace(...) ((void)0)
#define Verbose(...) ((void)0)

#endif  // !shigoku_sixel_shim_h
