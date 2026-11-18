#include "util.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <utility>


namespace shigoku::manga::detail {

namespace {


// Leading decimal via strtod on a null-terminated copy (from_chars<double>
// isn't reliably available across this tree's legacy toolchains). "Consumed
// at least one digit" is endptr != the buffer start, not merely != nullptr.
struct LeadingNumber {
  bool has_value = false;
  double value = 0.0;
};

LeadingNumber parse_leading_number(std::string_view s) {
  const std::string buf(s);
  char* endptr = nullptr;
  const double v = std::strtod(buf.c_str(), &endptr);
  LeadingNumber out;
  if (endptr != buf.c_str()) {
    out.has_value = true;
    out.value = v;
  }
  return out;
}

// "&#39;" / "&#x27;" → codepoint. Nullopt on anything malformed or hostile
// (0, > U+10FFFF, overlong digit runs) — the caller passes the raw text
// through untouched instead.
std::optional<std::uint32_t> parse_numeric_entity(std::string_view ent) {
  std::string_view digits = ent.substr(1);  // past '#'.
  std::uint32_t base = 10;
  if (!digits.empty() && (digits.front() == 'x' || digits.front() == 'X')) {
    base = 16;
    digits.remove_prefix(1);
  }
  if (digits.empty() || digits.size() > 7) return std::nullopt;
  std::uint32_t v = 0;
  for (const char c : digits) {
    std::uint32_t d = 0;
    if (c >= '0' && c <= '9') {
      d = static_cast<std::uint32_t>(c - '0');
    } else if (base == 16 && c >= 'a' && c <= 'f') {
      d = static_cast<std::uint32_t>(c - 'a') + 10;
    } else if (base == 16 && c >= 'A' && c <= 'F') {
      d = static_cast<std::uint32_t>(c - 'A') + 10;
    } else {
      return std::nullopt;
    }
    v = v * base + d;
    if (v > 0x10FFFF) return std::nullopt;
  }
  if (v == 0) return std::nullopt;
  return v;
}

void append_utf8(std::string& out, std::uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

}  // namespace

std::string url_encode(std::string_view s) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (const unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '.' || c == '_' || c == '~' || c == '-') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

bool filename_safe(std::string_view s) {
  if (s.empty()) return false;
  if (s.front() == '.') return false;
  for (const char c : s) {
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          c == '.' || c == '_' || c == '-')) {
      return false;
    }
  }
  if (s.find("..") != std::string_view::npos) return false;
  return true;
}

bool chapter_less(std::string_view a, std::string_view b) {
  const LeadingNumber na = parse_leading_number(a);
  const LeadingNumber nb = parse_leading_number(b);
  if (na.has_value && nb.has_value) {
    if (na.value != nb.value) return na.value < nb.value;
    return a < b;  // tie -> lexicographic on the full string.
  }
  if (na.has_value != nb.has_value) {
    return na.has_value;  // numeric sorts before non-numeric.
  }
  return a < b;  // both non-numeric (incl. "") -> lexicographic.
}

bool chapter_before(const MdChapter& a, const MdChapter& b) {
  if (chapter_less(a.chapter, b.chapter)) return true;
  if (chapter_less(b.chapter, a.chapter)) return false;
  if (a.publish_at != b.publish_at) return a.publish_at < b.publish_at;
  return a.id < b.id;
}

std::vector<MdChapter> dedupe_chapters(std::vector<MdChapter> in) {
  // Group by exact chapter string, keeping the newest publish_at seen so far
  // (strict '>' so an equal-or-older duplicate never displaces the
  // first-seen incumbent — the tie rule).
  std::vector<MdChapter> kept;
  kept.reserve(in.size());
  for (auto& c : in) {
    auto it = std::find_if(kept.begin(), kept.end(),
                           [&](const MdChapter& k) { return k.chapter == c.chapter; });
    if (it == kept.end()) {
      kept.push_back(std::move(c));
    } else if (c.publish_at > it->publish_at) {
      *it = std::move(c);
    }
  }
  std::sort(kept.begin(), kept.end(), [](const MdChapter& x, const MdChapter& y) {
    if (chapter_less(x.chapter, y.chapter)) return true;
    if (chapter_less(y.chapter, x.chapter)) return false;
    return x.id < y.id;  // final tie-break for determinism.
  });
  return kept;
}

std::string html_unescape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  std::size_t i = 0;
  while (i < s.size()) {
    if (s[i] != '&') {
      out += s[i++];
      continue;
    }
    const auto semi = s.find(';', i);
    // No terminator nearby → a bare ampersand, not an entity.
    if (semi == std::string_view::npos || semi - i > 10) {
      out += s[i++];
      continue;
    }
    const std::string_view ent = s.substr(i + 1, semi - i - 1);
    if (ent == "amp") {
      out += '&';
    } else if (ent == "lt") {
      out += '<';
    } else if (ent == "gt") {
      out += '>';
    } else if (ent == "quot") {
      out += '"';
    } else if (ent == "apos") {
      out += '\'';
    } else if (!ent.empty() && ent.front() == '#') {
      const auto cp = parse_numeric_entity(ent);
      if (!cp.has_value()) {
        out += s[i++];
        continue;
      }
      append_utf8(out, *cp);
    } else {
      // Unknown named entity passes through verbatim.
      out += s[i++];
      continue;
    }
    i = semi + 1;
  }
  return out;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                        s.front() == '\n' || s.front() == '\r')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                        s.back() == '\n' || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  return s;
}

}  // namespace shigoku::manga::detail
