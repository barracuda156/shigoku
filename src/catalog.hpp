// catalog.hpp — the catalog dispatcher: AniList first, MyAnimeList when
// AniList cannot answer. The five AppDeps catalog closures
// (search / discover / enrich / char_recs / genre_collection) and nothing
// else call through here, so the rest of the app keeps seeing one catalog.
//
// Two layers, split for testability:
//   - FailoverState: the pure latch (which source to try first, whether to
//     fall through, and how an outcome moves the latch). Time is a plain
//     integer argument.
//   - Catalog: two Backends (AniList, optional MAL) + the state under a
//     mutex, because the closures run on detached worker threads.
//
// Id law: a negative anilist_id is a synthetic MAL-only row and
// is always served by MAL, whatever the mode; a positive id goes to AniList
// first and to MAL by the row's mal_id hint (or the offline id map) when
// AniList fails. The answer for an existing row always keeps that row's id.

#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "domain.hpp"
#include "error.hpp"
#include "http.hpp"
#include "result.hpp"

namespace shigoku::catalog {

enum class Source { AniList, Mal };
enum class Mode { Auto, AniList, Mal };

// Config::catalog spelling <-> Mode. Unknown spellings read as Auto.
[[nodiscard]] Mode parse_mode(std::string_view s);
[[nodiscard]] std::string_view mode_str(Mode m);
[[nodiscard]] std::string_view source_name(Source s);  // "AniList" / "MAL"

struct Page {
  std::vector<Enrichment> entries;
  bool has_next = false;
  Source source = Source::AniList;  // who answered.
  friend bool operator==(const Page&, const Page&) = default;
};

// One catalog source's operations. enrich/char_recs take the row's own
// anilist_id plus the MAL id the caller already knows (nullopt = unknown);
// a backend that cannot serve the pair answers Err(Unsupported), which the
// dispatcher treats as "no answer from this source", never as a failure to
// report.
struct Backend {
  std::function<Result<Page, ProviderError>(std::string_view query, std::uint32_t page)> search;
  std::function<Result<Page, ProviderError>(DiscoverAxis axis, std::uint32_t page,
                                            const DiscoverFilters& filters)>
      discover;
  std::function<Result<std::optional<Enrichment>, ProviderError>(
      std::int64_t anilist_id, std::optional<std::int64_t> mal_id)>
      enrich;
  std::function<Result<std::optional<CharactersAndRecommendations>, ProviderError>(
      std::int64_t anilist_id, std::optional<std::int64_t> mal_id)>
      char_recs;
  std::function<Result<std::vector<std::string>, ProviderError>()> genres;
};

// The production backends. AniList refuses negative (synthetic) ids as
// Unsupported. MAL answers characters as an empty list (no MAL endpoint) and
// recommendations from the by-id call; its genre list is the static
// vocabulary.
[[nodiscard]] Backend anilist_backend(const http::Client& client);
[[nodiscard]] Backend mal_backend(const http::Client& client, std::string client_id);

// The pure latch.
struct FailoverState {
  Mode mode = Mode::Auto;
  bool mal_available = false;  // a MAL backend exists (client id known).
  Source active = Source::AniList;
  // Set while latched on MAL: AniList is probed again once now >= this.
  std::optional<std::int64_t> anilist_retry_at;
  static constexpr std::int64_t kRetryAfterSecs = 600;

  // Which source a call tries first, now.
  [[nodiscard]] Source first(std::int64_t now) const;
  // Where a failure on `tried` falls through to, if anywhere.
  [[nodiscard]] std::optional<Source> fallback_for(Source tried) const;
  // Record an outcome: an AniList success clears the latch; an AniList
  // failure (auto mode, MAL available) latches MAL for kRetryAfterSecs. MAL
  // outcomes never move the latch on their own.
  void on_result(Source s, bool ok, std::int64_t now);

  friend bool operator==(const FailoverState&, const FailoverState&) = default;
};

struct Status {
  Mode mode = Mode::Auto;
  Source active = Source::AniList;
  bool mal_available = false;
  // True when the next browse call would go to MAL (mode Mal, or auto while
  // latched) — what the top-bar chip shows.
  [[nodiscard]] bool serving_from_mal() const {
    return mode == Mode::Mal || (mode == Mode::Auto && active == Source::Mal);
  }
};

class Catalog {
 public:
  using Clock = std::function<std::int64_t()>;

  // `mal` nullopt = no client id: MAL is never tried (mode Mal then fails
  // every call with the Settings hint). A null clock uses wall time.
  Catalog(Mode mode, Backend anilist, std::optional<Backend> mal, Clock clock = {});

  [[nodiscard]] Result<Page, ProviderError> search(std::string_view query, std::uint32_t page);
  [[nodiscard]] Result<Page, ProviderError> discover(DiscoverAxis axis, std::uint32_t page,
                                                     const DiscoverFilters& filters);
  [[nodiscard]] Result<std::optional<Enrichment>, ProviderError> enrich(
      std::int64_t anilist_id, std::optional<std::int64_t> mal_id);
  [[nodiscard]] Result<std::optional<CharactersAndRecommendations>, ProviderError> char_recs(
      std::int64_t anilist_id, std::optional<std::int64_t> mal_id);
  [[nodiscard]] Result<std::vector<std::string>, ProviderError> genres();

  [[nodiscard]] Status status() const;

  // Exposed for the dispatcher tests: the two-step decision and its report.
  struct Plan {
    Source first;
    std::optional<Source> second;
  };
  [[nodiscard]] Plan plan();
  void report(Source s, bool ok);
  [[nodiscard]] const Backend* backend(Source s) const;

 private:
  mutable std::mutex mu_;
  FailoverState state_;
  Backend anilist_;
  std::optional<Backend> mal_;
  Clock clock_;
};

// Human copy for one source's failure: "AniList blocked us (403)",
// "MAL is down (503)", "AniList network unreachable".
[[nodiscard]] std::string failure_copy(Source s, const ProviderError& e);

// The copy the UI renders for a catalog error: the dispatcher's composed
// detail when present ("AniList blocked us (403); MAL: no client id"), else
// the bare kind copy.
[[nodiscard]] std::string describe(const ProviderError& e);

}  // namespace shigoku::catalog
