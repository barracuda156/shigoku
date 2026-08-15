// discover_feed.cpp — DiscoverState::on_feed only (P17a). Split out of
// discover.cpp because it is the ONE method that touches the store
// (upsert_catalog_cache / enrichment_ttl_secs); keeping it here lets the pure
// state + render code (discover.cpp) live in the store-free render core
// (shigoku_tui_render), while this TU links the sqlite graph like app.cpp.

#include <iterator>

#include "../store.hpp"
#include "discover.hpp"

namespace shigoku::tui {

void DiscoverState::on_feed(DiscoverAxis axis, std::uint32_t page,
                            std::vector<CatalogRow> entries, bool has_next,
                            Store* store, std::int64_t now_unix, std::uint32_t gen) {
  // Filter-generation gate FIRST (P38 review): an answer spawned under the
  // old filters must be discarded whole — the positional check below cannot
  // tell it from the new set's own page, and `loading` now belongs to the
  // new generation's fetch.
  if (gen != filter_gen_) return;
  AxisSlot& s = slots_[axis_index(axis)];
  s.loading = false;
  if (page != s.page + 1) return;  // out-of-order / duplicate: discard.
  if (store != nullptr) {
    for (const CatalogRow& row : entries) {
      const std::int64_t ttl = enrichment_ttl_secs(
          row.meta.status.has_value() ? std::optional<std::string_view>(*row.meta.status)
                                       : std::nullopt);
      // best-effort: a cache write failure never drops a rendered feed.
      (void)store->upsert_catalog_cache(row.meta, now_unix, now_unix + ttl);
    }
  }
  s.entries.insert(s.entries.end(), std::make_move_iterator(entries.begin()),
                   std::make_move_iterator(entries.end()));
  s.page = page;
  s.failed.reset();
  s.exhausted = !has_next || s.entries.size() >= kMaxFeedRows;
}

}  // namespace shigoku::tui
