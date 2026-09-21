// mupdf_source.hpp — DocSource over libmupdf: PDF, EPUB, FB2, XPS, OXPS.
//
// This header deliberately names no mupdf type. The library's handles live
// behind an opaque `Doc`, so the viewer's own translation units keep their
// include set (and their warning posture) free of fitz.h, and the seam stays
// the only thing they know about documents.
//
// Threading: one MupdfSource owns one fz_context and is NOT safe to use from
// two threads. The viewer renders documents on the main thread; nothing hands
// one to the image prefetch worker.
//
// This TU is compiled only when the build found libmupdf (SHIGOKU_HAVE_MUPDF
// in CMake, HAVE_MUPDF in the code); callers guard their use accordingly.

#pragma once

#include <memory>
#include <string>

#include "../result.hpp"
#include "docsrc.hpp"

namespace shigoku::view {

class MupdfSource final : public DocSource {
 public:
  // Open `path`. `w_px` x `h_px` is the initial layout size for reflowable
  // books (EPUB/FB2) — a fixed-layout document ignores it. Err carries a
  // reader-facing sentence already prefixed with the path: an unreadable or
  // unrecognised file, a document with no pages, or an encrypted one
  // (passwords are not supported).
  [[nodiscard]] static Result<std::unique_ptr<MupdfSource>, std::string> open(
      const std::string& path, int w_px, int h_px);

  ~MupdfSource() override;
  MupdfSource(const MupdfSource&) = delete;
  MupdfSource& operator=(const MupdfSource&) = delete;

  [[nodiscard]] int page_count() const override;
  // Natural size in pixels at 96 dpi, or {0, 0} for a page that cannot be
  // measured (out of range, or a damaged page) — the caller falls back to the
  // window size rather than dividing by zero.
  [[nodiscard]] PageSize page_size(int page) const override;
  [[nodiscard]] Result<RgbaImage, std::string> render(int page, int w_px,
                                                      int h_px) override;
  [[nodiscard]] bool reflowable() const override;
  void relayout(int w_px, int h_px) override;
  [[nodiscard]] Bookmark mark(int page) override;
  [[nodiscard]] int lookup(const Bookmark& b) override;

 private:
  struct Doc;
  explicit MupdfSource(std::unique_ptr<Doc> doc);
  std::unique_ptr<Doc> impl_;
};

}  // namespace shigoku::view
