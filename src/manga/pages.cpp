// pages.cpp — chapter fetch-to-cache. See pages.hpp for the laws
// (offline-first, .part/rename atomicity, node rotation, URL guard).

#include "pages.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace shigoku::manga {

namespace detail {

std::string page_filename(std::size_t index, std::string_view at_home_name) {
  std::string ext = "jpg";
  const std::size_t dot = at_home_name.rfind('.');
  if (dot != std::string_view::npos && dot + 1 < at_home_name.size()) {
    ext = std::string(at_home_name.substr(dot + 1));
  }
  char num[16];
  std::snprintf(num, sizeof(num), "%03zu", index + 1);
  return std::string(num) + "." + ext;
}

Result<Unit, ProviderError> mkdir_p(const std::string& dir) {
  // Walk the components, creating as we go (the download ensure_parent_dirs
  // shape, without pulling the download lib into the manga link line).
  std::size_t i = 0;
  while (i < dir.size()) {
    std::size_t j = dir.find('/', i);
    if (j == std::string::npos) j = dir.size();
    const std::string acc = dir.substr(0, j);
    if (!acc.empty() && acc != "/") {
      if (::mkdir(acc.c_str(), 0755) != 0 && errno != EEXIST) {
        return err(ProviderError::decode(std::string("mkdir: ") +
                                         std::strerror(errno)));
      }
    }
    i = j + 1;
  }
  return Unit{};
}

}  // namespace detail

namespace {

bool file_exists(const std::string& path) {
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// The chapter id's uniqueness-bearing part, for the library dir's [id8]
// anchor. MangaDex uuids and WeebCentral ULIDs are flat and opaque, so this
// is just the id — but Dynasty's are HIERARCHICAL: every chapter of
// kase_san is "kase_san_ch01", "kase_san_rival_and_kase_san", … and a raw
// first-8 would be the literal string "kase_san" for the whole series, so
// every oneshot-numbered extra would collide into ONE directory (mixed pages
// under one .complete marker). Dropping the manga-id prefix (plus its
// separator) leaves "ch01" / "rival_an" — distinct again. Ids that don't
// carry the manga id as a prefix are untouched: md/wc dirs are byte-identical
// to before this rule, so nothing already on disk is orphaned.
std::string_view chapter_anchor(const MdManga& manga, const MdChapter& chapter) {
  std::string_view id(chapter.id);
  const std::string_view mid(manga.id);
  if (mid.empty() || id.size() <= mid.size() || id.compare(0, mid.size(), mid) != 0) {
    return id;
  }
  std::string_view tail = id.substr(mid.size());
  while (!tail.empty() && (tail.front() == '_' || tail.front() == '-')) {
    tail.remove_prefix(1);
  }
  return tail.empty() ? id : tail;
}

// Stream one page unit to <dir>/<name> via <name>.part + fsync + rename.
// Failure unlinks the .part (a partial PAGE is useless, unlike the episode
// download's .part, which resumes a partial transfer — re-entry re-fetches
// the whole page).
Result<Unit, ProviderError> fetch_page(const http::Client& client,
                                       const PageUnit& unit,
                                       const std::string& dir,
                                       const std::string& name) {
  const std::string final_path = dir + "/" + name;
  const std::string part = final_path + ".part";
  const int fd = ::open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return err(ProviderError::decode(std::string("open: ") +
                                     std::strerror(errno)));
  }
  bool write_failed = false;
  http::Request req;
  req.url = unit.url;
  req.user_agent = kUserAgent;
  if (!unit.referer.empty()) {
    req.extra_headers.push_back({"Referer", unit.referer});
  }
  auto r = client.fetch_to_sink(
      req, [&](const http::StreamMeta&, const std::uint8_t* p, std::size_t n) {
        std::size_t off = 0;
        while (off < n) {
          const ssize_t w = ::write(fd, p + off, n - off);
          if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            write_failed = true;
            return false;  // abort the transfer.
          }
          off += static_cast<std::size_t>(w);
        }
        return true;
      });
  if (!r.has_value() || write_failed) {
    ::close(fd);
    ::unlink(part.c_str());
    if (write_failed) {
      return err(ProviderError::decode("disk write failed"));
    }
    return err(r.error());
  }
  // fsync before rename: the final name must never point at a page the
  // kernel hasn't durably written.
  ::fsync(fd);
  ::close(fd);
  if (::rename(part.c_str(), final_path.c_str()) != 0) {
    const std::string why = std::strerror(errno);
    ::unlink(part.c_str());
    return err(ProviderError::decode("rename: " + why));
  }
  return Unit{};
}

// The rotation taxonomy (pages.hpp): true = this failure asks for a retry
// (1s backoff) before rotating; false = rotate immediately.
bool retry_before_rotate(const ProviderError& e) {
  switch (e.kind) {
    case ProviderError::Kind::Network:
    case ProviderError::Kind::Server:
    case ProviderError::Kind::RateLimited:
      return true;
    case ProviderError::Kind::Http:
      return e.status == 408;  // other 4xx (404 included) rotate now.
    case ProviderError::Kind::Forbidden:
    case ProviderError::Kind::Decode:
    case ProviderError::Kind::Unsupported:
      return false;
  }
  return false;  // unreachable; the switch is exhaustive.
}

// A failure kind rotation can help at all: transport/status trouble is the
// node's fault; Decode (disk, malformed, guard refusal) is not.
bool rotatable(const ProviderError& e) {
  return e.kind != ProviderError::Kind::Decode &&
         e.kind != ProviderError::Kind::Unsupported;
}

}  // namespace

std::string chapter_dir(std::string_view root, std::string_view manga_id,
                        std::string_view chapter_id) {
  std::string d(root);
  d += "/";
  d += manga_id;
  d += "/";
  d += chapter_id;
  return d;
}

bool chapter_complete(const std::string& dir) {
  return file_exists(dir + "/.complete");
}

std::string fs_name(std::string_view title) {
  std::string out;
  out.reserve(title.size());
  bool pending_space = false;
  for (const char ch : title) {
    const unsigned char u = static_cast<unsigned char>(ch);
    if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
      pending_space = true;
      continue;
    }
    if (pending_space && !out.empty()) out.push_back(' ');
    pending_space = false;
    if (u < 0x20 || ch == '/' || ch == '\\' || ch == ':' || u == 0x7F) {
      out.push_back('_');
    } else {
      out.push_back(ch);
    }
  }
  constexpr std::size_t kMaxBytes = 100;
  if (out.size() > kMaxBytes) {
    std::size_t cut = kMaxBytes;
    // Never cut inside a UTF-8 sequence: back off over continuation bytes.
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
      --cut;
    }
    out.resize(cut);
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
  if (!out.empty() && (out.front() == '.' || out.front() == '-')) {
    out.insert(out.begin(), '_');
  }
  if (out.empty()) out = "untitled";
  return out;
}

std::string pad_chapter(std::string_view chapter) {
  std::size_t digits = 0;
  while (digits < chapter.size() && chapter[digits] >= '0' &&
         chapter[digits] <= '9') {
    ++digits;
  }
  std::string out;
  for (std::size_t i = digits; i < 4; ++i) out.push_back('0');
  if (digits == 0) out.clear();  // non-numeric: pass through unpadded.
  out.append(chapter);
  return out;
}

std::string library_chapter_dir(std::string_view root, const MdManga& manga,
                                const MdChapter& chapter) {
  const std::string mid8 = manga.id.substr(0, 8);
  const std::string cid8(chapter_anchor(manga, chapter).substr(0, 8));
  std::string mseg = fs_name(manga.title);
  mseg += " [";
  mseg += mid8.empty() ? "?" : mid8;
  mseg += "]";
  std::string cseg = chapter.chapter.empty()
                         ? std::string("oneshot")
                         : "ch " + pad_chapter(chapter.chapter);
  cseg += " [";
  if (!chapter.lang.empty()) {
    cseg += chapter.lang;
    cseg += " ";
  }
  cseg += cid8.empty() ? "?" : cid8;
  cseg += "]";
  // The chapter number and lang are API strings too — one fs_name pass keeps
  // the segment path-safe (the title went through it above).
  return std::string(root) + "/" + mseg + "/" + fs_name(cseg);
}

std::vector<std::string> list_pages(const std::string& dir) {
  std::vector<std::string> out;
  DIR* d = ::opendir(dir.c_str());
  if (d == nullptr) return out;
  while (dirent* e = ::readdir(d)) {
    const std::string name = e->d_name;
    const std::size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) continue;  // marker/dotfiles out.
    std::string ext = name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    // The viewer's is_image_ext set (pager.cpp), minus nothing: .webp counts,
    // since Dynasty and nhentai chapters land nothing else, and a chapter
    // whose pages are all filtered out here reads as "no pages on disk" no
    // matter how well the fetch went. Everything else (.part, the .complete
    // marker, .report) stays out.
    if (ext != "jpg" && ext != "jpeg" && ext != "png" && ext != "webp") continue;
    out.push_back(dir + "/" + name);
  }
  ::closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

Result<std::uint32_t, ProviderError> fetch_chapter_pages(
    const http::Client& client, const PageSet& pages, const std::string& dir,
    const PagesProgressFn& progress, const PagesRefetchFn& refetch) {
  PageSet cur = pages;
  const std::uint32_t total = static_cast<std::uint32_t>(cur.units.size());
  if (total == 0) {
    return err(ProviderError::decode("empty page list"));
  }

  if (auto mk = detail::mkdir_p(dir); !mk.has_value()) return err(mk.error());

  if (chapter_complete(dir)) {
    if (progress) progress(total, total);
    return total;
  }

  bool rotated = false;
  for (std::uint32_t i = 0; i < total; ++i) {
    if (i >= cur.units.size()) {
      // A rotated set with fewer pages than the first one: refuse rather
      // than write a hole into the chapter.
      return err(ProviderError::decode("rotated grant page-count mismatch"));
    }
    const std::string name = detail::page_filename(i, cur.units[i].name_hint);
    if (file_exists(dir + "/" + name)) {
      if (progress) progress(i + 1, total);
      continue;  // re-entry skips completed pages.
    }

    // One page under the rotation policy. No guard here (the header's
    // contract: the orchestrator guarded every unit's url).
    auto attempt = [&]() -> Result<Unit, ProviderError> {
      return fetch_page(client, cur.units[i], dir, name);
    };

    auto r = attempt();
    if (!r.has_value() && retry_before_rotate(r.error())) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      r = attempt();
    }
    if (!r.has_value() && rotatable(r.error())) {
      if (rotated || !refetch) return err(r.error());  // second node: give up.
      rotated = true;
      auto fresh = refetch();
      if (!fresh.has_value()) return err(fresh.error());
      cur = std::move(*fresh);
      if (i >= cur.units.size()) {
        return err(ProviderError::decode("rotated grant page-count mismatch"));
      }
      r = attempt();
      if (!r.has_value() && retry_before_rotate(r.error())) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        r = attempt();
      }
    }
    if (!r.has_value()) return err(r.error());
    if (progress) progress(i + 1, total);
  }

  // The marker: plain open/close is enough for an empty flag file.
  const std::string marker = dir + "/.complete";
  const int fd = ::open(marker.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return err(ProviderError::decode(std::string("marker: ") +
                                     std::strerror(errno)));
  }
  ::close(fd);
  return total;
}

}  // namespace shigoku::manga
