// scrape.cpp — see scrape.hpp.

#include "scrape.hpp"

#include <algorithm>
#include <cctype>

#include "domain.hpp"  // kMaxEpisodeHint.

namespace shigoku::scrape {

namespace {

// Named/numeric entity -> char. nullopt if unrecognized (caller keeps literal).
std::optional<char32_t> entity_char(std::string_view entity) {
  if (entity == "amp") return U'&';
  if (entity == "lt") return U'<';
  if (entity == "gt") return U'>';
  if (entity == "quot") return U'"';
  if (entity == "apos") return U'\'';
  if (entity == "nbsp") return U' ';
  if (entity.empty() || entity.front() != '#') return std::nullopt;
  std::string_view digits = entity.substr(1);
  if (digits.empty()) return std::nullopt;
  int base = 10;
  if (digits.front() == 'x' || digits.front() == 'X') {
    base = 16;
    digits = digits.substr(1);
    if (digits.empty()) return std::nullopt;
  }
  char32_t code = 0;
  for (const char c : digits) {
    int d;
    if (c >= '0' && c <= '9') {
      d = c - '0';
    } else if (base == 16 && c >= 'a' && c <= 'f') {
      d = c - 'a' + 10;
    } else if (base == 16 && c >= 'A' && c <= 'F') {
      d = c - 'A' + 10;
    } else {
      return std::nullopt;
    }
    code = code * static_cast<char32_t>(base) + static_cast<char32_t>(d);
    if (code > 0x10FFFF) return std::nullopt;
  }
  return code;
}

void push_utf8(std::string& out, char32_t c) {
  if (c < 0x80) {
    out.push_back(static_cast<char>(c));
  } else if (c < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (c >> 6)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  } else if (c < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (c >> 12)));
    out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (c >> 18)));
    out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  }
}

}  // namespace

std::optional<std::string> attr_value(std::string_view block, std::string_view name) {
  const std::string needle = std::string(name) + "=\"";
  const auto at = block.find(needle);
  if (at == std::string_view::npos) return std::nullopt;
  std::string_view rest = block.substr(at + needle.size());
  const auto end = rest.find('"');
  if (end == std::string_view::npos) return std::nullopt;
  return std::string(rest.substr(0, end));
}

std::string decode_entities(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  std::string_view rest = s;
  while (true) {
    const auto at = rest.find('&');
    if (at == std::string_view::npos) break;
    out.append(rest.substr(0, at));
    std::string_view tail = rest.substr(at);
    const auto semi = tail.find(';');
    if (semi != std::string_view::npos && semi <= 10) {
      auto c = entity_char(tail.substr(1, semi - 1));
      if (c) {
        push_utf8(out, *c);
      } else {
        out.append(tail.substr(0, semi + 1));
      }
      rest = tail.substr(semi + 1);
    } else {
      out.push_back('&');
      rest = tail.substr(1);
    }
  }
  out.append(rest);
  return out;
}

std::string_view trim(std::string_view s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string_view::npos) return {};
  const auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

std::string url_encode(std::string_view s) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (const unsigned char c : s) {
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                            c == '~';
    if (unreserved) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

std::string form_urlencode(std::string_view s) {
  // url::form_urlencoded::byte_serialize: unreserved (A-Za-z0-9 and *-._) pass;
  // space -> '+'; everything else -> %XX uppercase.
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (const unsigned char c : s) {
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '*' || c == '-' || c == '.' || c == '_';
    if (unreserved) {
      out.push_back(static_cast<char>(c));
    } else if (c == ' ') {
      out.push_back('+');
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

std::string to_lower(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return out;
}

std::optional<std::string> origin_of(std::string_view absolute_url) {
  const auto scheme_end = absolute_url.find("://");
  if (scheme_end == std::string_view::npos) return std::nullopt;
  const auto path_start = absolute_url.find_first_of("/?#", scheme_end + 3);
  const std::string_view origin = absolute_url.substr(0, path_start);
  if (origin.size() == scheme_end + 3) return std::nullopt;
  return std::string(origin);
}

std::vector<std::string> numbered_labels(std::uint32_t n) {
  const std::uint32_t count = std::clamp<std::uint32_t>(n, 1, kMaxEpisodeHint);
  std::vector<std::string> out;
  out.reserve(count);
  for (std::uint32_t i = 1; i <= count; ++i) out.push_back(std::to_string(i));
  return out;
}

}  // namespace shigoku::scrape
