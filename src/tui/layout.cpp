// layout.cpp — see layout.hpp.

#include "layout.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

#include "cells.hpp"

namespace shigoku::tui {

PaneSplit pane_split(int w) {
  PaneSplit s;
  s.list_w = std::max(30, w * 38 / 100);
  s.detail_x = 2 + s.list_w + 2;
  s.detail_w = w - s.detail_x - 1;
  return s;
}

namespace detail {

std::vector<std::string> wrap_text(std::string_view text, int max_cols) {
  std::vector<std::string> out;
  if (max_cols <= 0) return out;
  std::string line;
  int line_w = 0;
  std::size_t i = 0;
  while (i < text.size()) {
    std::size_t j = text.find(' ', i);
    if (j == std::string_view::npos) j = text.size();
    std::string_view word = text.substr(i, j - i);
    i = j + 1;
    if (word.empty()) continue;  // space runs collapse.
    int ww = str_width(word);
    // A word wider than the box: flush the line, then hard-break the word on
    // codepoint boundaries (truncate_to_cols with no ellipsis = a raw prefix).
    while (ww > max_cols) {
      if (!line.empty()) {
        out.push_back(std::move(line));
        line.clear();
        line_w = 0;
      }
      const std::string head = truncate_to_cols(word, max_cols, "");
      out.push_back(head);
      word.remove_prefix(head.size());
      ww = str_width(word);
    }
    if (word.empty()) continue;
    if (line.empty()) {
      line.assign(word);
      line_w = ww;
    } else if (line_w + 1 + ww <= max_cols) {
      line += ' ';
      line.append(word);
      line_w += 1 + ww;
    } else {
      out.push_back(std::move(line));
      line.assign(word);
      line_w = ww;
    }
  }
  if (!line.empty()) out.push_back(std::move(line));
  return out;
}

}  // namespace detail

}  // namespace shigoku::tui
