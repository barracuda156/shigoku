// result.hpp — Result<T,E> = std::expected alias, the Rust Result port.
//
// PORT_CPP.md A2: fallible functions return [[nodiscard]] std::expected<T, E>.
// No exceptions cross module boundaries (nlohmann throws internally, our APIs
// don't). §3: std::expected needs GCC 12+/C++23; keep tl::expected as the
// documented fallback for a lagging target.
//
// Idiom map (§7): Rust `Result<T,E>` -> Result<T,E>; `Ok(x)` -> just `x` (an
// implicit expected); `Err(e)` -> `err(e)` / `Unexpected{e}`. Every function
// returning Result is [[nodiscard]] via the RESULT macro or manual attribute.

#pragma once

// Prefer the standard library; fall back to the vendored tl::expected only if
// <expected> is genuinely absent (a pre-C++23 / pre-GCC-12 target, §3).
//
// NB: __cpp_lib_expected is only defined AFTER a library header exposes it, so
// we must include <version> (the feature-test-macro carrier, cheap — no real
// declarations) BEFORE testing it. Testing it cold, as an earlier draft did,
// always read it as 0 and silently fell back to tl::expected even on GCC 13.
#include <version>

#if defined(__has_include)
#  if __has_include(<expected>) && defined(__cpp_lib_expected) && \
      __cpp_lib_expected >= 202202L
#    define SHIGOKU_USE_STD_EXPECTED 1
#  else
#    define SHIGOKU_USE_STD_EXPECTED 0
#  endif
#else
#  define SHIGOKU_USE_STD_EXPECTED 0
#endif

#if SHIGOKU_USE_STD_EXPECTED
#  include <expected>
#else
#  include <tl/expected.hpp>
#endif

#include <utility>

namespace shigoku {

#if SHIGOKU_USE_STD_EXPECTED
template <class T, class E>
using Result = std::expected<T, E>;

template <class E>
using Unexpected = std::unexpected<E>;
#else
template <class T, class E>
using Result = tl::expected<T, E>;

template <class E>
using Unexpected = tl::unexpected<E>;
#endif

// err(e) — construct the error side of a Result without spelling out the
// unexpected wrapper. `return err(ProviderError::network());`
template <class E>
[[nodiscard]] constexpr auto err(E&& e) {
  return Unexpected<std::decay_t<E>>(std::forward<E>(e));
}

// ok() — the unit success value, so a `Result<void-like, E>` reads cleanly.
// We use Result<std::monostate, E> for fallible-but-valueless functions.
struct Unit {
  friend constexpr bool operator==(Unit, Unit) noexcept { return true; }
};

}  // namespace shigoku
