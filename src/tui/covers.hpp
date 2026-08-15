// covers.hpp — the cover pipeline + detail-cover state machine (P8/P26, 04
// §7.3, 05 §12; ported from sabigoku src/tui/covers/{mod,cache,disk,detail}.rs).
// Two pieces:
//
//   1. load_cover_pixels(): url → decoded-LRU hit → raw-LRU hit (decode+warm)
//      → disk hit (decode+warm both memory tiers) → guarded fetch → decode →
//      disk write → warm both tiers → center-crop-to-fill → RGBA8 resize to
//      the cover box. The decoded/raw LRUs are byte-bounded (cache.rs's
//      ByteLru, ~48MiB/~32MiB caps) and key on url alone — they hold the FULL
//      decoded image / encoded body, never the per-box crop, so a box change
//      (terminal resize) re-crops from the cached decode instead of
//      re-fetching. The worker calls this off the UI thread and posts
//      CoverDone/CoverError.
//
//   2. CoverState: the 05 §12 detail-cover decision table (pure). Keys on
//      anilist_id + url; a live cover wins over a stale failure record;
//      failures suppress the SAME id+url for 10s (a url change or a success
//      clears immediately). The app owns one CoverState; tick() drives it.
//
// A1: all fetch/decode/resize is on the worker thread; the state machine and
// the emitter run on the UI thread. A5: cover fetch is guarded (guard_fetch_url)
// and redirects are refused (a followed 3xx re-enters unguarded, 03 §6.7).
//
// ENDIANNESS (§3): the pipeline moves RGBA bytes; nothing packs a pixel into a
// wider int (stb produces byte-order RGBA; the resize is over bytes).

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../error.hpp"
#include "../event.hpp"   // CoverPixels
#include "../result.hpp"

namespace shigoku {
class StreamProvider;
namespace http {
class Client;
}  // namespace http
}  // namespace shigoku

namespace shigoku::tui {

// --- Pipeline constants (covers/mod.rs) ------------------------------------

// One encoded body admission bound (mod.rs MAX_ENCODED_BYTES). Enforced at
// fetch; an oversize body is a failure, never a partial decode.
inline constexpr std::size_t kMaxEncodedBytes = 8u * 1024u * 1024u;
// Decode footprint cap per side (mod.rs MAX_COVER_DIMENSION, the RAM rail):
// full RGBA reaches the terminal un-downscaled, so a decode bomb is rejected by
// its header dimensions before the pixels allocate.
inline constexpr std::uint32_t kMaxCoverDimension = 2560;
// Same id+url failure suppress window (mod.rs RETRY_COOLDOWN); a url change
// clears immediately. Measured in TickCount elsewhere; the state machine takes
// a monotonic tick and a window in ticks so it stays wall-clock-free (04 §8).
inline constexpr std::uint64_t kCoverCooldownTicks = 100;  // 10s at 100ms ticks.

// Byte-bounded dual-LRU caps (cache.rs RAW_CACHE_BYTES / DECODED_CACHE_BYTES;
// 04 §7.3 freeze caps, tune in M1 if measured).
inline constexpr std::size_t kRawCacheBytes = 32u * 1024u * 1024u;
inline constexpr std::size_t kDecodedCacheBytes = 48u * 1024u * 1024u;

// --- Cover fetch failure taxonomy (covers/mod.rs CoverError) ---------------
//
// A closed kind (A2 fence); the worker maps it to a CoverError event (cooldown
// path) — none of these reach the user as distinct copy in v0 (a failed cover
// is silent; the placeholder box renders).
enum class CoverFetchError {
  BadRef,  // ref unusable or blocked by the fetch guard; skip, never sanitize.
  Fetch,   // transport / status / oversize.
  Decode,  // undecodable, zero-dim, or over the dimension cap.
};

// --- The pipeline ----------------------------------------------------------

// Decode a fetched encoded body (PNG/JPEG/…) to RGBA8, rejecting a zero-dim or
// over-cap image by its header BEFORE allocating pixels (mod.rs decode_cover:
// decode-bomb guard). Ported to stb_image (header probe via stbi_info).
[[nodiscard]] Result<CoverPixels, CoverFetchError> decode_cover(
    const std::vector<unsigned char>& body);

// Center-crop `src` to the `tw`×`th` aspect at full source resolution, then
// resize to exactly tw×th RGBA8 (mod.rs cover_crop + the encode worker's
// scale-to-fill). tw/th are the cover box IN PIXELS (cell box × cell geometry).
// Returns nullopt on any zero dimension (mod.rs cover_crop guard).
[[nodiscard]] std::optional<CoverPixels> crop_and_resize(const CoverPixels& src,
                                                         std::uint32_t tw,
                                                         std::uint32_t th);

// Full pipeline: decoded-LRU hit → else guarded fetch → decode → crop+resize to
// the box → insert. `provider` resolves a stored ref to an absolute request
// (nullptr = AniList/absolute-url path, the v0 detail cover). `box_w/box_h` are
// the target box in PIXELS. The LRU + client are owned by the caller (the
// worker's Covers instance) so the pipeline stays testable with a fetch stub.
//
// `fetch` is the transport seam: production binds it to the http::Client; the
// tests inject a canned body (mirrors mod.rs load_with's `fetch` parameter).
using CoverFetchFn = std::function<Result<std::vector<unsigned char>, CoverFetchError>(
    std::string_view url, const std::optional<std::string>& referer,
    const std::optional<std::string>& user_agent)>;

// --- Byte-bounded LRU (covers/cache.rs ByteLru) -----------------------------
//
// url-keyed, get() promotes, insert() evicts oldest-stamp entries until the
// new value fits. An entry larger than the whole cap is declined outright (the
// caller keeps its own copy) — never a partial cache of an oversize value.
// `Sized` is a callable `const T& -> std::size_t` (byte_len equivalent).
template <typename T, typename Sized>
class ByteLru {
 public:
  ByteLru(std::size_t cap, Sized sized) : cap_(cap), sized_(std::move(sized)) {}

  // A hit promotes (recency writer, cache.rs get). Returns a copy — callers
  // never hold a cache-owned buffer (cache.rs's clone-under-lock rule).
  [[nodiscard]] std::optional<T> get(std::string_view url) {
    for (auto& e : entries_) {
      if (e.url == url) {
        e.stamp = ++stamp_;
        return e.value;
      }
    }
    return std::nullopt;
  }

  // Insert / replace; evicts the least-recently-used until the value fits.
  // Returns false (declined, nothing changed) when the value alone exceeds
  // the cap.
  bool insert(std::string_view url, T value) {
    const std::size_t bytes = sized_(value);
    if (bytes > cap_) return false;
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
      if (it->url == url) {
        total_ -= sized_(it->value);
        entries_.erase(it);
        break;
      }
    }
    while (total_ + bytes > cap_) {
      if (!evict_oldest()) break;  // nothing left to evict.
    }
    entries_.push_back(Entry{std::string(url), std::move(value), ++stamp_});
    total_ += bytes;
    return true;
  }

  [[nodiscard]] std::size_t total_bytes() const { return total_; }
  [[nodiscard]] std::size_t size() const { return entries_.size(); }

 private:
  struct Entry {
    std::string url;
    T value;
    std::uint64_t stamp = 0;
  };

  bool evict_oldest() {
    if (entries_.empty()) return false;
    auto oldest = std::min_element(
        entries_.begin(), entries_.end(),
        [](const Entry& a, const Entry& b) { return a.stamp < b.stamp; });
    total_ -= sized_(oldest->value);
    entries_.erase(oldest);
    return true;
  }

  std::size_t cap_;
  std::size_t total_ = 0;
  std::uint64_t stamp_ = 0;
  Sized sized_;
  std::vector<Entry> entries_;  // byte caps keep n in the dozens; linear scan.
};

[[nodiscard]] inline std::size_t byte_len(const std::vector<unsigned char>& v) {
  return v.size();
}
[[nodiscard]] inline std::size_t byte_len(const CoverPixels& p) {
  return p.rgba.size();
}

using RawLru = ByteLru<std::vector<unsigned char>, std::size_t (*)(const std::vector<unsigned char>&)>;
using DecodedLru = ByteLru<CoverPixels, std::size_t (*)(const CoverPixels&)>;

// --- Url-keyed disk cover cache (covers/disk.rs) ----------------------------
//
// Disk before network, so cold starts do not refetch the world. Best-effort
// everywhere: any failure is a miss and the pipeline refetches. Filenames are
// the full SHA-256 hex of the url (disk.rs's deliberate widen from zigoku's
// hex-16 — refs cross the provider trust boundary, and a truncated key gives
// a resourced attacker a birthday bound to alias another show's art).
namespace disk_cache {

[[nodiscard]] std::string cover_path(const std::string& dir, std::string_view url);
// Encoded body for `url`, or nullopt on any miss (missing file, open failure,
// or a body over kMaxEncodedBytes — read as a miss, never a truncated body).
[[nodiscard]] std::optional<std::vector<unsigned char>> read(const std::string& dir,
                                                             std::string_view url);
// Per-writer unique temp path then atomic rename (04 §7.3 CLONE rule): a
// shared `.tmp` for the same url would tear the file under concurrent
// writers. Best-effort; a failure here never surfaces past the caller.
void write(const std::string& dir, std::string_view url,
          const std::vector<unsigned char>& body);

}  // namespace disk_cache

// The pipeline object the cover worker owns (one per app). Holds both memory
// tiers; `covers_dir` (empty = memory-only, no disk tier — tests use this) is
// the disk-cache directory (paths.cache + "/covers"). The fetch seam is passed
// per call so production and tests share the code path.
class Covers {
 public:
  explicit Covers(CoverFetchFn fetch, std::string covers_dir = {})
      : fetch_(std::move(fetch)),
        covers_dir_(std::move(covers_dir)),
        raw_lru_(kRawCacheBytes, &byte_len),
        decoded_lru_(kDecodedCacheBytes, &byte_len) {}

  // url → RGBA cover cropped/resized to box_w×box_h pixels. The decoded/raw
  // LRUs (and the disk tier) key on url alone, holding the full pre-crop
  // decode; crop+resize happens on every call so a box change never refetches
  // or re-decodes.
  [[nodiscard]] Result<CoverPixels, CoverFetchError> load(
      const StreamProvider* provider, std::string_view url, std::uint32_t box_w,
      std::uint32_t box_h);

 private:
  CoverFetchFn fetch_;
  std::string covers_dir_;
  RawLru raw_lru_;
  DecodedLru decoded_lru_;
};

// Production fetch seam: an http::Client GET with the cover request's
// referer/UA, redirects refused, oversize → Fetch (mod.rs fetch_cover_body).
[[nodiscard]] CoverFetchFn make_http_fetch(const http::Client& client);

// --- CoverState: the 05 §12 detail-cover decision table (pure) -------------

enum class CoverAction {
  None,      // no target, or target has no art and held state is not foreign.
  Clear,     // held art belongs to a different id and the target has no art.
  Suppress,  // same-id same-url failure still inside the cooldown.
  UpToDate,  // already loading or holding pixels for this id.
  Fetch,     // fresh fetch; supersedes any failure record.
};

// Detail cover state machine (05 §12; detail.rs CoverState). Keys on
// anilist_id + url, never provider ids. The app owns one; tick() reconciles it
// against the selected row each frame and acts on the returned CoverAction.
// Time is a monotonic TickCount (04 §8 tick clock), not wall time.
class CoverState {
 public:
  [[nodiscard]] bool has_pixels() const { return has_pixels_; }
  [[nodiscard]] bool is_loading() const { return loading_; }
  [[nodiscard]] std::optional<std::int64_t> for_id() const { return for_id_; }

  // The 05 §12 decision table, pure. Order is law: a live cover wins over a
  // stale failure record, so UpToDate is decided before Suppress. `now` is the
  // current tick; `cooldown` the suppress window in ticks.
  [[nodiscard]] CoverAction decide(std::optional<std::int64_t> target_id,
                                   std::optional<std::string_view> target_url,
                                   std::uint64_t now,
                                   std::uint64_t cooldown = kCoverCooldownTicks) const;

  // The Fetch transition (caller spawns the worker). On spawn failure the
  // caller must clear() so no spinner strands.
  void begin_fetch(std::int64_t id, std::string_view url);
  // Worker success. False = stale (wrong id): state untouched, pixels dropped.
  bool on_done(std::int64_t for_id);
  // Worker failure. False = stale drop. Records the id+url cooldown at `now`,
  // then clears held art so a later sync can retry (05 §12: error clears).
  bool on_error(std::int64_t for_id, std::uint64_t now);
  // Drop held art + in-flight attribution; the failure record survives (it has
  // its own lifecycle, see decide()).
  void clear();

 private:
  struct Failure {
    std::int64_t id = 0;
    std::optional<std::string> url;
    std::uint64_t at = 0;
  };
  bool has_pixels_ = false;
  std::optional<std::int64_t> for_id_;
  bool loading_ = false;
  std::optional<Failure> failed_;
  std::optional<std::string> inflight_url_;
};

}  // namespace shigoku::tui
