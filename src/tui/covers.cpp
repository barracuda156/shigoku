// covers.cpp — the cover pipeline + detail-cover state machine (P8/P26, 04
// §7.3, 05 §12). Ported from sabigoku src/tui/covers/{mod,cache,disk,detail}.rs.
//
// stb single-headers are the image/image-resize crate equivalents (A4/P0
// vendored them SYSTEM so their own warnings don't trip -Werror). This is the
// one TU that instantiates their implementations. sha256/sha256.h (disk cache
// filenames) is likewise a vendored SYSTEM single-header (03 §portability: no
// new external dep beyond curl/sqlite3 for the MacPorts target).
//
// ENDIANNESS (§3): stb produces byte-order RGBA; the crop is a byte copy;
// stbir resizes over bytes. No pixel is ever packed into a wider int.

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb/stb_image.h>
#include <stb/stb_image_resize2.h>
#include <sha256/sha256.h>

#include "covers.hpp"

#include <sys/stat.h>
#include <unistd.h>  // getpid — disk-cache temp-file uniqueness (POSIX, both targets).

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>

#include "../debug_log.hpp"
#include "../http.hpp"
#include "../provider.hpp"
#include "../webp_decode.hpp"

namespace shigoku::tui {

// --- decode + crop + resize -------------------------------------------------

Result<CoverPixels, CoverFetchError> decode_cover(
    const std::vector<unsigned char>& body) {
  if (body.empty() || body.size() > kMaxEncodedBytes) {
    return err(CoverFetchError::Decode);
  }
  // stb_image cannot read WebP (WeebCentral/Dynasty/nhentai serve
  // it), so a RIFF….WEBP body routes to libwebp instead. Same decode-bomb
  // dimension guard as the stb path below.
  if (shigoku::looks_like_webp(body.data(), body.size())) {
#ifdef HAVE_WEBP
    int ww = 0, wh = 0;
    if (!shigoku::webp_info(body.data(), body.size(), &ww, &wh) || ww <= 0 ||
        wh <= 0 || static_cast<std::uint32_t>(ww) > kMaxCoverDimension ||
        static_cast<std::uint32_t>(wh) > kMaxCoverDimension) {
      debug_log("cover decode: webp dim reject " + std::to_string(ww) + "x" +
                std::to_string(wh));
      return err(CoverFetchError::Decode);
    }
    int out_w = 0, out_h = 0;
    std::uint8_t* pixels =
        shigoku::decode_webp(body.data(), body.size(), &out_w, &out_h);
    if (pixels == nullptr || out_w <= 0 || out_h <= 0) {
      debug_log("cover decode: webp decode failed bytes=" +
                std::to_string(body.size()));
      if (pixels != nullptr) shigoku::free_webp_pixels(pixels);
      return err(CoverFetchError::Decode);
    }
    CoverPixels img;
    img.w = static_cast<std::uint32_t>(out_w);
    img.h = static_cast<std::uint32_t>(out_h);
    const std::size_t n = static_cast<std::size_t>(out_w) *
                          static_cast<std::size_t>(out_h) * 4u;
    img.rgba.assign(pixels, pixels + n);
    shigoku::free_webp_pixels(pixels);
    return img;
#else
    debug_log("cover decode: webp body but libwebp is unavailable (WITH_WEBP)");
    return err(CoverFetchError::Decode);
#endif
  }
  // Header probe FIRST (mod.rs decode_cover): reject a zero-dim or over-cap
  // image by its dimensions before allocating any pixels (decode-bomb guard).
  int w = 0, h = 0, comp = 0;
  if (stbi_info_from_memory(body.data(), static_cast<int>(body.size()), &w, &h,
                            &comp) == 0) {
    debug_log("cover decode: stbi_info failed (" +
              std::string(stbi_failure_reason() ? stbi_failure_reason() : "?") +
              ") bytes=" + std::to_string(body.size()));
    return err(CoverFetchError::Decode);
  }
  if (w <= 0 || h <= 0 || static_cast<std::uint32_t>(w) > kMaxCoverDimension ||
      static_cast<std::uint32_t>(h) > kMaxCoverDimension) {
    debug_log("cover decode: dim reject " + std::to_string(w) + "x" +
              std::to_string(h));
    return err(CoverFetchError::Decode);
  }
  int out_w = 0, out_h = 0, out_comp = 0;
  stbi_uc* pixels = stbi_load_from_memory(body.data(), static_cast<int>(body.size()),
                                          &out_w, &out_h, &out_comp,
                                          /*desired_channels=*/4);  // force RGBA8.
  if (pixels == nullptr || out_w <= 0 || out_h <= 0) {
    debug_log("cover decode: stbi_load failed (" +
              std::string(stbi_failure_reason() ? stbi_failure_reason() : "?") +
              ") bytes=" + std::to_string(body.size()));
    if (pixels != nullptr) stbi_image_free(pixels);
    return err(CoverFetchError::Decode);
  }
  CoverPixels img;
  img.w = static_cast<std::uint32_t>(out_w);
  img.h = static_cast<std::uint32_t>(out_h);
  const std::size_t n = static_cast<std::size_t>(out_w) *
                        static_cast<std::size_t>(out_h) * 4u;
  img.rgba.assign(pixels, pixels + n);
  stbi_image_free(pixels);
  return img;
}

std::optional<CoverPixels> crop_and_resize(const CoverPixels& src,
                                           std::uint32_t tw, std::uint32_t th) {
  // cover_crop (mod.rs): largest centered sub-rect of src whose aspect matches
  // tw:th at full source resolution. A zero dim underflows the centering
  // subtraction, so reject it (mod.rs cover_crop guard).
  const std::uint32_t sw = src.w, sh = src.h;
  if (tw == 0 || th == 0 || sw == 0 || sh == 0 ||
      src.rgba.size() < static_cast<std::size_t>(sw) * sh * 4u) {
    return std::nullopt;
  }
  const std::uint64_t sw64 = sw, sh64 = sh, tw64 = tw, th64 = th;
  std::uint32_t cw, ch;
  if (sw64 * th64 > tw64 * sh64) {
    // Source wider than the block: keep full height, crop the width.
    cw = static_cast<std::uint32_t>(std::max<std::uint64_t>((sh64 * tw64) / th64, 1));
    ch = sh;
  } else {
    // Source taller (or equal): keep full width, crop the height.
    cw = sw;
    ch = static_cast<std::uint32_t>(std::max<std::uint64_t>((sw64 * th64) / tw64, 1));
  }
  const std::uint32_t x0 = (sw - cw) / 2;
  const std::uint32_t y0 = (sh - ch) / 2;

  // Copy the crop window into a tight RGBA buffer (row-major), then resize it
  // to exactly tw×th. A byte copy per row — no pixel packing (§3).
  std::vector<unsigned char> crop(static_cast<std::size_t>(cw) * ch * 4u);
  for (std::uint32_t y = 0; y < ch; ++y) {
    const std::size_t src_off =
        (static_cast<std::size_t>(y0 + y) * sw + x0) * 4u;
    const std::size_t dst_off = static_cast<std::size_t>(y) * cw * 4u;
    std::copy_n(src.rgba.data() + src_off, static_cast<std::size_t>(cw) * 4u,
                crop.data() + dst_off);
  }

  CoverPixels out;
  out.w = tw;
  out.h = th;
  out.rgba.assign(static_cast<std::size_t>(tw) * th * 4u, 0);
  // sRGB-correct downscale, RGBA with non-premultiplied alpha (covers are
  // opaque posters; the layout is still RGBA for the emitter's f=32).
  unsigned char* r = stbir_resize_uint8_srgb(
      crop.data(), static_cast<int>(cw), static_cast<int>(ch),
      /*input_stride=*/0, out.rgba.data(), static_cast<int>(tw),
      static_cast<int>(th), /*output_stride=*/0, STBIR_RGBA);
  if (r == nullptr) {
    return std::nullopt;
  }
  return out;
}

// --- Url-keyed disk cover cache (disk.rs) -----------------------------------

namespace disk_cache {

namespace {

std::atomic<std::uint64_t> g_tmp_nonce{0};

// mkdir -p (paths.cpp has the same helper for a different dir set; the covers
// dir is only known here, not worth threading through paths.hpp for one call
// site). Best-effort: a real failure surfaces on the write open below.
void mkdir_p(const std::string& dir) {
  std::string prefix;
  std::size_t pos = 0;
  while (pos < dir.size()) {
    const std::size_t next = dir.find('/', pos + 1);
    prefix = (next == std::string::npos) ? dir : dir.substr(0, next);
    if (!prefix.empty()) ::mkdir(prefix.c_str(), 0755);
    if (next == std::string::npos) break;
    pos = next;
  }
}

}  // namespace

std::string cover_path(const std::string& dir, std::string_view url) {
  const std::string stem = sha256::hex_digest(url.data(), url.size());
  return dir + "/" + stem + ".jpg";  // extension is cosmetic; format-agnostic.
}

std::optional<std::vector<unsigned char>> read(const std::string& dir,
                                                std::string_view url) {
  std::ifstream f(cover_path(dir, url), std::ios::binary);
  if (!f.is_open()) return std::nullopt;
  std::vector<unsigned char> body(
      (std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (!f.good() && !f.eof()) return std::nullopt;
  // Past the admission bound reads as a miss, never a truncated body (disk.rs
  // read: MAX_ENCODED_BYTES + 1 take, then a size check).
  if (body.size() > kMaxEncodedBytes) return std::nullopt;
  return body;
}

void write(const std::string& dir, std::string_view url,
          const std::vector<unsigned char>& body) {
  mkdir_p(dir);
  const std::string path = cover_path(dir, url);
  // Per-writer unique temp path (pid + a process-wide nonce) then atomic
  // rename: a shared `.tmp` for the same url would tear the file under
  // concurrent writers (04 §7.3 CLONE rule, disk.rs write).
  const std::uint64_t nonce = g_tmp_nonce.fetch_add(1, std::memory_order_relaxed);
  const std::string tmp = path + "." + std::to_string(static_cast<long>(getpid())) +
                          "." + std::to_string(nonce) + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return;
    out.write(reinterpret_cast<const char*>(body.data()),
              static_cast<std::streamsize>(body.size()));
    if (!out.good()) {
      out.close();
      std::remove(tmp.c_str());
      return;
    }
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::remove(tmp.c_str());
  }
}

}  // namespace disk_cache

// --- the pipeline object ----------------------------------------------------

Result<CoverPixels, CoverFetchError> Covers::load(const StreamProvider* provider,
                                                  std::string_view url,
                                                  std::uint32_t box_w,
                                                  std::uint32_t box_h) {
  if (box_w == 0 || box_h == 0) {
    return err(CoverFetchError::BadRef);
  }
  const std::string key(url);

  // load_with (mod.rs): decoded-LRU (full pre-crop image) -> raw-LRU (decode +
  // warm decoded) -> disk (decode + warm both memory tiers) -> guarded network
  // (disk write + warm both tiers). Every hit still crops/resizes fresh, so a
  // box change never re-fetches or re-decodes.
  if (auto hit = decoded_lru_.get(key)) {
    auto sized = crop_and_resize(*hit, box_w, box_h);
    if (!sized.has_value()) return err(CoverFetchError::Decode);
    return *sized;
  }
  if (auto raw = raw_lru_.get(key)) {
    auto decoded = decode_cover(*raw);
    if (!decoded.has_value()) return err(decoded.error());
    decoded_lru_.insert(key, *decoded);
    auto sized = crop_and_resize(*decoded, box_w, box_h);
    if (!sized.has_value()) return err(CoverFetchError::Decode);
    return *sized;
  }
  if (!covers_dir_.empty()) {
    if (auto body = disk_cache::read(covers_dir_, key)) {
      // Corrupt or truncated on disk: fall through and refetch (disk.rs
      // load_with: a bad disk body must not wedge the pipeline).
      if (auto decoded = decode_cover(*body); decoded.has_value()) {
        raw_lru_.insert(key, *body);
        decoded_lru_.insert(key, *decoded);
        auto sized = crop_and_resize(*decoded, box_w, box_h);
        if (!sized.has_value()) return err(CoverFetchError::Decode);
        return *sized;
      }
    }
  }

  // Resolve the ref to an absolute request. nullptr provider = AniList /
  // absolute-url path (the v0 detail cover): no referer/UA, url verbatim.
  std::optional<std::string> referer, user_agent;
  std::string fetch_url(url);
  if (provider != nullptr) {
    auto req = provider->cover_request(url);
    if (!req.has_value()) {
      debug_log("cover load: bad ref (provider) url=" + std::string(url));
      return err(CoverFetchError::BadRef);
    }
    fetch_url = req->url;
    referer = req->referer;
    user_agent = req->user_agent;
  }

  // Guard the ABSOLUTE fetch url (A5 / mod.rs: guard_fetch_url before fetch;
  // the guard rejects on Err — never sanitize and proceed).
  if (!http::guard_fetch_url(fetch_url).has_value()) {
    debug_log("cover load: guard refused url=" + fetch_url);
    return err(CoverFetchError::BadRef);
  }

  auto body = fetch_(fetch_url, referer, user_agent);
  if (!body.has_value()) {
    return err(body.error());  // make_http_fetch already logged the cause.
  }
  auto decoded = decode_cover(*body);
  if (!decoded.has_value()) {
    return err(decoded.error());
  }
  auto sized = crop_and_resize(*decoded, box_w, box_h);
  if (!sized.has_value()) {
    debug_log("cover load: resize failed src=" + std::to_string(decoded->w) +
              "x" + std::to_string(decoded->h) + " box=" +
              std::to_string(box_w) + "x" + std::to_string(box_h));
    return err(CoverFetchError::Decode);
  }
  debug_log("cover load: OK src=" + std::to_string(decoded->w) + "x" +
            std::to_string(decoded->h) + " box=" + std::to_string(box_w) + "x" +
            std::to_string(box_h) + " url=" + fetch_url);
  // `key` (the stored ref, not fetch_url) stays the cache key at every layer —
  // provider-relative refs never collide with an absolute url (mod.rs: CDN
  // host rotation must not bust cache, ROD-267).
  if (!covers_dir_.empty()) disk_cache::write(covers_dir_, key, *body);
  raw_lru_.insert(key, *body);
  decoded_lru_.insert(key, *decoded);
  return *sized;
}

CoverFetchFn make_http_fetch(const http::Client& client) {
  return [&client](std::string_view url,
                   const std::optional<std::string>& referer,
                   const std::optional<std::string>& user_agent)
             -> Result<std::vector<unsigned char>, CoverFetchError> {
    http::Request req;
    req.method = http::Method::Get;
    req.url = std::string(url);
    if (user_agent.has_value()) req.user_agent = *user_agent;
    if (referer.has_value()) req.extra_headers.push_back({"Referer", *referer});
    req.accept = http::Accept::Any2xx;
    // The Client already refuses redirects (03 §6.7) and caps the body at 4MiB
    // (< kMaxEncodedBytes 8MiB), so an oversize body is a Decode/Network there.
    auto body = client.fetch(req);
    if (!body.has_value()) {
      debug_log("cover fetch: http fail kind=" +
                std::to_string(static_cast<int>(body.error().kind)) +
                " status=" + std::to_string(body.error().status) + " url=" +
                std::string(url));
      return err(CoverFetchError::Fetch);
    }
    if (body->size() > kMaxEncodedBytes) {
      debug_log("cover fetch: oversize bytes=" + std::to_string(body->size()));
      return err(CoverFetchError::Fetch);
    }
    // http::Client returns std::uint8_t; CoverPixels/decode want unsigned char
    // (same type on every target — uint8_t is unsigned char here).
    return std::vector<unsigned char>(body->begin(), body->end());
  };
}

// --- CoverState: the 05 §12 decision table (detail.rs) ----------------------

CoverAction CoverState::decide(std::optional<std::int64_t> target_id,
                               std::optional<std::string_view> target_url,
                               std::uint64_t now, std::uint64_t cooldown) const {
  if (!target_id.has_value()) {
    return CoverAction::None;
  }
  if (!target_url.has_value()) {
    // Target has no art: clear only if held state is another id's.
    if (for_id_.has_value() && *for_id_ != *target_id) {
      return CoverAction::Clear;
    }
    return CoverAction::None;
  }
  if (for_id_ == target_id && (loading_ || has_pixels_)) {
    return CoverAction::UpToDate;  // a live cover wins over a stale failure.
  }
  // Failure records survive navigation; only cooldown expiry, a url change, or
  // a successful fetch end the suppression.
  if (failed_.has_value() && failed_->id == *target_id &&
      failed_->url.has_value() && *failed_->url == *target_url &&
      (now - failed_->at) < cooldown) {
    return CoverAction::Suppress;
  }
  return CoverAction::Fetch;
}

void CoverState::begin_fetch(std::int64_t id, std::string_view url) {
  failed_.reset();
  clear();
  for_id_ = id;
  inflight_url_ = std::string(url);
  loading_ = true;
}

bool CoverState::on_done(std::int64_t for_id) {
  if (for_id_ != for_id) {
    return false;  // stale (wrong id): state untouched, pixels dropped.
  }
  loading_ = false;
  failed_.reset();
  inflight_url_.reset();
  has_pixels_ = true;
  return true;
}

bool CoverState::on_error(std::int64_t for_id, std::uint64_t now) {
  if (for_id_ != for_id) {
    return false;  // stale drop.
  }
  failed_ = Failure{for_id, std::move(inflight_url_), now};
  inflight_url_.reset();  // moved-from; make the intent explicit.
  clear();
  return true;
}

void CoverState::clear() {
  has_pixels_ = false;
  for_id_.reset();
  inflight_url_.reset();
  loading_ = false;
}

}  // namespace shigoku::tui
