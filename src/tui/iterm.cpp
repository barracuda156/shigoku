// iterm.cpp — OSC 1337 inline-image emitter (see iterm.hpp for the wire
// contract). This is the one TU that instantiates stb_image_write, the same
// single-header vendoring pattern as covers.cpp/stb_image.

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

#include "iterm.hpp"

#include <cctype>

#include "kitty.hpp"  // base64_encode — the RFC 4648 helper both emitters share.

namespace shigoku::tui::iterm {

namespace {

// stbi_write_png_to_func sink: append the encoded bytes to a vector.
void append_bytes(void* context, void* data, int size) {
  auto* out = static_cast<std::vector<unsigned char>*>(context);
  const auto* p = static_cast<const unsigned char*>(data);
  out->insert(out->end(), p, p + static_cast<std::size_t>(size));
}

}  // namespace

std::vector<unsigned char> encode_png(const std::vector<unsigned char>& rgba,
                                      std::uint32_t w, std::uint32_t h) {
  if (w == 0 || h == 0) return {};
  if (rgba.size() != static_cast<std::size_t>(w) * h * 4u) return {};
  std::vector<unsigned char> png;
  const int ok = stbi_write_png_to_func(
      &append_bytes, &png, static_cast<int>(w), static_cast<int>(h),
      /*comp=*/4, rgba.data(), /*stride_bytes=*/static_cast<int>(w) * 4);
  if (ok == 0) return {};
  return png;
}

std::string transmit(const std::vector<unsigned char>& rgba, std::uint32_t w,
                     std::uint32_t h, int cols, int rows) {
  const std::vector<unsigned char> png = encode_png(rgba, w, h);
  if (png.empty()) return {};
  std::string out;
  out += "\x1b]1337;File=inline=1;size=";
  out += std::to_string(png.size());
  out += ";width=";
  out += std::to_string(cols);
  out += ";height=";
  out += std::to_string(rows);
  out += ";preserveAspectRatio=0;doNotMoveCursor=1:";
  out += kitty::base64_encode(png);
  out += '\x07';  // BEL terminator — the documented OSC 1337 form.
  return out;
}

std::string query_xtversion() { return "\x1b[>0q"; }

std::string query_cell_size() { return "\x1b]1337;ReportCellSize\x07"; }

bool parse_cell_size_reply(std::string_view reply, double& w_px,
                           double& h_px) {
  // OSC 1337 ; ReportCellSize=<h>;<w>[;<scale>] ST — h/w in points, scale =
  // pixels per point (older iTerm2 omits it ⇒ 1.0). No lowercase 'c' in the
  // reply body, so it never trips the probe reader's DA1 terminator scan.
  constexpr std::string_view kPrefix = "\x1b]1337;ReportCellSize=";
  const std::size_t p = reply.find(kPrefix);
  if (p == std::string_view::npos) return false;
  std::size_t i = p + kPrefix.size();

  auto read_double = [&](double& out) -> bool {
    double v = 0.0;
    bool any = false;
    while (i < reply.size() && reply[i] >= '0' && reply[i] <= '9') {
      v = v * 10.0 + (reply[i] - '0');
      ++i;
      any = true;
    }
    if (i < reply.size() && reply[i] == '.') {
      ++i;
      double frac = 0.1;
      while (i < reply.size() && reply[i] >= '0' && reply[i] <= '9') {
        v += (reply[i] - '0') * frac;
        frac *= 0.1;
        ++i;
        any = true;
      }
    }
    out = v;
    return any;
  };

  double h = 0.0, w = 0.0, scale = 1.0;
  if (!read_double(h)) return false;
  if (i >= reply.size() || reply[i] != ';') return false;
  ++i;
  if (!read_double(w)) return false;
  if (i < reply.size() && reply[i] == ';') {
    ++i;
    if (!read_double(scale)) return false;
  }
  if (h <= 0.0 || w <= 0.0 || scale <= 0.0) return false;
  w_px = w * scale;
  h_px = h * scale;
  return true;
}

namespace {

// The name out of an XTVERSION reply (DCS > | <name...> ST), lowercased, up
// to the ST / a separator / end. Empty when no XTVERSION reply is present.
std::string xtversion_name(std::string_view reply) {
  constexpr std::string_view kPrefix = "\x1bP>|";
  const std::size_t p = reply.find(kPrefix);
  if (p == std::string_view::npos) return {};
  std::size_t i = p + kPrefix.size();
  std::string name;
  while (i < reply.size() && reply[i] != '\x1b' && reply[i] != ' ' &&
         reply[i] != '(' && reply[i] != '/') {
    name += static_cast<char>(
        std::tolower(static_cast<unsigned char>(reply[i])));
    ++i;
  }
  return name;
}

}  // namespace

bool probe_reply_is_iterm(std::string_view reply) {
  // The definitive signal: the terminal answered the protocol's own query.
  double w = 0.0, h = 0.0;
  if (parse_cell_size_reply(reply, w, h)) return true;

  // Terminals known to draw OSC 1337 inline images but which may answer
  // XTVERSION without ReportCellSize. WezTerm/Konsole also speak Kitty
  // graphics and normally win the kitty probe first — they are listed for
  // the builds where that support is disabled.
  const std::string name = xtversion_name(reply);
  return name == "iterm2" || name == "wezterm" || name == "mintty" ||
         name == "konsole";
}

bool probe_reply_is_iterm2(std::string_view reply) {
  // ReportCellSize is iTerm2's proprietary report — an answer IS iTerm2. The
  // XTVERSION name is the belt-and-suspenders second signal.
  double w = 0.0, h = 0.0;
  if (parse_cell_size_reply(reply, w, h)) return true;
  return xtversion_name(reply) == "iterm2";
}

}  // namespace shigoku::tui::iterm
