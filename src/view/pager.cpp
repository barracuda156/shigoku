// pager.cpp — implementation of the pure viewer core. See pager.hpp for the
// design. Everything here is deterministic and offline;
// build_page_list is the only I/O (a directory read), and it is best-effort.

#include "pager.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <system_error>

namespace shigoku::view {

namespace {

// clamp without <algorithm>'s std::clamp reference-lifetime footguns.
int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Round a non-negative double to int (dest dims are always >= 0).
int round_pos(double v) { return static_cast<int>(v + 0.5); }

bool is_image_ext(const std::filesystem::path& p) {
  std::string ext = p.extension().string();
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  // WeebCentral/Dynasty/nhentai chapters can land .webp pages (decode_page
  // in main.cpp routes those to libwebp; stb handles the rest).
  return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".webp";
}

}  // namespace

std::string usage() {
  return "usage: shigoku-view [--rtl] [--start-page N] [--report-file PATH]\n"
         "                    [--fit page|width] [--title S] <dir | file...>\n"
         "\n"
         "keys: space / PgDn / j      next page\n"
         "      backspace / PgUp / k  previous page\n"
         "      left / right          page (pans first when zoomed in)\n"
         "      up / down             scroll\n"
         "      home / end            first / last page\n"
         "      f                     fit page <-> fit width\n"
         "      + / - / 0             zoom in / out / reset (Cmd +,-,0 on macOS)\n"
         "      shift+f / F11         fullscreen (View menu on macOS)\n"
         "      p                     page-number overlay\n"
         "      q                     quit (esc leaves fullscreen first)\n";
}

Result<Options, std::string> parse_cli(int argc, char** argv) {
  Options opt;
  bool end_of_flags = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    // A value-taking flag needs its next argv; this helper fetches it or errs.
    auto take = [&](const char* name) -> Result<std::string, std::string> {
      if (i + 1 >= argc) return err(std::string("missing value for ") + name);
      return std::string(argv[++i]);
    };
    if (!end_of_flags && a == "--") {
      end_of_flags = true;
    } else if (!end_of_flags && (a == "--help" || a == "-h")) {
      opt.help = true;
    } else if (!end_of_flags && a == "--rtl") {
      opt.rtl = true;
    } else if (!end_of_flags && a == "--start-page") {
      auto v = take("--start-page");
      if (!v.has_value()) return err(v.error());
      char* endp = nullptr;
      const long n = std::strtol(v->c_str(), &endp, 10);
      if (endp != v->c_str() + v->size() || n < 1) {
        return err(std::string("--start-page must be a positive integer, got '") + *v + "'");
      }
      opt.start_page = static_cast<int>(n);
    } else if (!end_of_flags && a == "--report-file") {
      auto v = take("--report-file");
      if (!v.has_value()) return err(v.error());
      opt.report_file = *v;
    } else if (!end_of_flags && a == "--fit") {
      auto v = take("--fit");
      if (!v.has_value()) return err(v.error());
      if (*v == "page") {
        opt.fit = Fit::Page;
      } else if (*v == "width") {
        opt.fit = Fit::Width;
      } else {
        return err(std::string("--fit must be 'page' or 'width', got '") + *v + "'");
      }
    } else if (!end_of_flags && a == "--title") {
      auto v = take("--title");
      if (!v.has_value()) return err(v.error());
      opt.title = *v;
    } else if (!end_of_flags && a.size() > 2 && a.substr(0, 2) == "--") {
      return err(std::string("unknown flag: ") + std::string(a));
    } else {
      opt.paths.emplace_back(a);
    }
  }
  if (opt.help) return opt;  // usage is the whole job; paths are irrelevant.
  if (opt.paths.empty()) return err(std::string("no input path given\n") + usage());
  return opt;
}

bool natural_less(const std::string& a, const std::string& b) {
  std::size_t i = 0, j = 0;
  const std::size_t na = a.size(), nb = b.size();
  auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
  while (i < na && j < nb) {
    const char ca = a[i], cb = b[j];
    if (is_digit(ca) && is_digit(cb)) {
      // Compare maximal digit runs numerically, ignoring leading zeros so
      // "007" == "7" in magnitude (then a shorter original run tie-breaks
      // lower, so "7" < "07" for a stable total order).
      std::size_t si = i, sj = j;
      while (i < na && is_digit(a[i])) ++i;
      while (j < nb && is_digit(b[j])) ++j;
      std::string_view da(a.data() + si, i - si);
      std::string_view db(b.data() + sj, j - sj);
      std::string_view ta = da, tb = db;
      while (ta.size() > 1 && ta.front() == '0') ta.remove_prefix(1);
      while (tb.size() > 1 && tb.front() == '0') tb.remove_prefix(1);
      if (ta.size() != tb.size()) return ta.size() < tb.size();
      if (ta != tb) return ta < tb;
      if (da.size() != db.size()) return da.size() < db.size();  // "7" < "07".
      // Equal magnitude and run length: continue past the runs.
    } else {
      if (ca != cb) return static_cast<unsigned char>(ca) < static_cast<unsigned char>(cb);
      ++i;
      ++j;
    }
  }
  return (na - i) < (nb - j);  // shorter remainder sorts first.
}

std::vector<std::string> build_page_list(const std::vector<std::string>& paths) {
  namespace fs = std::filesystem;
  std::vector<std::string> out;
  std::error_code ec;

  if (paths.size() == 1 && fs::is_directory(paths[0], ec)) {
    for (fs::directory_iterator it(paths[0], ec), end; !ec && it != end; it.increment(ec)) {
      if (it->is_regular_file(ec) && is_image_ext(it->path())) {
        out.push_back(it->path().string());
      }
    }
  } else {
    for (const auto& p : paths) {
      const fs::path path(p);
      if (fs::is_regular_file(path, ec) && is_image_ext(path)) out.push_back(p);
    }
  }
  std::sort(out.begin(), out.end(), natural_less);
  return out;
}

ViewState advance(ViewState s, Key k, Viewport vp) {
  const int last = s.page_count - 1 > 0 ? s.page_count - 1 : 0;
  auto go = [&](int p) {
    const int np = clampi(p, 0, last);
    if (np != s.page) {
      s.page = np;
      s.scroll_y = 0;  // a new page always starts at the top…
      s.scroll_x = 0;  // …and at the left edge (right edge under RTL is not
                       // a thing here: the pan origin is the image's).
    }
  };
  const int max_scroll = vp.content_h - vp.viewport_h > 0 ? vp.content_h - vp.viewport_h : 0;
  const int max_pan = vp.content_w - vp.viewport_w > 0 ? vp.content_w - vp.viewport_w : 0;
  const int step = vp.viewport_h / 2;  // half-window; 0 when height unknown.
  const int pan_step = vp.viewport_w / 2;
  auto scroll = [&](int dy) { s.scroll_y = clampi(s.scroll_y + dy, 0, max_scroll); };
  auto pan = [&](int dx) { s.scroll_x = clampi(s.scroll_x + dx, 0, max_pan); };

  // Zoom keeps the window's centre over the same point of the page: the
  // content scales by nz/zoom, so the centre's offset into it does too.
  // Everything is computed from the CURRENT (already zoomed) content dims in
  // `vp`, so this needs no knowledge of the page's native size.
  auto rezoom = [&](int nz) {
    if (nz == s.zoom || s.zoom <= 0 || nz <= 0) return;
    const long long z0 = s.zoom, z1 = nz;
    const long long cw = static_cast<long long>(vp.content_w) * z1 / z0;
    const long long ch = static_cast<long long>(vp.content_h) * z1 / z0;
    const long long nx =
        (static_cast<long long>(s.scroll_x) + vp.viewport_w / 2) * z1 / z0 -
        vp.viewport_w / 2;
    const long long ny =
        (static_cast<long long>(s.scroll_y) + vp.viewport_h / 2) * z1 / z0 -
        vp.viewport_h / 2;
    const long long px = cw - vp.viewport_w > 0 ? cw - vp.viewport_w : 0;
    const long long py = ch - vp.viewport_h > 0 ? ch - vp.viewport_h : 0;
    s.scroll_x = static_cast<int>(nx < 0 ? 0 : (nx > px ? px : nx));
    s.scroll_y = static_cast<int>(ny < 0 ? 0 : (ny > py ? py : ny));
    s.zoom = nz;
  };

  switch (k) {
    case Key::Space:
    case Key::PageDown:
    case Key::J:
      go(s.page + 1);
      break;
    case Key::Backspace:
    case Key::PageUp:
    case Key::K:
      go(s.page - 1);
      break;
    case Key::ArrowRight:
      // Zoomed past the right edge, walk the page first; the page turn (the
      // RTL-mirrored one) is what the edge means.
      if (s.scroll_x < max_pan) {
        pan(pan_step);
      } else {
        go(s.rtl ? s.page - 1 : s.page + 1);  // spatial: mirrored under RTL.
      }
      break;
    case Key::ArrowLeft:
      if (s.scroll_x > 0) {
        pan(-pan_step);
      } else {
        go(s.rtl ? s.page + 1 : s.page - 1);
      }
      break;
    case Key::ArrowUp:
      scroll(-step);
      break;
    case Key::ArrowDown:
      scroll(step);
      break;
    case Key::Home:
      go(0);
      break;
    case Key::End:
      go(last);
      break;
    case Key::F:
      s.fit = s.fit == Fit::Page ? Fit::Width : Fit::Page;
      s.scroll_y = 0;  // the new layout invalidates the old offsets.
      s.scroll_x = 0;
      break;
    case Key::Fullscreen:
      s.fullscreen = !s.fullscreen;
      break;
    case Key::ZoomIn:
      rezoom(zoom_next(s.zoom));
      break;
    case Key::ZoomOut:
      rezoom(zoom_prev(s.zoom));
      break;
    case Key::ZoomReset:
      rezoom(kZoomFit);
      break;
    case Key::P:
      s.hud = !s.hud;
      break;
    case Key::Q:
      s.quit = true;
      break;
    case Key::Escape:
      // In fullscreen there is no window chrome and (on macOS) no menu bar to
      // reach, so Esc must be the way back out rather than the way out.
      if (s.fullscreen) {
        s.fullscreen = false;
      } else {
        s.quit = true;
      }
      break;
    case Key::Other:
      break;
  }
  return s;
}

namespace {

// The zoom ladder, percent of the fit size. Coarse on purpose: a reader wants
// two or three useful magnifications, not a continuum, and every step must
// survive a round trip (in then out lands where it started).
constexpr int kZoomSteps[] = {50, 75, 100, 125, 150, 200, 300};
constexpr int kZoomCount = static_cast<int>(sizeof(kZoomSteps) / sizeof(kZoomSteps[0]));

}  // namespace

int zoom_next(int percent) {
  for (int i = 0; i < kZoomCount; ++i) {
    if (kZoomSteps[i] > percent) return kZoomSteps[i];
  }
  return kZoomSteps[kZoomCount - 1];
}

int zoom_prev(int percent) {
  for (int i = kZoomCount - 1; i >= 0; --i) {
    if (kZoomSteps[i] < percent) return kZoomSteps[i];
  }
  return kZoomSteps[0];
}

Rect apply_zoom(Rect fit, int win_w, int win_h, int zoom, long long max_pixels) {
  if (zoom == kZoomFit || zoom <= 0 || fit.w <= 0 || fit.h <= 0) return fit;
  long long w = static_cast<long long>(fit.w) * zoom / 100;
  long long h = static_cast<long long>(fit.h) * zoom / 100;
  if (w <= 0 || h <= 0) return fit;  // zoomed into nothing: leave the fit rect.
  if (max_pixels > 0 && w * h > max_pixels) {
    // Over budget: back off along the aspect until the area fits. Integer
    // halving would be too blunt, so scale by the exact ratio and floor.
    const double k = std::sqrt(static_cast<double>(max_pixels) /
                               static_cast<double>(w * h));
    w = static_cast<long long>(static_cast<double>(w) * k);
    h = static_cast<long long>(static_cast<double>(h) * k);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
  }
  Rect r;
  r.w = static_cast<int>(w);
  r.h = static_cast<int>(h);
  r.x = r.w <= win_w ? (win_w - r.w) / 2 : 0;  // centre what fits, pan the rest.
  r.y = r.h <= win_h ? (win_h - r.h) / 2 : 0;
  return r;
}

Rect fit_rect(int img_w, int img_h, int win_w, int win_h, Fit fit) {
  if (img_w <= 0 || img_h <= 0 || win_w <= 0 || win_h <= 0) return Rect{};
  switch (fit) {
    case Fit::Page: {
      const double sx = static_cast<double>(win_w) / img_w;
      const double sy = static_cast<double>(win_h) / img_h;
      const double s = sx < sy ? sx : sy;
      const int dw = round_pos(img_w * s);
      const int dh = round_pos(img_h * s);
      return Rect{(win_w - dw) / 2, (win_h - dh) / 2, dw, dh};
    }
    case Fit::Width: {
      const double s = static_cast<double>(win_w) / img_w;
      return Rect{0, 0, win_w, round_pos(img_h * s)};
    }
  }
  return Rect{};  // unreachable; the switch is exhaustive.
}

std::string report_line(int idx) {
  return "LAST_PAGE=" + std::to_string(idx + 1) + "\n";
}

std::string hud_text(const ViewState& s) {
  std::string t = std::to_string(s.page + 1) + "/" + std::to_string(s.page_count);
  if (s.zoom != kZoomFit) t += " " + std::to_string(s.zoom) + "%";
  return t;
}

namespace {

// 5x7 glyph bitmaps for the HUD (digits, then '/' and '%'): one byte per row,
// the low 5 bits are the columns, bit 4 = leftmost.
constexpr std::uint8_t kHudGlyphs[12][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},  // 0
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},  // 1
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},  // 2
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},  // 3
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},  // 4
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},  // 5
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},  // 6
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},  // 7
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},  // 8
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},  // 9
    {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10},  // '/'
    {0x19, 0x1A, 0x02, 0x04, 0x08, 0x13, 0x13},  // '%'
};

const std::uint8_t* hud_glyph(char c) {
  if (c >= '0' && c <= '9') return kHudGlyphs[c - '0'];
  if (c == '/') return kHudGlyphs[10];
  if (c == '%') return kHudGlyphs[11];
  return nullptr;  // blank cell (the space in "3/20 150%" included).
}

}  // namespace

HudImage render_hud(const std::string& text, int scale) {
  HudImage img;
  if (text.empty() || scale < 1) return img;
  const int cell_w = 6 * scale;  // 5 glyph columns + 1 spacing column.
  const int pad = 2 * scale;
  img.w = 2 * pad + cell_w * static_cast<int>(text.size()) - scale;
  img.h = 2 * pad + 7 * scale;
  img.rgba.assign(static_cast<std::size_t>(img.w) * img.h * 4u, 0);
  auto put = [&](int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b,
                 std::uint8_t a) {
    std::uint8_t* p =
        img.rgba.data() + (static_cast<std::size_t>(y) * img.w + x) * 4u;
    p[0] = r;
    p[1] = g;
    p[2] = b;
    p[3] = a;
  };
  for (int y = 0; y < img.h; ++y) {
    for (int x = 0; x < img.w; ++x) put(x, y, 0, 0, 0, 150);  // the box.
  }
  for (std::size_t ci = 0; ci < text.size(); ++ci) {
    const std::uint8_t* g = hud_glyph(text[ci]);
    if (g == nullptr) continue;
    const int gx = pad + static_cast<int>(ci) * cell_w;
    for (int row = 0; row < 7; ++row) {
      for (int col = 0; col < 5; ++col) {
        if ((g[row] & (1u << (4 - col))) == 0) continue;
        for (int sy = 0; sy < scale; ++sy) {
          for (int sx = 0; sx < scale; ++sx) {
            put(gx + col * scale + sx, pad + row * scale + sy, 255, 255, 255, 235);
          }
        }
      }
    }
  }
  return img;
}

}  // namespace shigoku::view
