// character_page.cpp — see character_page.hpp.

#include "character_page.hpp"

#include <algorithm>
#include <optional>

#include "../domain.hpp"
#include "layout.hpp"
#include "theme.hpp"

namespace shigoku::tui {

namespace detail {

std::string spoiler_text(std::string_view text, bool reveal) {
  std::string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    if (text.compare(i, 2, "~!") != 0) {
      out.push_back(text[i]);
      ++i;
      continue;
    }
    const std::size_t end = text.find("!~", i + 2);
    if (reveal) {
      out.append(text.substr(i + 2, end == std::string_view::npos ? end : end - (i + 2)));
    } else {
      out.append(kSpoilerMark);
    }
    i = end == std::string_view::npos ? text.size() : end + 2;
  }
  return out;
}

bool has_spoilers(const CharacterDetail& data) {
  return !data.spoiler_names.empty() || data.description.find("~!") != std::string::npos;
}

namespace {

// 36091 -> "36,091".
std::string grouped(std::uint32_t n) {
  std::string digits = std::to_string(n);
  std::string out;
  for (std::size_t i = 0; i < digits.size(); ++i) {
    if (i != 0 && (digits.size() - i) % 3 == 0) out.push_back(',');
    out.push_back(digits[i]);
  }
  return out;
}

void push_blank(std::vector<CharacterLine>& lines) {
  if (!lines.empty() && lines.back().kind != CharacterLine::Kind::Blank) {
    lines.push_back(CharacterLine{});
  }
}

void push(std::vector<CharacterLine>& lines, CharacterLine::Kind kind, std::string text) {
  CharacterLine l;
  l.kind = kind;
  l.text = std::move(text);
  lines.push_back(std::move(l));
}

void push_fact(std::vector<CharacterLine>& lines, std::string_view label,
               const std::optional<std::string>& value) {
  if (!value.has_value() || value->empty()) return;
  CharacterLine l;
  l.kind = CharacterLine::Kind::Fact;
  l.label = std::string(label);
  l.text = *value;
  lines.push_back(std::move(l));
}

std::size_t max_scroll(const std::vector<CharacterLine>& lines, int rows) {
  const std::size_t visible = rows > 0 ? static_cast<std::size_t>(rows) : 0;
  return lines.size() > visible ? lines.size() - visible : 0;
}

std::size_t appearance_count(const std::vector<CharacterLine>& lines) {
  return static_cast<std::size_t>(
      std::count_if(lines.begin(), lines.end(), [](const CharacterLine& l) {
        return l.kind == CharacterLine::Kind::Appearance;
      }));
}

// Scroll so appearance `idx` (and, for the first one, its header) is on
// screen.
void show_appearance(CharacterPageState& page, const std::vector<CharacterLine>& lines,
                     int rows, std::size_t idx) {
  if (rows <= 0) return;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].kind != CharacterLine::Kind::Appearance || lines[i].idx != idx) continue;
    const std::size_t top = (idx == 0 && i > 0) ? i - 1 : i;
    if (top < page.scroll) page.scroll = top;
    const std::size_t visible = static_cast<std::size_t>(rows);
    if (i >= page.scroll + visible) page.scroll = i - visible + 1;
    return;
  }
}

}  // namespace

std::vector<CharacterLine> character_page_lines(const CharacterPageState& page, int width) {
  std::vector<CharacterLine> lines;
  if (width < 1) return lines;
  using K = CharacterLine::Kind;
  const CharacterDetail& d = page.data;

  push(lines, K::Name, d.name.empty() ? page.name : d.name);

  std::string names = d.native_name.value_or(std::string{});
  std::string alts;
  auto add_alts = [&](const std::vector<std::string>& more) {
    for (const std::string& n : more) {
      if (!alts.empty()) alts += ", ";
      alts += n;
    }
  };
  add_alts(d.alt_names);
  if (page.reveal_spoilers) add_alts(d.spoiler_names);
  if (!alts.empty()) {
    if (!names.empty()) names += " \xC2\xB7 ";  // ·
    names += alts;
  }
  for (std::string& l : wrap_text(names, width)) push(lines, K::Names, std::move(l));

  push_blank(lines);
  push_fact(lines, "Gender", d.gender);
  push_fact(lines, "Age", d.age);
  push_fact(lines, "Birthday", d.birthday);
  push_fact(lines, "Blood type", d.blood_type);
  if (d.favourites.has_value()) push_fact(lines, "Favourites", grouped(*d.favourites));
  push_fact(lines, "Voice", page.show_voice);

  if (page.loading) {
    push_blank(lines);
    push(lines, K::Note, "loading\xE2\x80\xA6");
  } else if (page.failed) {
    push_blank(lines);
    push(lines, K::Note, "can't reach the catalog");
  } else if (page.missing) {
    push_blank(lines);
    push(lines, K::Note, "the catalog has no page for this character");
  } else if (page.fetched && d.description.empty() && d.appearances.empty()) {
    push_blank(lines);
    push(lines, K::Note, "nothing more is listed");
  }

  if (!d.description.empty()) {
    push_blank(lines);
    const std::string text = spoiler_text(d.description, page.reveal_spoilers);
    std::size_t pos = 0;
    while (pos <= text.size()) {
      std::size_t nl = text.find('\n', pos);
      if (nl == std::string::npos) nl = text.size();
      const std::string_view para = std::string_view(text).substr(pos, nl - pos);
      if (para.find_first_not_of(' ') == std::string_view::npos) {
        push_blank(lines);
      } else {
        for (std::string& l : wrap_text(para, width)) push(lines, K::Text, std::move(l));
      }
      pos = nl + 1;
    }
  }

  if (!d.appearances.empty()) {
    push_blank(lines);
    push(lines, K::Header, "Appears in");
    for (std::size_t i = 0; i < d.appearances.size(); ++i) {
      CharacterLine l;
      l.kind = K::Appearance;
      l.idx = i;
      lines.push_back(std::move(l));
    }
  }

  while (!lines.empty() && lines.back().kind == K::Blank) lines.pop_back();
  return lines;
}

CharacterPageBox character_page_box(const App& app) {
  CharacterPageBox box;
  // The zoom's canvas, less a two-column gutter for the scroll marks.
  const int left = 2;
  const int right = app.win.cols - 2 - 2;
  box.x = left;
  if (const Rect portrait = cover_rect(app); !portrait.empty()) {
    box.x = portrait.x + portrait.w + kCharacterBodyGap;
  }
  box.w = right - box.x;
  box.y = kContentY0;
  box.rows = content_height(app.win.rows);
  if (box.w < 1 || box.rows < 1) {
    box.w = 0;
    box.rows = 0;
  }
  return box;
}

void character_page_clamp(CharacterPageState& page, const std::vector<CharacterLine>& lines,
                          int rows) {
  page.scroll = std::min(page.scroll, max_scroll(lines, rows));
  const std::size_t n = appearance_count(lines);
  if (page.selected.has_value() && *page.selected >= n) {
    page.selected = n > 0 ? std::optional<std::size_t>(n - 1) : std::nullopt;
  }
}

void character_page_step(CharacterPageState& page, const std::vector<CharacterLine>& lines,
                         int rows, int dir) {
  character_page_clamp(page, lines, rows);
  const std::size_t n = appearance_count(lines);
  if (dir > 0) {
    if (page.selected.has_value()) {
      if (*page.selected + 1 < n) ++*page.selected;
      show_appearance(page, lines, rows, *page.selected);
    } else if (page.scroll < max_scroll(lines, rows)) {
      ++page.scroll;
    } else if (n > 0) {
      page.selected = 0;
      show_appearance(page, lines, rows, 0);
    }
    return;
  }
  if (page.selected.has_value()) {
    if (*page.selected > 0) {
      --*page.selected;
      show_appearance(page, lines, rows, *page.selected);
    } else {
      page.selected = std::nullopt;
    }
  } else if (page.scroll > 0) {
    --page.scroll;
  }
}

void character_page_jump(CharacterPageState& page, const std::vector<CharacterLine>& lines,
                         int rows, bool top) {
  if (top) {
    page.selected = std::nullopt;
    page.scroll = 0;
    return;
  }
  page.scroll = max_scroll(lines, rows);
  const std::size_t n = appearance_count(lines);
  if (n > 0) {
    page.selected = n - 1;
    show_appearance(page, lines, rows, n - 1);
  }
}

}  // namespace detail

namespace {

constexpr int kFactLabelCols = 12;

std::string_view appearance_title(const Enrichment& e, TitleLanguage pref) {
  return preferred_title(e.title_romaji,
                         e.title_english ? std::optional<std::string_view>(*e.title_english)
                                         : std::nullopt,
                         e.title_native ? std::optional<std::string_view>(*e.title_native)
                                        : std::nullopt,
                         pref);
}

// "▸ title            MAIN · voice  [88]": the role and score always, the
// voice only where the row is wide enough to keep the title readable.
void draw_appearance(const App& app, CellBuffer& buf, int x0, int w, int y,
                     const CharacterAppearance& a, bool selected) {
  constexpr int kVoiceMinCols = 56;
  const Rgb bg = selected ? theme::surface : theme::bg;
  buf.fill(Rect{x0, y, w, 1}, bg);
  int x = x0;
  if (selected) {
    x = buf.put_str(x, y, "\xE2\x96\xB8 ", theme::focus, bg);  // ▸
  } else {
    x += 2;
  }
  std::string meta = a.role;
  if (w >= kVoiceMinCols && a.va_name.has_value() && !a.va_name->empty()) {
    if (!meta.empty()) meta += " \xC2\xB7 ";  // ·
    meta += *a.va_name;
  }
  if (!meta.empty()) meta += "  ";
  meta += a.media.score.has_value() ? ("[" + std::to_string(*a.media.score) + "]") : "[--]";
  meta = truncate_to_cols(meta, std::max(0, w / 2));
  const int meta_w = str_width(meta);
  const int title_max = (x0 + w - meta_w - 2) - x;
  const std::string title = truncate_to_cols(
      appearance_title(a.media, parse_title_language(app.config.title_language)),
      title_max > 0 ? title_max : 0);
  buf.put_str(x, y, title, selected ? theme::focus : theme::fg, bg,
              selected ? Style::Bold : Style::None);
  buf.put_str(x0 + w - meta_w, y, meta, theme::fg2, bg);
}

}  // namespace

void draw_character_page(const App& app, CellBuffer& buf, int y0, int y1) {
  const detail::CharacterPageBox box = detail::character_page_box(app);
  const int rows = std::min(box.rows, y1 - std::max(y0, box.y));
  if (rows <= 0 || box.w <= 0) return;
  const CharacterPageState& page = app.character;
  const std::vector<detail::CharacterLine> lines =
      detail::character_page_lines(page, box.w);
  const std::size_t visible = static_cast<std::size_t>(rows);
  const std::size_t top =
      std::min(page.scroll, lines.size() > visible ? lines.size() - visible : 0);

  using K = detail::CharacterLine::Kind;
  int y = box.y;
  for (std::size_t i = top; i < lines.size() && y < box.y + rows; ++i, ++y) {
    const detail::CharacterLine& l = lines[i];
    switch (l.kind) {
      case K::Blank:
        break;
      case K::Name:
        buf.put_str(box.x, y, truncate_to_cols(l.text, box.w), theme::fg, theme::bg,
                    Style::Bold);
        break;
      case K::Names:
        buf.put_str(box.x, y, l.text, theme::fg2, theme::bg);
        break;
      case K::Fact:
        buf.put_str(box.x, y, l.label, theme::fg3, theme::bg);
        buf.put_str(box.x + kFactLabelCols, y,
                    truncate_to_cols(l.text, std::max(0, box.w - kFactLabelCols)), theme::fg,
                    theme::bg);
        break;
      case K::Text:
        buf.put_str(box.x, y, l.text, theme::fg2, theme::bg);
        break;
      case K::Note:
        buf.put_str(box.x, y, truncate_to_cols(l.text, box.w),
                    page.failed ? theme::warn : theme::fg3, theme::bg, Style::Italic);
        break;
      case K::Header:
        buf.put_str(box.x, y, l.text, theme::fg2, theme::bg, Style::Bold);
        break;
      case K::Appearance:
        if (l.idx < page.data.appearances.size()) {
          draw_appearance(app, buf, box.x, box.w, y, page.data.appearances[l.idx],
                          page.selected.has_value() && *page.selected == l.idx);
        }
        break;
    }
  }

  // Scroll marks in the gutter: more above / more below.
  const int mark_x = box.x + box.w + 1;
  if (top > 0) buf.put_str(mark_x, box.y, "\xE2\x86\x91", theme::fg3, theme::bg);  // ↑
  if (top + visible < lines.size()) {
    buf.put_str(mark_x, box.y + rows - 1, "\xE2\x86\x93", theme::fg3, theme::bg);  // ↓
  }
}

}  // namespace shigoku::tui
