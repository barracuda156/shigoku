// cover_pool.cpp — CoverPool coordinator + render store (P17b). Part A is a
// straight port of sabigoku's covers/discover.rs DiscoverCovers; Part B is
// shigoku-only bookkeeping with no Rust counterpart (file header comment).

#include "cover_pool.hpp"

#include <algorithm>

#include "covers.hpp"  // kCoverCooldownTicks

namespace shigoku::tui {

// --- Part A: CoverPool -------------------------------------------------

std::optional<std::size_t> CoverPool::index_of(std::string_view url) const {
  for (std::size_t i = 0; i < slots_.size(); ++i) {
    if (slots_[i].url == url) return i;
  }
  return std::nullopt;
}

const CoverSlot* CoverPool::get(std::string_view url) const {
  const auto i = index_of(url);
  return i.has_value() ? &slots_[*i] : nullptr;
}

CoverSlot& CoverPool::ensure_slot(std::string_view url) {
  if (const auto i = index_of(url); i.has_value()) return slots_[*i];
  slots_.push_back(CoverSlot{});
  slots_.back().url = std::string(url);
  return slots_.back();
}

void CoverPool::adopt(std::string_view url) {
  CoverSlot& slot = ensure_slot(url);
  slot.has_pixels = true;
  slot.status = CoverSlotStatus::Ready;
  slot.has_failed_at = false;
}

void CoverPool::note_failure(std::string_view url, TickCount now) {
  CoverSlot& slot = ensure_slot(url);
  slot.status = CoverSlotStatus::Failed;
  slot.has_failed_at = true;
  slot.failed_at = now;
}

void CoverPool::reset_loading(std::string_view url) {
  const auto i = index_of(url);
  if (i.has_value() && slots_[*i].status == CoverSlotStatus::Loading) {
    slots_[*i].status = CoverSlotStatus::Idle;
  }
}

void CoverPool::evict(std::string_view url) {
  const auto i = index_of(url);
  if (!i.has_value()) return;
  // swap_remove (order doesn't matter; the pool stays within a few dozen
  // slots, matching the Rust Vec::swap_remove).
  slots_[*i] = std::move(slots_.back());
  slots_.pop_back();
}

bool CoverPool::needs_fetch(std::string_view url, TickCount now) const {
  const CoverSlot* slot = get(url);
  if (slot == nullptr) return true;
  if (slot->has_pixels || slot->status == CoverSlotStatus::Loading) return false;
  if (!slot->has_failed_at) return true;
  return (now - slot->failed_at) >= kCoverCooldownTicks;
}

void CoverPool::evict_past_cap(const std::vector<std::string>& window) {
  if (slots_.size() <= kCoverPoolCap) return;
  const std::size_t over = slots_.size() - kCoverPoolCap;
  std::vector<std::pair<std::uint64_t, std::string>> candidates;
  for (const CoverSlot& s : slots_) {
    if (s.status == CoverSlotStatus::Loading) continue;
    if (std::find(window.begin(), window.end(), s.url) != window.end()) continue;
    candidates.emplace_back(s.last_seen_frame, s.url);
  }
  std::sort(candidates.begin(), candidates.end());
  const std::size_t take = std::min(over, candidates.size());
  for (std::size_t i = 0; i < take; ++i) evict(candidates[i].second);
}

std::vector<std::string> CoverPool::pump(const std::vector<std::string>& window,
                                         TickCount now, std::size_t cap,
                                         std::size_t busy) {
  ++frame_;
  const std::uint64_t frame = frame_;
  for (const std::string& url : window) {
    if (const auto i = index_of(url); i.has_value()) {
      slots_[*i].last_seen_frame = frame;
    }
  }
  evict_past_cap(window);
  if (busy >= cap) return {};
  std::size_t budget = cap - busy;
  std::vector<std::string> chosen;
  for (const std::string& url : window) {
    if (budget == 0) break;
    if (!needs_fetch(url, now)) continue;
    // Marking here makes a duplicate url in the window single-flight.
    ensure_slot(url).status = CoverSlotStatus::Loading;
    chosen.push_back(url);
    --budget;
  }
  return chosen;
}

// --- Part B: CoverRenderStore -------------------------------------------

std::uint32_t CoverRenderStore::alloc_id() {
  if (!free_ids_.empty()) {
    const std::uint32_t id = free_ids_.back();
    free_ids_.pop_back();
    return id;
  }
  return next_id_++;
}

void CoverRenderStore::free_id(std::uint32_t id) { free_ids_.push_back(id); }

std::optional<std::uint32_t> CoverRenderStore::install(std::string_view url,
                                                        CoverPixels pixels) {
  for (auto& [u, entry] : entries_) {
    if (u == url) {
      entry.pixels = std::move(pixels);
      return entry.id;
    }
  }
  const std::uint32_t id = alloc_id();
  entries_.emplace_back(std::string(url), CoverRenderEntry{std::move(pixels), id});
  return id;
}

const CoverRenderEntry* CoverRenderStore::get(std::string_view url) const {
  for (const auto& [u, entry] : entries_) {
    if (u == url) return &entry;
  }
  return nullptr;
}

void CoverRenderStore::erase(std::string_view url) {
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].first == url) {
      free_id(entries_[i].second.id);
      entries_[i] = std::move(entries_.back());
      entries_.pop_back();
      return;
    }
  }
}

void CoverRenderStore::retain(const CoverPool& pool) {
  for (std::size_t i = 0; i < entries_.size();) {
    if (pool.get(entries_[i].first) == nullptr) {
      free_id(entries_[i].second.id);
      entries_[i] = std::move(entries_.back());
      entries_.pop_back();
    } else {
      ++i;
    }
  }
}

}  // namespace shigoku::tui
