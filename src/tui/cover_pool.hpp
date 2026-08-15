// cover_pool.hpp — the reusable url-keyed multi-cover coordinator + render
// store (P17b, DESIGN §3.8/§7; ported 1:1 from sabigoku
// src/tui/covers/discover.rs). Discover is the first client; nothing in this
// file names "discover" — Browse/History thumbnails can adopt it later
// without rework.
//
// Part A (CoverSlot / CoverPool) is a straight port: slot status machine,
// pump() picks which urls to fetch (single-flight, cooldown, cap-aware),
// evict_past_cap() sheds LRU slots that are neither visible nor in-flight.
// TickCount replaces Instant (04 §8 wall-clock-free discipline — the same
// swap CoverState already made in covers.hpp); the cooldown window is
// kCoverCooldownTicks (covers.hpp), the same 100-tick/10s constant.
//
// Part B (the render store) is shigoku-only: Rust delegates transmitted-pixel
// bookkeeping to ratatui-image's ProtocolPool, which has no shigoku
// equivalent (kitty.hpp emits raw APCs; nothing else owns image lifecycles).
// It holds the decoded pixels + a stable small Kitty image id per adopted
// url, so compose_grid_apc (app.cpp) can retransmit on scroll-back without
// refetching.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../event.hpp"  // CoverPixels
#include "toast.hpp"      // TickCount

namespace shigoku::tui {

// Slot pool cap, ~two large pages (discover.rs DISCOVER_COVER_CAP). Visible
// or in-flight slots are never evicted, so the pool can exceed this while the
// excess is on screen.
inline constexpr std::size_t kCoverPoolCap = 30;

enum class CoverSlotStatus {
  Idle,
  Loading,
  Ready,
  Failed,
};

struct CoverSlot {
  std::string url;
  CoverSlotStatus status = CoverSlotStatus::Idle;
  // Flag only; pixels live in the render store (Part B below) — a copy per
  // slot would sit outside the 04 §7.3 RAM rail.
  bool has_pixels = false;
  bool has_failed_at = false;
  TickCount failed_at = 0;
  // Last pump the url was in the window; eviction recency.
  std::uint64_t last_seen_frame = 0;
};

// Part A — the coordinator. Embed on App by value (04 §7.4).
class CoverPool {
 public:
  [[nodiscard]] const CoverSlot* get(std::string_view url) const;

  // Adopt a landed cover for `url`, wherever the grid moved meanwhile. The
  // caller installs the pixels in the render store; this is only the record.
  void adopt(std::string_view url);

  // Failure cooldown per url; the pump re-admits after kCoverCooldownTicks.
  void note_failure(std::string_view url, TickCount now);

  // Spawn-failure path: a slot the pump marked but no worker serves must not
  // wedge in Loading.
  void reset_loading(std::string_view url);

  void evict(std::string_view url);

  [[nodiscard]] std::size_t size() const { return slots_.size(); }
  [[nodiscard]] bool empty() const { return slots_.empty(); }

  // One pump pass (04 §7.4, 05 §7): stamp recency for the windowed urls,
  // evict LRU slots past the pool cap (never visible, never in-flight), then
  // pick fetches in window order, at most `cap - busy` so in-flight work
  // keeps its room; `busy` can exceed `cap` after a live decrease. Every
  // returned url is already marked Loading: the caller must spawn its worker
  // or call reset_loading.
  [[nodiscard]] std::vector<std::string> pump(const std::vector<std::string>& window,
                                              TickCount now, std::size_t cap,
                                              std::size_t busy);

 private:
  [[nodiscard]] std::optional<std::size_t> index_of(std::string_view url) const;
  CoverSlot& ensure_slot(std::string_view url);
  [[nodiscard]] bool needs_fetch(std::string_view url, TickCount now) const;
  void evict_past_cap(const std::vector<std::string>& window);

  std::vector<CoverSlot> slots_;
  std::uint64_t frame_ = 0;
};

// --- Part B: the render store (shigoku-only) --------------------------------

// Fixed id pool for grid covers (A4: a stable small id space, no quota
// creep). kDetailCoverImageId (app.hpp, = 1) stays reserved for the single
// detail cover.
inline constexpr std::uint32_t kGridImageIdBase = 100;

// Per-adopted-url pixels + the Kitty image id assigned to it. Pixels are
// RETAINED after transmit so scroll-back can retransmit without refetching —
// a deviation from Rust's "flag only" (bounded by kCoverPoolCap: 30 ×
// ~190 KB worst case ≈ 5.7 MB, logged against the 04 §7.3 RAM rail in
// NOTES.md).
struct CoverRenderEntry {
  CoverPixels pixels;
  std::uint32_t id = 0;
};

// The render store the CoverPool coordinator's adopted urls feed into.
// Id allocation is a fixed pool starting at kGridImageIdBase: an evicted
// url's id returns to the free list. retain() drops store entries whose
// CoverPool slot is gone (mirrors Rust's pool.retain, called after each
// pump so the two stay in sync).
class CoverRenderStore {
 public:
  // Install decoded pixels for `url`, assigning a fresh id from the free
  // list (or reusing the url's existing id if it already has one). Returns
  // the assigned id, or nullopt if the pool is exhausted (kCoverPoolCap
  // bounds the CoverPool, so this should not happen in practice).
  std::optional<std::uint32_t> install(std::string_view url, CoverPixels pixels);

  // The (pixels, id) for an adopted url, or nullptr if never installed.
  [[nodiscard]] const CoverRenderEntry* get(std::string_view url) const;

  // Drop the store entry for `url` (its id returns to the free list).
  void erase(std::string_view url);

  // Sync mirror of the CoverPool: drop every store entry whose url has no
  // corresponding slot in `pool` anymore.
  void retain(const CoverPool& pool);

  [[nodiscard]] std::size_t size() const { return entries_.size(); }

 private:
  [[nodiscard]] std::uint32_t alloc_id();
  void free_id(std::uint32_t id);

  std::vector<std::pair<std::string, CoverRenderEntry>> entries_;  // small; linear scan.
  std::vector<std::uint32_t> free_ids_;
  std::uint32_t next_id_ = kGridImageIdBase;
};

}  // namespace shigoku::tui
