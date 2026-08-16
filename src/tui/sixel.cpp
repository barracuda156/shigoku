// sixel.cpp — SIXEL emitter (see sixel.hpp for the wire contract). The ONE TU
// that touches the vendored sayaka encoder; everything above it sees a
// std::string, the same leaf-module discipline that keeps libcurl-impersonate
// inside anidbapp.cpp.

#include "sixel.hpp"

#include <cstddef>
#include <cstring>
#include <memory>

#include "memstream_compat.hpp"

// The vendored reductor + writer. Its headers are C and its shim defines a
// pile of short lower-case macros (MIN/MAX/ESC/…) that have no business
// leaking into a C++ TU, so the include is fenced and the macros dropped
// immediately after.
extern "C" {
#include <sixel_sayaka/image.h>
}
#undef MIN
#undef MAX
#undef CAN
#undef ESC
#undef CSI
#undef ESCchar
#undef PUTD
#undef Debug
#undef Trace
#undef Verbose
#undef countof
#undef howmany
#undef roundup
#undef rounddown
#undef UNCONST

namespace shigoku::tui::sixel {

namespace {

// Destination pixel rails. Quantization runs on the UI thread inside the
// flush, so an absurd box — a HiDPI cell under a full-screen zoom — must not
// turn one frame into a multi-second stall on the slow target. Both rails
// scale width and height by the same factor, preserving the aspect the cover
// was already cropped to. Real cover boxes sit an order of magnitude below
// them; these are rails, not policy.
constexpr std::uint32_t kMaxSide = 2560;      // mirrors the decode-side rail.
constexpr std::uint64_t kMaxPixels = 4u * 1024u * 1024u;

// Shrink (w, h) proportionally until both rails hold. False if the result
// degenerates to zero — nothing sane to draw.
bool clamp_box(std::uint32_t& w, std::uint32_t& h) {
  while (w > kMaxSide || h > kMaxSide ||
         static_cast<std::uint64_t>(w) * h > kMaxPixels) {
    w /= 2;
    h /= 2;
    if (w == 0 || h == 0) return false;
  }
  return true;
}

struct ImageDeleter {
  void operator()(struct image* p) const { ::image_free(p); }
};
using ImagePtr = std::unique_ptr<struct image, ImageDeleter>;

}  // namespace

std::string transmit(const std::vector<unsigned char>& rgba, std::uint32_t w,
                     std::uint32_t h, int cols, int rows, std::uint16_t cell_w,
                     std::uint16_t cell_h) {
  if (w == 0 || h == 0 || cols <= 0 || rows <= 0 || cell_w == 0 || cell_h == 0)
    return {};
  const std::size_t need = static_cast<std::size_t>(w) * h * 4u;
  if (rgba.size() != need) return {};

  std::uint32_t dst_w = static_cast<std::uint32_t>(cols) * cell_w;
  std::uint32_t dst_h = static_cast<std::uint32_t>(rows) * cell_h;
  if (!clamp_box(dst_w, dst_h)) return {};

  // IMAGE_FMT_ARGB32 is the encoder's name for R,G,B,A in memory order — our
  // exact CoverPixels layout, so the source is a straight byte copy.
  ImagePtr src(::image_create(w, h, IMAGE_FMT_ARGB32));
  if (!src) return {};
  std::memcpy(src->buf, rgba.data(), need);
  // Covers are opaque posters. A cover that does carry transparency still
  // works: convert_to16 flags pixels under 50% alpha and the writer leaves
  // them unpainted, showing the terminal background through.
  src->has_alpha = false;
  ::image_convert_to16(src.get());

  // The encoder's defaults are already what we want — an adaptive 256-colour
  // palette, Sierra Filter Lite diffusion, background-opaque (not OR-mode)
  // output — so v1 exposes no knobs.
  struct image_opt opt;
  ::image_opt_init(&opt);

  // One call resamples to the destination box AND quantizes to the palette.
  ImagePtr dst(::image_reduct(src.get(), dst_w, dst_h, &opt, nullptr));
  src.reset();
  if (!dst) return {};

  StringFile sink;
  if (!sink.ok()) return {};
  if (!::image_sixel_write(sink.file(), dst.get(), &opt, nullptr)) return {};
  dst.reset();
  return sink.take();
}

namespace detail {

bool da1_has_attribute(std::string_view reply, int attr) {
  // A DA1 answer is CSI ? <ps> ; <ps> … c — private-parameter marker, a
  // semicolon-separated decimal list, terminated by lowercase 'c'. Scan every
  // such answer in the window (the probe collects several replies at once)
  // and compare tokens whole.
  constexpr std::string_view kPrefix = "\x1b[?";
  std::size_t at = 0;
  while ((at = reply.find(kPrefix, at)) != std::string_view::npos) {
    std::size_t i = at + kPrefix.size();
    int cur = 0;
    bool have_digits = false;
    for (; i < reply.size(); ++i) {
      const char c = reply[i];
      if (c >= '0' && c <= '9') {
        cur = cur * 10 + (c - '0');
        if (cur > 100000) cur = 100000;  // clamp; no real attribute is huge.
        have_digits = true;
        continue;
      }
      if (c == ';' || c == 'c') {
        if (have_digits && cur == attr) return true;
        cur = 0;
        have_digits = false;
        if (c == 'c') break;  // end of this DA1 answer.
        continue;
      }
      break;  // not a DA1 body after all; resume scanning past this prefix.
    }
    at += kPrefix.size();
  }
  return false;
}

}  // namespace detail

bool probe_reply_is_sixel(std::string_view reply) {
  return detail::da1_has_attribute(reply, 4);
}

}  // namespace shigoku::tui::sixel
