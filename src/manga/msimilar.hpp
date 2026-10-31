// msimilar.hpp — the Similar feed (`~`): AniList's recommendations for one
// title, listed as rows no source has bound yet, and the binding that turns
// one into the active source's row on Enter. The lookup itself is the one
// AniList call here; everything else is pure and test-driven.

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../domain.hpp"
#include "../error.hpp"
#include "../http.hpp"
#include "../result.hpp"
#include "model.hpp"

namespace shigoku::manga {

// One page of recommendations for a seed. The seed is an AniList id when the
// row carried one, else its title, which the lookup resolves first; the
// answer names the id and title it settled on, so the next page skips the
// search.
struct MgSimilarPage {
  std::int64_t al_id = 0;
  std::string al_title;
  std::vector<Enrichment> rows;
  bool has_next = false;
};

using MgSimilarFn = std::function<Result<MgSimilarPage, ProviderError>(
    std::optional<std::int64_t> al_id, std::string_view title, std::uint32_t page)>;

// The real lookup: recommendations by id, or by the title AniList best
// matches when the row carried none. Ok with no rows = AniList knows no such
// title, or nothing recommended for it.
[[nodiscard]] Result<MgSimilarPage, ProviderError> anilist_similar(
    const http::Client& client, std::optional<std::int64_t> al_id, std::string_view title,
    std::uint32_t page);

// An AniList entry as an unbound row (model.hpp): no native id, the AniList
// cover and description, its other titles kept for binding.
[[nodiscard]] MdManga similar_row(const Enrichment& e);

// AniList's description as plain text: tags dropped, entities decoded.
[[nodiscard]] std::string strip_markup(std::string_view s);

// The source row an unbound row binds to: the hit sharing its AniList id,
// else one whose title equals one of its titles (case and punctuation
// aside); nullopt = none, never a guess.
[[nodiscard]] std::optional<MdManga> pick_binding(const std::vector<MdManga>& hits,
                                                  const MdManga& want);

}  // namespace shigoku::manga
