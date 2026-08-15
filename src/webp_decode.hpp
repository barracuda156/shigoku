// webp_decode.hpp — the RIFF….WEBP magic sniff + a thin libwebp wrapper,
// shared by shigoku-view's decode_page (src/view/main.cpp) and the covers
// pipeline's decode_cover (src/tui/covers.cpp). Both otherwise decode via
// stb_image, which cannot read WebP.
//
// HAVE_WEBP is a PUBLIC compile definition on the shigoku_webp_decode CMake
// target (auto-off, like WITH_VIEWER, when libwebp isn't found) — every TU
// that links this target sees the same value, so the two call sites never
// disagree about whether decode_webp() exists. When it's off, looks_like_webp
// still works (so a WebP body is recognized and rejected cleanly instead of
// being handed to stb_image, which would just fail anyway).

#pragma once

#include <cstddef>
#include <cstdint>

namespace shigoku {

// True when `data` starts with the RIFF….WEBP container magic (bytes 0-3
// "RIFF", bytes 8-11 "WEBP"). Cheap enough to call on every decode.
[[nodiscard]] bool looks_like_webp(const std::uint8_t* data, std::size_t len);

#ifdef HAVE_WEBP

// Header-only probe (dimensions, no pixel decode) — mirrors stbi_info's
// decode-bomb guard usage at both call sites: check dimensions before
// spending a full decode on an oversized image.
[[nodiscard]] bool webp_info(const std::uint8_t* data, std::size_t len, int* w,
                              int* h);

// Full RGBA8888 decode, row-major top-down — the same layout stb_image
// produces, so both call sites treat the result identically. Returns nullptr
// on failure. Caller frees a non-null result with free_webp_pixels().
[[nodiscard]] std::uint8_t* decode_webp(const std::uint8_t* data,
                                        std::size_t len, int* out_w,
                                        int* out_h);

void free_webp_pixels(std::uint8_t* pixels);

#endif  // HAVE_WEBP

}  // namespace shigoku
