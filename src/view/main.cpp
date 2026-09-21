// main.cpp — shigoku-view event loop. Ties the pure pager core (pager.hpp)
// to the SDL layer (sdl_compat.hpp) and the two page sources:
// parse -> classify -> [images: page list, stb decode with ±1 prefetch on
// one worker, scale to the fit rect | documents: open once through the
// DocSource seam, rasterise each page at the fit rect] -> upload -> present;
// on every normal exit write the LAST_PAGE report the TUI parses for
// mid-chapter resume.
//
// The scale-to-fit-rect-then-upload-1:1 policy (not "upload native, let the
// renderer scale") keeps the software renderer path cheap — the one that runs
// under SDL_VIDEODRIVER=dummy here and on the PPC target's non-GL path.
// Documents get the same policy for free: they are vector art, so the fit
// rect is simply the size they are rasterised at, and a zoom is a bigger
// rasterisation rather than a magnified bitmap.
//
// A reflowable book (EPUB, FB2) has no fixed pages — the window size decides
// how many there are — so every change of viewport goes through one hook,
// on_viewport(), which relays the book out and carries the reading position
// across (docsrc.hpp's relayout_and_remap). Images take the same hook and
// only rescale. Documents are rendered on this thread and never prefetched:
// the decode cache and its worker exist in image mode only.
//
// Test seam (documented, like the tree's SHIGOKU_LIVE / stb_selftest):
// SHIGOKU_VIEW_SELFTEST=N renders the start page, applies N "next" advances,
// then walks the zoom ladder and a fullscreen round trip (so the zoomed
// scale/upload/present path runs too), writes the report, and exits 0 — a
// headless end-to-end smoke with no display and no key injection. The report
// is the page, which the zoom/fullscreen part leaves alone — except for a
// reflowable book, whose fullscreen round trip may legitimately renumber it.
// Unset in normal use.

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb/stb_image.h>
#include <stb/stb_image_resize2.h>

#include "docsrc.hpp"
#include "pager.hpp"
#include "sdl_compat.hpp"
#include "../webp_decode.hpp"
#ifdef HAVE_MUPDF
#include "mupdf_source.hpp"
#endif

namespace {

using namespace shigoku::view;

// Initial window size (portrait comic default) + the clamp when sizing from
// the first page's native dimensions.
constexpr int kInitW = 900;
constexpr int kInitH = 1300;
constexpr int kMinW = 320, kMinH = 240, kMaxW = 1400, kMaxH = 2000;

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

struct Decoded {
  std::vector<std::uint8_t> rgba;  // w*h*4, RGBA row-major.
  int w = 0;
  int h = 0;
  bool ok = false;
};

// WeebCentral/Dynasty/nhentai pages can be WebP, which stb_image cannot
// read. Read the file into memory first (was a direct stbi_load(path)
// before) so a RIFF….WEBP body can be sniffed and routed to libwebp; every
// other format still falls through to stb, unchanged.
std::shared_ptr<Decoded> decode_page(const std::string& path) {
  auto d = std::make_shared<Decoded>();
  std::ifstream f(path, std::ios::binary);
  if (!f) return d;  // ok = false.
  std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
  if (bytes.empty()) return d;

  if (shigoku::looks_like_webp(bytes.data(), bytes.size())) {
#ifdef HAVE_WEBP
    int w = 0, h = 0;
    std::uint8_t* px =
        shigoku::decode_webp(bytes.data(), bytes.size(), &w, &h);
    if (px == nullptr || w <= 0 || h <= 0) {
      if (px != nullptr) shigoku::free_webp_pixels(px);
      return d;  // ok = false.
    }
    const std::size_t n = static_cast<std::size_t>(w) * h * 4u;
    d->rgba.assign(px, px + n);
    d->w = w;
    d->h = h;
    d->ok = true;
    shigoku::free_webp_pixels(px);
#endif
    return d;  // ok = false when libwebp is unavailable (WITH_WEBP).
  }

  int w = 0, h = 0, comp = 0;
  stbi_uc* px = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                      &w, &h, &comp, /*desired=*/4);
  if (px == nullptr || w <= 0 || h <= 0) {
    if (px != nullptr) stbi_image_free(px);
    return d;  // ok = false.
  }
  const std::size_t n = static_cast<std::size_t>(w) * h * 4u;
  d->rgba.assign(px, px + n);
  d->w = w;
  d->h = h;
  d->ok = true;
  stbi_image_free(px);
  return d;
}

// Decoded-page cache with a single prefetch worker (3 slots: cur-1/cur/cur+1).
// A1-ish: the worker owns only its request queue + the shared cache under a
// mutex, and is joined in the destructor (it touches no SDL, so joining is
// safe — unlike the TUI's detached workers).
class PageCache {
 public:
  explicit PageCache(std::vector<std::string> files) : files_(std::move(files)) {
    worker_ = std::thread([this] { run(); });
  }
  ~PageCache() {
    {
      std::lock_guard<std::mutex> lk(m_);
      stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
  }
  PageCache(const PageCache&) = delete;
  PageCache& operator=(const PageCache&) = delete;

  // Blocking fetch: cache hit, else decode inline (the current page must be
  // ready now; the worker only hides neighbor latency).
  std::shared_ptr<Decoded> get(int i) {
    if (i < 0 || i >= static_cast<int>(files_.size())) return std::make_shared<Decoded>();
    {
      std::lock_guard<std::mutex> lk(m_);
      auto it = cache_.find(i);
      if (it != cache_.end()) return it->second;
    }
    auto d = decode_page(files_[static_cast<std::size_t>(i)]);
    {
      std::lock_guard<std::mutex> lk(m_);
      cache_[i] = d;
    }
    return d;
  }

  // Ask the worker to warm page `i` (no-op if out of range or already cached).
  void prefetch(int i) {
    if (i < 0 || i >= static_cast<int>(files_.size())) return;
    {
      std::lock_guard<std::mutex> lk(m_);
      if (cache_.count(i)) return;
      requests_.push_back(i);
    }
    cv_.notify_one();
  }

  // Drop everything outside [cur-1, cur+1] (the 3-slot bound).
  void retain(int cur) {
    std::lock_guard<std::mutex> lk(m_);
    for (auto it = cache_.begin(); it != cache_.end();) {
      if (it->first < cur - 1 || it->first > cur + 1) {
        it = cache_.erase(it);
      } else {
        ++it;
      }
    }
  }

 private:
  void run() {
    for (;;) {
      int want = -1;
      {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [this] { return stop_ || !requests_.empty(); });
        if (stop_) return;
        want = requests_.front();
        requests_.pop_front();
        if (cache_.count(want)) continue;  // filled since it was queued.
      }
      auto d = decode_page(files_[static_cast<std::size_t>(want)]);
      {
        std::lock_guard<std::mutex> lk(m_);
        cache_[want] = d;
      }
    }
  }

  std::vector<std::string> files_;
  std::mutex m_;
  std::condition_variable cv_;
  std::map<int, std::shared_ptr<Decoded>> cache_;
  std::deque<int> requests_;
  bool stop_ = false;
  std::thread worker_;
};

// Scale native RGBA to dst_w x dst_h (sRGB-correct, RGBA). Empty on any bad dim.
std::vector<std::uint8_t> scale_rgba(const Decoded& src, int dst_w, int dst_h) {
  std::vector<std::uint8_t> out;
  if (!src.ok || dst_w <= 0 || dst_h <= 0) return out;
  out.assign(static_cast<std::size_t>(dst_w) * dst_h * 4u, 0);
  unsigned char* r = stbir_resize_uint8_srgb(
      src.rgba.data(), src.w, src.h, /*in_stride=*/0, out.data(), dst_w, dst_h,
      /*out_stride=*/0, STBIR_RGBA);
  if (r == nullptr) out.clear();
  return out;
}

void write_report(const Options& opt, int page) {
  const std::string line = report_line(page);
  if (opt.report_file.has_value()) {
    std::ofstream f(*opt.report_file, std::ios::trunc);
    if (f) f << line;  // best-effort; the viewer's job is done regardless.
  } else {
    std::fputs(line.c_str(), stdout);
    std::fflush(stdout);
  }
}

}  // namespace

int main(int argc, char** argv) {
  auto parsed = parse_cli(argc, argv);
  if (!parsed.has_value()) {
    std::fputs(parsed.error().c_str(), stderr);
    std::fputc('\n', stderr);
    return 2;
  }
  const Options opt = *parsed;
  if (opt.help) {
    std::fputs(usage().c_str(), stdout);
    return 0;
  }

  // What was asked for, decided by extension alone (classify_paths): a
  // document is opened through the DocSource seam, an archive is the
  // libarchive work's, anything else is the image path. A build without the
  // library says so here rather than reporting "no pages".
  auto plan = classify_paths(opt.paths);
  if (!plan.has_value()) {
    std::fputs(plan.error().c_str(), stderr);
    std::fputc('\n', stderr);
    return 2;
  }

  // Exactly one of these is live: `doc` in document mode, `pages` (and the
  // decode cache built over it) in image mode.
  std::unique_ptr<DocSource> doc;
  std::vector<std::string> pages;
  if (plan->kind == SourceKind::Document) {
#ifdef HAVE_MUPDF
    // A reflowable book is laid out at the default window size here, and the
    // window then opens at the page size that produced, so the two agree
    // unless the window manager has other ideas — on_viewport() below
    // settles that before the first render.
    auto opened = MupdfSource::open(plan->path, kInitW, kInitH);
    if (!opened.has_value()) {
      std::fprintf(stderr, "shigoku-view: %s\n", opened.error().c_str());
      return 5;
    }
    doc = std::move(*opened);
#else
    std::fputs("shigoku-view: built without PDF/EPUB support (libmupdf not "
               "found at build time, or -DWITH_MUPDF=OFF)\n", stderr);
    return 5;
#endif
  } else if (plan->kind == SourceKind::Archive) {
    std::fputs("shigoku-view: built without archive support (libarchive not "
               "found at build time, or -DWITH_LIBARCHIVE=OFF)\n", stderr);
    return 5;
  } else {
    pages = build_page_list(opt.paths);
    if (pages.empty()) {
      std::fputs("shigoku-view: no .jpg/.jpeg/.png/.webp pages in the given path(s)\n",
                 stderr);
      return 3;
    }
  }

  ViewState state;
  // For a reflowable book this is the count of the layout just made, so the
  // start page is clamped against the numbering the reader will see.
  state.page_count = doc ? doc->page_count() : static_cast<int>(pages.size());
  state.page = clampi(opt.start_page - 1, 0, state.page_count - 1);
  state.fit = opt.fit;
  state.rtl = opt.rtl;

  std::unique_ptr<PageCache> cache;
  if (!doc) cache = std::make_unique<PageCache>(pages);

  // Size the window from the start page's native dimensions (a document's are
  // its 96-dpi page size) so it opens at the page's aspect; anything
  // unmeasurable falls back to the portrait default.
  int init_w = kInitW, init_h = kInitH;
  std::shared_ptr<Decoded> first;
  if (doc) {
    const PageSize ps = doc->page_size(state.page);
    if (ps.w > 0 && ps.h > 0) {
      init_w = clampi(ps.w, kMinW, kMaxW);
      init_h = clampi(ps.h, kMinH, kMaxH);
    }
  } else {
    first = cache->get(state.page);
    if (first->ok) {
      init_w = clampi(first->w, kMinW, kMaxW);
      init_h = clampi(first->h, kMinH, kMaxH);
    }
  }

  const std::string title = opt.title.value_or("shigoku-view");
  auto backend = Backend::create(title, init_w, init_h);
  if (!backend.has_value()) {
    std::fprintf(stderr, "shigoku-view: SDL init failed: %s\n",
                 Backend::last_error().c_str());
    return 4;
  }

  if (cache) {
    cache->retain(state.page);
    cache->prefetch(state.page - 1);
    cache->prefetch(state.page + 1);
  }

  // Page-number HUD: the page images carry no numbering of their own, so
  // the viewer surfaces its own count — a corner overlay (p toggles) plus
  // the title bar, which survives with the HUD off. The count is part of the
  // key because a relayout can change it under an unchanged page index.
  constexpr int kHudScale = 2;
  int hud_page = -1;
  int hud_count = -1;
  int hud_zoom = 0;
  bool hud_on = !state.hud;  // impossible values force the first sync.
  auto sync_hud = [&]() {
    if (state.page == hud_page && state.page_count == hud_count &&
        state.hud == hud_on && state.zoom == hud_zoom) {
      return;
    }
    hud_page = state.page;
    hud_count = state.page_count;
    hud_zoom = state.zoom;
    hud_on = state.hud;
    backend->set_title(title + "  [" + hud_text(state) + "]");
    if (state.hud) {
      const HudImage hud = render_hud(hud_text(state), kHudScale);
      backend->set_overlay(hud.rgba.data(), hud.w, hud.h);
    } else {
      backend->set_overlay(nullptr, 0, 0);
    }
  };

  // Render pipeline: rescale only when page/size/fit/zoom changed; scrolling
  // and panning just re-offset the same texture. An image is scaled ONCE,
  // from native straight to the on-screen size — a zoom is a different scale
  // of the original, never a magnified copy of the fit-sized one. A document
  // page is rasterised at that same on-screen size, so it is sharp at every
  // zoom step; a page that fails to render shows as black and is logged
  // once, so the reader can page past it.
  bool need_rescale = true;
  Rect shown_rect;
  int content_w = 0;
  int content_h = 0;
  int logged_page = -1;
  auto render = [&]() {
    sync_hud();
    if (need_rescale) {
      const int win_w = backend->width();
      const int win_h = backend->height();
      shown_rect = Rect{};
      content_w = 0;
      content_h = 0;
      if (doc) {
        PageSize ps = doc->page_size(state.page);
        if (ps.w <= 0 || ps.h <= 0) ps = PageSize{win_w, win_h};  // unmeasurable.
        const Rect fit = fit_rect(ps.w, ps.h, win_w, win_h, state.fit);
        Rect want = apply_zoom(fit, win_w, win_h, state.zoom);
        // A zero rect (a minimised window) is nothing to render, not an error.
        if (want.w > 0 && want.h > 0) {
          auto img = doc->render(state.page, want.w, want.h);
          if (img.has_value()) {
            backend->set_texture(img->rgba.data(), img->w, img->h);
            want.w = img->w;  // the rasteriser's own dims are the truth.
            want.h = img->h;
            shown_rect = want;
            content_w = want.w;
            content_h = want.h;
          } else if (logged_page != state.page) {
            logged_page = state.page;
            std::fprintf(stderr, "shigoku-view: page %d: %s\n", state.page + 1,
                         img.error().c_str());
          }
        }
      } else {
        std::shared_ptr<Decoded> pg = cache->get(state.page);
        if (pg->ok) {
          const Rect fit = fit_rect(pg->w, pg->h, win_w, win_h, state.fit);
          shown_rect = apply_zoom(fit, win_w, win_h, state.zoom);
          std::vector<std::uint8_t> scaled = scale_rgba(*pg, shown_rect.w, shown_rect.h);
          if (!scaled.empty()) backend->set_texture(scaled.data(), shown_rect.w, shown_rect.h);
          content_w = shown_rect.w;
          content_h = shown_rect.h;
        }
      }
      need_rescale = false;
    }
    Rect dst = shown_rect;
    dst.x = shown_rect.x - state.scroll_x;  // pan (zoomed past the window edge).
    dst.y = shown_rect.y - state.scroll_y;  // scroll (fit-width / zoomed).
    backend->render(dst);
  };

  // The one viewport hook: called after anything that can change the window
  // size — a Resize event, a fullscreen toggle, and once right after the
  // window opens (it may not have got the size it asked for). A document is
  // relaid out for the new size with the reading position carried across; an
  // unchanged size is never relaid out, so the fullscreen toggle and the
  // Resize SDL reports for it cannot renumber the book twice. Images only
  // rescale. `laid_w/h` start at the size the document was opened at.
  int laid_w = kInitW;
  int laid_h = kInitH;
  auto on_viewport = [&]() {
    need_rescale = true;
    const int w = backend->width();
    const int h = backend->height();
    if (w == laid_w && h == laid_h) return;
    laid_w = w;
    laid_h = h;
    if (doc) state = relayout_and_remap(*doc, state, w, h);
  };

  on_viewport();
  render();

  // Headless test seam: apply N "next" advances, walk the zoom ladder and a
  // fullscreen round trip (both go through scale/upload/present with no
  // display attached and no key injection), report, exit — no event loop. The
  // report is the page, which the zoom/fullscreen part deliberately leaves
  // alone, so the seam's contract is unchanged by that exercise.
  if (const char* st = std::getenv("SHIGOKU_VIEW_SELFTEST")) {
    const int n = std::atoi(st);
    auto viewport = [&] {
      return Viewport{content_h, backend->height(), content_w, backend->width()};
    };
    for (int i = 0; i < n; ++i) {
      state = advance(state, Key::Space, viewport());
      need_rescale = true;
      if (cache) cache->retain(state.page);
      render();
    }
    for (Key k : {Key::ZoomIn, Key::ZoomIn, Key::ZoomOut, Key::ZoomReset}) {
      state = advance(state, k, viewport());
      need_rescale = true;
      render();
    }
    backend->set_fullscreen(true);
    on_viewport();
    render();
    backend->set_fullscreen(false);
    on_viewport();
    render();
    write_report(opt, state.page);
    return 0;
  }

  while (!state.quit) {
    const ViewEvent ev = backend->wait_event(/*timeout_ms=*/200);
    switch (ev.type) {
      case ViewEvent::Type::None:
        break;  // timeout — idle.
      case ViewEvent::Type::Quit:
        state.quit = true;
        break;
      case ViewEvent::Type::Resize:
        on_viewport();
        render();
        break;
      case ViewEvent::Type::Key: {
        // The window is the authority on fullscreen: the system can take it in
        // or out (green button, space switch) without asking us, and a stale
        // flag here would invert the next toggle.
        state.fullscreen = backend->is_fullscreen();
        const Viewport vp{content_h, backend->height(), content_w, backend->width()};
        const ViewState ns = advance(state, ev.key, vp);
        if (ns == state) break;
        const bool rescale = ns.page != state.page || ns.fit != state.fit ||
                             ns.zoom != state.zoom;
        const bool fs = ns.fullscreen != state.fullscreen;
        const bool paged = ns.page != state.page;
        state = ns;
        if (fs) {
          backend->set_fullscreen(state.fullscreen);
          on_viewport();  // the window just changed size under us.
        }
        if (rescale) need_rescale = true;
        if (paged && cache) {
          cache->retain(state.page);
          cache->prefetch(state.page - 1);
          cache->prefetch(state.page + 1);
        }
        render();
        break;
      }
    }
  }

  write_report(opt, state.page);
  return 0;
}
