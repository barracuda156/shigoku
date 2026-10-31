#include "msimilar.hpp"

#include <cctype>

#include "../anilist.hpp"

namespace shigoku::manga {

namespace {

// Letters and digits only, lower-cased: "[Oshi no Ko]" and "Oshi no Ko" are
// the same title. Non-ASCII bytes stay as they are.
std::string title_key(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (u >= 0x80) {
      out += c;
    } else if (std::isalnum(u)) {
      out += static_cast<char>(std::tolower(u));
    }
  }
  return out;
}

std::optional<std::string_view> view_of(const std::optional<std::string>& s) {
  if (!s.has_value()) return std::nullopt;
  return std::string_view(*s);
}

std::string display_title(const Enrichment& e) {
  return std::string(preferred_title(e.title_romaji, view_of(e.title_english),
                                     view_of(e.title_native), TitleLanguage::Romaji));
}

bool any_title_is(const Enrichment& e, const std::string& key) {
  if (title_key(e.title_romaji) == key) return true;
  if (e.title_english.has_value() && title_key(*e.title_english) == key) return true;
  return e.title_native.has_value() && title_key(*e.title_native) == key;
}

// AniList's media status in the sources' vocabulary (model.hpp status).
std::string status_text(std::string_view al_status) {
  if (al_status == "FINISHED") return "completed";
  if (al_status == "RELEASING") return "ongoing";
  if (al_status == "NOT_YET_RELEASED") return "upcoming";
  if (al_status == "CANCELLED") return "cancelled";
  if (al_status == "HIATUS") return "hiatus";
  std::string out(al_status);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

}  // namespace

Result<MgSimilarPage, ProviderError> anilist_similar(const http::Client& client,
                                                      std::optional<std::int64_t> al_id,
                                                      std::string_view title,
                                                      std::uint32_t page) {
  MgSimilarPage out;
  std::int64_t id = al_id.value_or(0);
  if (id <= 0) {
    if (title.empty()) return out;
    auto hits = anilist::search(client, title, 1, anilist::MediaKind::Manga);
    if (!hits.has_value()) return err(hits.error());
    const Enrichment* pick = nullptr;
    const std::string key = title_key(title);
    for (const Enrichment& e : hits->entries) {
      if (e.anilist_id > 0 && any_title_is(e, key)) {
        pick = &e;
        break;
      }
    }
    if (pick == nullptr) {
      for (const Enrichment& e : hits->entries) {
        if (e.anilist_id > 0) {
          pick = &e;
          break;
        }
      }
    }
    if (pick == nullptr) return out;
    id = pick->anilist_id;
    out.al_title = display_title(*pick);
  }
  out.al_id = id;
  auto recs = anilist::recommendations(client, id, page, anilist::MediaKind::Manga);
  if (!recs.has_value()) return err(recs.error());
  if (!recs->has_value()) return out;
  out.rows = std::move((*recs)->entries);
  out.has_next = (*recs)->has_next;
  return out;
}

std::string strip_markup(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size();) {
    const char c = s[i];
    if (c == '<') {
      const std::size_t close = s.find('>', i);
      if (close == std::string_view::npos) break;
      std::string tag = title_key(s.substr(i + 1, close - i - 1));
      if (tag == "br" || tag == "p") out += '\n';
      i = close + 1;
      continue;
    }
    if (c == '&') {
      const std::size_t semi = s.find(';', i);
      if (semi != std::string_view::npos && semi - i <= 8) {
        const std::string_view ent = s.substr(i + 1, semi - i - 1);
        const char* rep = nullptr;
        if (ent == "amp") rep = "&";
        else if (ent == "lt") rep = "<";
        else if (ent == "gt") rep = ">";
        else if (ent == "quot") rep = "\"";
        else if (ent == "#039" || ent == "#39" || ent == "apos") rep = "'";
        else if (ent == "nbsp") rep = " ";
        if (rep != nullptr) {
          out += rep;
          i = semi + 1;
          continue;
        }
      }
    }
    out += c;
    ++i;
  }
  // Runs of blank lines fold to one; edges are trimmed.
  std::string folded;
  folded.reserve(out.size());
  int newlines = 0;
  for (const char c : out) {
    if (c == '\n') {
      if (++newlines <= 1 && !folded.empty()) folded += '\n';
      continue;
    }
    if (c == '\r') continue;
    newlines = 0;
    folded += c;
  }
  while (!folded.empty() && (folded.back() == '\n' || folded.back() == ' ')) folded.pop_back();
  return folded;
}

MdManga similar_row(const Enrichment& e) {
  MdManga m;
  m.title = display_title(e);
  if (e.year.has_value()) m.year = static_cast<int>(*e.year);
  if (e.status.has_value()) m.status = status_text(*e.status);
  if (e.description.has_value()) m.description = strip_markup(*e.description);
  if (e.cover_url.has_value()) m.cover_url = *e.cover_url;
  if (e.anilist_id > 0) m.al_id = e.anilist_id;
  m.mal_id = e.mal_id;
  auto keep = [&](const std::string& t) {
    if (!t.empty() && t != m.title) m.alt_titles.push_back(t);
  };
  keep(e.title_romaji);
  if (e.title_english.has_value()) keep(*e.title_english);
  if (e.title_native.has_value()) keep(*e.title_native);
  return m;
}

std::optional<MdManga> pick_binding(const std::vector<MdManga>& hits, const MdManga& want) {
  if (want.al_id.has_value() && *want.al_id > 0) {
    for (const MdManga& h : hits) {
      if (h.al_id.has_value() && *h.al_id == *want.al_id) return h;
    }
  }
  std::vector<std::string> keys;
  if (const std::string k = title_key(want.title); !k.empty()) keys.push_back(k);
  for (const std::string& t : want.alt_titles) {
    if (const std::string k = title_key(t); !k.empty()) keys.push_back(k);
  }
  auto wanted = [&](std::string_view title) {
    const std::string k = title_key(title);
    if (k.empty()) return false;
    for (const std::string& w : keys) {
      if (w == k) return true;
    }
    return false;
  };
  for (const MdManga& h : hits) {
    if (wanted(h.title)) return h;
  }
  for (const MdManga& h : hits) {
    for (const std::string& t : h.alt_titles) {
      if (wanted(t)) return h;
    }
  }
  return std::nullopt;
}

}  // namespace shigoku::manga
