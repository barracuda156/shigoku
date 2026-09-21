// mupdf_source.cpp — the libmupdf half of the document seam.
//
// Two rules shape every line below.
//
// 1. fz_try/fz_catch are setjmp/longjmp. A longjmp out of a block skips the
//    destructors of anything declared inside it, so no object with a
//    non-trivial destructor (no std::string, no std::vector, no Result) may
//    be declared inside an fz_try — and anything assigned inside and read
//    afterwards needs fz_var, or the compiler may keep it in a register the
//    longjmp restores. Every mupdf call therefore sits in a small helper
//    whose try block holds nothing but ints, floats and raw pointers; the
//    C++ objects are built from the results afterwards.
// 2. A page arrives as a pixmap allocated by mupdf. The copy into RgbaImage
//    allocates, and an allocation inside an fz_try would be a C++ exception
//    escaping through the setjmp frame, so the pixmap is carried OUT of the
//    try block and dropped after the copy instead of in fz_always.
//
// Rendering is always a fresh rasterisation at the requested size: documents
// are vector art, so a zoom is a bigger render rather than a magnified
// bitmap, and the result is sharp at every step of the ladder.

#include "mupdf_source.hpp"

#include <mupdf/fitz.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace shigoku::view {

namespace {

// A document's own unit is the point; the viewer's geometry is in pixels at
// 96 dpi, so the two conversions below are the whole of the unit story.
constexpr double kPxPerPt = 96.0 / 72.0;

// Nominal font size for a reflowable layout. One value for now; the reader
// has no type-size control yet.
constexpr float kEmPt = 12.0F;

// Layout size used when the caller has no window yet (open() called with a
// non-positive size). Roughly a paperback page at 96 dpi.
constexpr int kDefaultLayoutW = 600;
constexpr int kDefaultLayoutH = 800;

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

float to_pt(int px) { return static_cast<float>(static_cast<double>(px) / kPxPerPt); }

int to_px(float pt) { return static_cast<int>(static_cast<double>(pt) * kPxPerPt + 0.5); }

// What open_document() managed to learn. `doc` is null on failure, and then
// *why explains it.
struct Opened {
  fz_document* doc = nullptr;
  int reflowable = 0;
  int pages = 0;
};

// Open, check for a password, lay a reflowable out once, count the pages.
// On failure nothing is left open and *why carries the reason.
//
// The message is copied out BEFORE anything is dropped: fz_caught_message
// points into a single buffer inside the context that the next throw
// overwrites, and a drop may well throw and catch internally.
Opened open_document(fz_context* ctx, const char* path, float w_pt, float h_pt,
                     std::string* why) {
  fz_document* doc = nullptr;
  int reflowable = 0;
  int pages = 0;
  const char* fail = nullptr;
  fz_var(doc);
  fz_var(reflowable);
  fz_var(pages);
  fz_var(fail);
  fz_try(ctx) {
    fz_register_document_handlers(ctx);
    doc = fz_open_document(ctx, path);
    if (fz_needs_password(ctx, doc)) {
      fail = "encrypted document (password-protected files are not supported)";
    } else {
      reflowable = fz_is_document_reflowable(ctx, doc);
      if (reflowable != 0) fz_layout_document(ctx, doc, w_pt, h_pt, kEmPt);
      pages = fz_count_pages(ctx, doc);
      if (pages < 1) fail = "the document has no pages";
    }
  }
  fz_catch(ctx) { fail = fz_caught_message(ctx); }
  if (fail != nullptr) {
    why->assign(fail);
    fz_drop_document(ctx, doc);
    return Opened{};
  }
  return Opened{doc, reflowable, pages};
}

// Re-lay a reflowable document out and report the new page count, or 0 if
// the relayout failed (the caller then keeps the count it had — a failed
// relayout must not renumber the book to nothing).
int lay_out(fz_context* ctx, fz_document* doc, float w_pt, float h_pt) {
  int pages = 0;
  fz_var(pages);
  fz_try(ctx) {
    fz_layout_document(ctx, doc, w_pt, h_pt, kEmPt);
    pages = fz_count_pages(ctx, doc);
  }
  fz_catch(ctx) { pages = 0; }
  return pages;
}

// The page's bounding box in points, or an empty rect when it cannot be
// loaded.
fz_rect bound_page(fz_context* ctx, fz_document* doc, int page) {
  fz_rect box = fz_rect{0, 0, 0, 0};
  fz_page* pg = nullptr;
  fz_var(box);
  fz_var(pg);
  fz_try(ctx) {
    pg = fz_load_page(ctx, doc, page);
    box = fz_bound_page(ctx, pg);
  }
  fz_always(ctx) { fz_drop_page(ctx, pg); }
  fz_catch(ctx) { box = fz_rect{0, 0, 0, 0}; }
  return box;
}

// Rasterise `page` to exactly w_px x h_px (mupdf may still round a dimension
// by a pixel — the pixmap's own size is the truth). Returns null with *why
// set on failure. The pixmap outlives the try block on purpose; the caller
// drops it once the samples are copied out.
//
// The page is dropped after the whole construct rather than in fz_always so
// that the error message is copied out first — fz_always runs ahead of
// fz_catch, and a drop is free to reuse the context's message buffer. Both
// paths reach the code below: fz_catch falls through.
fz_pixmap* render_pixmap(fz_context* ctx, fz_document* doc, int page, int w_px,
                         int h_px, std::string* why) {
  fz_pixmap* pix = nullptr;
  fz_page* pg = nullptr;
  const char* fail = nullptr;
  fz_var(pix);
  fz_var(pg);
  fz_var(fail);
  fz_try(ctx) {
    pg = fz_load_page(ctx, doc, page);
    const fz_rect box = fz_bound_page(ctx, pg);
    const float bw = box.x1 - box.x0;
    const float bh = box.y1 - box.y0;
    if (bw <= 0.0F || bh <= 0.0F)
      fz_throw(ctx, FZ_ERROR_GENERIC, "page %d has an empty bounding box", page + 1);
    const fz_matrix ctm =
        fz_scale(static_cast<float>(w_px) / bw, static_cast<float>(h_px) / bh);
    // alpha = 0: an opaque RGB pixmap on a white ground, which is what a
    // page on screen should look like and saves compositing later.
    pix = fz_new_pixmap_from_page(ctx, pg, ctm, fz_device_rgb(ctx), /*alpha=*/0);
  }
  fz_catch(ctx) { fail = fz_caught_message(ctx); }
  if (fail != nullptr) why->assign(fail);
  fz_drop_page(ctx, pg);
  if (fail == nullptr) return pix;
  fz_drop_pixmap(ctx, pix);
  return nullptr;
}

// Everything a Bookmark needs about one page's position, gathered in one
// pass over the document's chapter structure.
struct MarkInfo {
  fz_bookmark mark = 0;
  int chapter = 0;
  int chapter_page = 0;
  int chapter_pages = 0;
};

MarkInfo make_mark(fz_context* ctx, fz_document* doc, int page) {
  MarkInfo info;
  fz_var(info);
  fz_try(ctx) {
    const fz_location loc = fz_location_from_page_number(ctx, doc, page);
    if (loc.chapter >= 0 && loc.page >= 0) {
      info.chapter = loc.chapter;
      info.chapter_page = loc.page;
      info.chapter_pages = fz_count_chapter_pages(ctx, doc, loc.chapter);
      info.mark = fz_make_bookmark(ctx, doc, loc);
    }
  }
  // A position that cannot be taken is not an error the reader can act on:
  // whatever was gathered before the throw still feeds the coarse fallback,
  // and the worst case is landing at the top of the book.
  fz_catch(ctx) {}
  return info;
}

// Where a bookmark lands in the CURRENT layout. mupdf answers directly when
// it can; when it cannot — it returns no mark for the last page(s) of a
// chapter, because no text flow starts there — the chapter and the fraction
// through it place the reader proportionally instead. -1 only for a document
// with no chapters or no pages at all.
int lookup_page(fz_context* ctx, fz_document* doc, fz_bookmark mark, int chapter,
                double frac) {
  int page = -1;
  fz_var(page);
  fz_try(ctx) {
    if (mark != 0) {
      const fz_location loc = fz_lookup_bookmark(ctx, doc, mark);
      if (loc.chapter >= 0 && loc.page >= 0)
        page = fz_page_number_from_location(ctx, doc, loc);
    }
    if (page < 0) {
      const int chapters = fz_count_chapters(ctx, doc);
      if (chapters > 0) {
        const int ch = clampi(chapter, 0, chapters - 1);
        const int n = fz_count_chapter_pages(ctx, doc, ch);
        if (n > 0) {
          const int p = clampi(static_cast<int>(frac * n + 0.5), 0, n - 1);
          page = fz_page_number_from_location(ctx, doc, fz_make_location(ch, p));
        }
      }
    }
  }
  fz_catch(ctx) { page = -1; }
  return page;
}

}  // namespace

// The library handles, out of the header's sight. The destructor is the only
// place they are released, so every early return in open() is leak-free.
struct MupdfSource::Doc {
  fz_context* ctx = nullptr;
  fz_document* doc = nullptr;
  std::string path;
  bool reflow = false;
  int pages = 1;

  Doc() = default;
  Doc(const Doc&) = delete;
  Doc& operator=(const Doc&) = delete;
  ~Doc() {
    if (ctx == nullptr) return;
    fz_drop_document(ctx, doc);
    fz_drop_context(ctx);
  }
};

MupdfSource::MupdfSource(std::unique_ptr<Doc> doc) : impl_(std::move(doc)) {}

MupdfSource::~MupdfSource() = default;

Result<std::unique_ptr<MupdfSource>, std::string> MupdfSource::open(
    const std::string& path, int w_px, int h_px) {
  auto d = std::unique_ptr<Doc>(new Doc());
  d->path = path;
  d->ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
  if (d->ctx == nullptr) return err(path + ": cannot create a mupdf context");

  const int lw = w_px > 0 ? w_px : kDefaultLayoutW;
  const int lh = h_px > 0 ? h_px : kDefaultLayoutH;
  std::string why;
  const Opened opened =
      open_document(d->ctx, path.c_str(), to_pt(lw), to_pt(lh), &why);
  if (opened.doc == nullptr)
    return err(path + ": " + (why.empty() ? "cannot open the document" : why));

  d->doc = opened.doc;
  d->reflow = opened.reflowable != 0;
  d->pages = opened.pages;
  return std::unique_ptr<MupdfSource>(new MupdfSource(std::move(d)));
}

int MupdfSource::page_count() const { return impl_->pages; }

bool MupdfSource::reflowable() const { return impl_->reflow; }

PageSize MupdfSource::page_size(int page) const {
  if (page < 0 || page >= impl_->pages) return PageSize{};
  const fz_rect box = bound_page(impl_->ctx, impl_->doc, page);
  const int w = to_px(box.x1 - box.x0);
  const int h = to_px(box.y1 - box.y0);
  if (w <= 0 || h <= 0) return PageSize{};
  return PageSize{w, h};
}

void MupdfSource::relayout(int w_px, int h_px) {
  if (!impl_->reflow || w_px <= 0 || h_px <= 0) return;
  const int pages = lay_out(impl_->ctx, impl_->doc, to_pt(w_px), to_pt(h_px));
  if (pages > 0) impl_->pages = pages;
}

Bookmark MupdfSource::mark(int page) {
  if (page < 0 || page >= impl_->pages) return Bookmark{};
  const MarkInfo info = make_mark(impl_->ctx, impl_->doc, page);
  const double frac = info.chapter_pages > 0
                          ? static_cast<double>(info.chapter_page) /
                                static_cast<double>(info.chapter_pages)
                          : 0.0;
  return Bookmark{info.mark, info.chapter, frac};
}

int MupdfSource::lookup(const Bookmark& b) {
  return lookup_page(impl_->ctx, impl_->doc, static_cast<fz_bookmark>(b.mark),
                     b.chapter, b.frac);
}

Result<RgbaImage, std::string> MupdfSource::render(int page, int w_px, int h_px) {
  if (page < 0 || page >= impl_->pages)
    return err(impl_->path + ": page out of range");
  if (w_px <= 0 || h_px <= 0) return err(impl_->path + ": empty render size");
  // The same ceiling the zoom ladder respects, applied here too so no single
  // page allocation can be unbounded whatever asked for it.
  if (static_cast<long long>(w_px) * h_px > kMaxZoomPixels)
    return err(impl_->path + ": render size too large");

  std::string why;
  fz_pixmap* pix = render_pixmap(impl_->ctx, impl_->doc, page, w_px, h_px, &why);
  if (pix == nullptr)
    return err(impl_->path + ": " + (why.empty() ? "cannot render the page" : why));

  const int w = fz_pixmap_width(impl_->ctx, pix);
  const int h = fz_pixmap_height(impl_->ctx, pix);
  const int n = fz_pixmap_components(impl_->ctx, pix);
  const int stride = fz_pixmap_stride(impl_->ctx, pix);
  const unsigned char* src = fz_pixmap_samples(impl_->ctx, pix);
  if (w <= 0 || h <= 0 || n < 3 || src == nullptr) {
    fz_drop_pixmap(impl_->ctx, pix);
    return err(impl_->path + ": the page rendered to nothing");
  }

  RgbaImage img;
  img.w = w;
  img.h = h;
  img.rgba.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
  // Byte per channel, never a packed 32-bit store: the RGBA32 upload is
  // byte-ordered, so this stays correct on a big-endian host.
  for (int y = 0; y < h; ++y) {
    const unsigned char* s = src + static_cast<std::ptrdiff_t>(y) * stride;
    std::uint8_t* d =
        img.rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 4;
    for (int x = 0; x < w; ++x, s += n, d += 4) {
      d[0] = s[0];
      d[1] = s[1];
      d[2] = s[2];
      d[3] = n >= 4 ? s[3] : 255;
    }
  }
  fz_drop_pixmap(impl_->ctx, pix);
  return img;
}

}  // namespace shigoku::view
