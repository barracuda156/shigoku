// docsrc.hpp — the document seam: what shigoku-view needs from a paginated
// document, stated without naming a library. MupdfSource implements it over
// libmupdf; the tests implement it over a script. NOTHING here includes
// mupdf, so the pure core (and viewer_tests) sees only this interface.
//
// The interesting part is not the page accessors but the RESIZE contract.
// A reflowable book (EPUB, FB2) has no fixed pages: the window size decides
// how many there are, so every resize renumbers them under the reader. The
// fix is to carry the reading POSITION across the relayout instead of the
// page index, which is exactly what mark()/lookup() are for, and to do it in
// one order — mark, relayout, lookup, remap. relayout_and_remap() is that
// order written once, so no caller can get it wrong.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../result.hpp"
#include "pager.hpp"

namespace shigoku::view {

// A rendered page: RGBA32 row-major, w*h*4 bytes — the same layout the SDL
// upload (sdl_compat) and the image path already speak, byte per channel, so
// nothing here depends on the host's endianness.
struct RgbaImage {
  std::vector<std::uint8_t> rgba;
  int w = 0;
  int h = 0;
};

// A page's natural size in PIXELS at 96 dpi (a document's own units are
// points; the seam converts once so fit_rect/apply_zoom keep working in the
// pixel arithmetic they already use for images).
struct PageSize {
  int w = 0;
  int h = 0;
};

// A reading position that survives a relayout. `mark` is the backend's own
// opaque handle (0 = none). `chapter` + `frac` (0..1 through that chapter)
// are the coarse fallback for the pages a backend cannot mark — position is
// approximate by nature, and an approximate answer beats losing the place.
struct Bookmark {
  std::intptr_t mark = 0;
  int chapter = 0;
  double frac = 0.0;
};

// One open document. Single-threaded by contract: the viewer renders
// documents on the main thread and never hands one to a worker (the image
// path's prefetch cache is images-only).
class DocSource {
 public:
  virtual ~DocSource() = default;

  virtual int page_count() const = 0;
  virtual PageSize page_size(int page) const = 0;

  // Render `page` at exactly w_px x h_px (documents are vector art: a zoom
  // is a bigger render, never a magnified bitmap). The returned image's own
  // w/h are authoritative — they may differ from the request by a pixel.
  virtual Result<RgbaImage, std::string> render(int page, int w_px, int h_px) = 0;

  // True when the page count depends on the layout size (EPUB/FB2); false
  // for fixed layouts (PDF/XPS), where relayout() is a no-op.
  virtual bool reflowable() const = 0;
  virtual void relayout(int w_px, int h_px) = 0;

  virtual Bookmark mark(int page) = 0;
  virtual int lookup(const Bookmark& b) = 0;  // -1 = unknown.
};

// The one resize flow, in the one order: mark -> relayout -> lookup -> remap.
// Fixed-layout sources return the state untouched (nothing moved).
[[nodiscard]] ViewState relayout_and_remap(DocSource& src, ViewState s,
                                           int w_px, int h_px);

}  // namespace shigoku::view
