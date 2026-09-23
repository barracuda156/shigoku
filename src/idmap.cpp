// idmap.cpp — binary search over the generated pair table (idmap.hpp).

#include "idmap.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace shigoku::idmap {

namespace {

// The reverse index (sorted by anilist_id), materialized on first use.
// Function-local static: initialized once, thread-safe under C++11 rules.
const std::vector<std::pair<std::uint32_t, std::uint32_t>>& by_anilist() {
  static const std::vector<std::pair<std::uint32_t, std::uint32_t>> index = [] {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> v;
    v.reserve(data::kCount);
    for (std::size_t i = 0; i < data::kCount; ++i) {
      v.emplace_back(data::kByMal[2 * i + 1], data::kByMal[2 * i]);
    }
    std::sort(v.begin(), v.end());
    return v;
  }();
  return index;
}

}  // namespace

std::optional<std::int64_t> to_anilist(std::int64_t mal_id) {
  if (mal_id <= 0 || mal_id > 0xFFFFFFFFLL) return std::nullopt;
  const auto key = static_cast<std::uint32_t>(mal_id);
  std::size_t lo = 0;
  std::size_t hi = data::kCount;
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    const std::uint32_t m = data::kByMal[2 * mid];
    if (m < key) {
      lo = mid + 1;
    } else if (m > key) {
      hi = mid;
    } else {
      return static_cast<std::int64_t>(data::kByMal[2 * mid + 1]);
    }
  }
  return std::nullopt;
}

std::optional<std::int64_t> to_mal(std::int64_t anilist_id) {
  if (anilist_id <= 0 || anilist_id > 0xFFFFFFFFLL) return std::nullopt;
  const auto key = static_cast<std::uint32_t>(anilist_id);
  const auto& idx = by_anilist();
  const auto it = std::lower_bound(
      idx.begin(), idx.end(), key,
      [](const std::pair<std::uint32_t, std::uint32_t>& p, std::uint32_t k) { return p.first < k; });
  if (it == idx.end() || it->first != key) return std::nullopt;
  return static_cast<std::int64_t>(it->second);
}

std::size_t size() { return data::kCount; }

const char* release() { return data::kRelease; }

std::int64_t anilist_or_synthetic(std::int64_t mal_id) {
  if (const auto real = to_anilist(mal_id); real.has_value()) return *real;
  return -mal_id;
}

}  // namespace shigoku::idmap
