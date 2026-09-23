// archive_names.cpp — see archive_names.hpp for the design. Pure string
// handling, no I/O, no libarchive.

#include "archive_names.hpp"

#include <unordered_map>
#include <utility>

#include "pager.hpp"

namespace shigoku::view {

std::optional<std::string> sanitize_entry_name(std::string_view raw) {
  for (unsigned char c : raw) {
    if (c < 0x20 || c == 0x7f) return std::nullopt;
  }

  std::string norm(raw);
  for (char& c : norm) {
    if (c == '\\') c = '/';
  }

  std::vector<std::string> stack;
  std::size_t start = 0;
  while (start <= norm.size()) {
    const std::size_t slash = norm.find('/', start);
    const std::size_t end = slash == std::string::npos ? norm.size() : slash;
    const std::string seg = norm.substr(start, end - start);
    if (seg.empty() || seg == ".") {
      // dropped.
    } else if (seg == "..") {
      if (!stack.empty()) stack.pop_back();
    } else {
      stack.push_back(seg);
    }
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  if (stack.empty()) return std::nullopt;

  std::string out;
  for (std::size_t i = 0; i < stack.size(); ++i) {
    if (i != 0) out += '_';
    out += stack[i];
  }
  return out;
}

std::string disambiguate(const std::string& name, int count) {
  if (count <= 1) return name;
  const std::string suffix = "~" + std::to_string(count);
  const std::size_t dot = name.find_last_of('.');
  if (dot == std::string::npos) return name + suffix;
  return name.substr(0, dot) + suffix + name.substr(dot);
}

std::vector<std::string> dedupe_names(std::vector<std::string> names) {
  std::unordered_map<std::string, int> counts;
  for (auto& n : names) {
    int& c = counts[n];
    ++c;
    n = disambiguate(n, c);
  }
  return names;
}

std::vector<std::string> select_image_entries(std::vector<std::string> names) {
  std::vector<std::string> out;
  out.reserve(names.size());
  for (auto& n : names) {
    if (is_image_ext(n)) out.push_back(std::move(n));
  }
  return out;
}

}  // namespace shigoku::view
