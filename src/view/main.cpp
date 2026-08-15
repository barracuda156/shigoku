// main.cpp — shigoku-view event loop. Ties the pure pager core (pager.hpp)
// to the SDL layer (sdl_compat.hpp) and the stb decode:
// parse -> page list -> decode (with ±1 prefetch on one worker) -> scale to
// the fit rect -> upload -> present; on every normal exit write the LAST_PAGE
// report the TUI parses for mid-chapter resume.
//
// The scale-to-fit-rect-then-upload-1:1 policy (not "upload native, let the
// renderer scale") keeps the software renderer path cheap — the one that runs
// under SDL_VIDEODRIVER=dummy here and on the PPC target's non-GL path.
//
// Test seam (documented, like the tree's SHIGOKU_LIVE / stb_selftest):
// SHIGOKU_VIEW_SELFTEST=N renders the start page, applies N "next" advances,
// then walks the zoom ladder and a fullscreen round trip (so the zoomed
// scale/upload/present path runs too), writes the report, and exits 0 — a
// headless end-to-end smoke with no display and no key injection. The report
// is the page, which the zoom/fullscreen part leaves alone. Unset in normal use.

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
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb/stb_image.h>
#include <stb/stb_image_resize2.h>

#include "pager.hpp"
#include "sdl_compat.hpp"
#include "../webp_decode.hpp"

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

  const std::vector<std::string> pages = build_page_list(opt.paths);
  if (pages.empty()) {
    std::fputs("shigoku-view: no .jpg/.jpeg/.png/.webp pages in the given path(s)\n",
               stderr);
    return 3;
  }

  ViewState state;
  state.page_count = static_cast<int>(pages.size());
  state.page = clampi(opt.start_page - 1, 0, state.page_count - 1);
  state.fit = opt.fit;
  state.rtl = opt.rtl;

  PageCache cache(pages);

  // Decode the start page first so the window can open at its aspect.
  std::shared_ptr<Decoded> first = cache.get(state.page);
  int init_w = kInitW, init_h = kInitH;
  if (first->ok) {
    init_w = clampi(first->w, kMinW, kMaxW);
    init_h = clampi(first->h, kMinH, kMaxH);
  }

  const std::string title = opt.title.value_or("shigoku-view");
  auto backend = Backend::create(title, init_w, init_h);
  if (!backend.has_value()) {
    std::fprintf(stderr, "shigoku-view: SDL init failed: %s\n",
                 Backend::last_error().c_str());
    return 4;
  }

  cache.retain(state.page);
  cache.prefetch(state.page - 1);
  cache.prefetch(state.page + 1);

  // Page-number HUD: the page images carry no numbering of their own, so
  // the viewer surfaces its own count — a corner overlay (p toggles) plus
  // the title bar, which survives with the HUD off.
  constexpr int kHudScale = 2;
  int hud_page = -1;
  int hud_zoom = 0;
  bool hud_on = !state.hud;  // impossible values force the first sync.
  auto sync_hud = [&]() {
    if (state.page == hud_page && state.hud == hud_on && state.zoom == hud_zoom) {
      return;
    }
    hud_page = state.page;
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
  // and panning just re-offset the same texture. The page is scaled ONCE, from
  // native straight to the on-screen size — a zoom is a different scale of the
  // original, never a magnified copy of the fit-sized one.
  bool need_rescale = true;
  Rect shown_rect;
  int content_w = 0;
  int content_h = 0;
  auto render = [&]() {
    sync_hud();
    if (need_rescale) {
      std::shared_ptr<Decoded> pg = cache.get(state.page);
      if (pg->ok) {
        const int win_w = backend->width();
        const int win_h = backend->height();
        const Rect fit = fit_rect(pg->w, pg->h, win_w, win_h, state.fit);
        shown_rect = apply_zoom(fit, win_w, win_h, state.zoom);
        std::vector<std::uint8_t> scaled = scale_rgba(*pg, shown_rect.w, shown_rect.h);
        if (!scaled.empty()) backend->set_texture(scaled.data(), shown_rect.w, shown_rect.h);
        content_w = shown_rect.w;
        content_h = shown_rect.h;
      } else {
        shown_rect = Rect{};
        content_w = 0;
        content_h = 0;
      }
      need_rescale = false;
    }
    Rect dst = shown_rect;
    dst.x = shown_rect.x - state.scroll_x;  // pan (zoomed past the window edge).
    dst.y = shown_rect.y - state.scroll_y;  // scroll (fit-width / zoomed).
    backend->render(dst);
  };

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
      cache.retain(state.page);
      render();
    }
    for (Key k : {Key::ZoomIn, Key::ZoomIn, Key::ZoomOut, Key::ZoomReset}) {
      state = advance(state, k, viewport());
      need_rescale = true;
      render();
    }
    backend->set_fullscreen(true);
    need_rescale = true;
    render();
    backend->set_fullscreen(false);
    need_rescale = true;
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
        need_rescale = true;
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
        const bool relayout = ns.page != state.page || ns.fit != state.fit ||
                              ns.zoom != state.zoom;
        const bool fs = ns.fullscreen != state.fullscreen;
        const bool paged = ns.page != state.page;
        state = ns;
        if (fs) {
          backend->set_fullscreen(state.fullscreen);
          need_rescale = true;  // the window just changed size under us.
        }
        if (relayout) need_rescale = true;
        if (paged) {
          cache.retain(state.page);
          cache.prefetch(state.page - 1);
          cache.prefetch(state.page + 1);
        }
        render();
        break;
      }
    }
  }

  write_report(opt, state.page);
  return 0;
}
