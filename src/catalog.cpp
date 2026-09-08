// catalog.cpp — the catalog dispatcher (catalog.hpp): the FailoverState
// latch, the two production backends, and the two-step try/fall-through.

#include "catalog.hpp"

#include <ctime>
#include <utility>

#include "anilist.hpp"
#include "idmap.hpp"
#include "mal_catalog.hpp"

namespace shigoku::catalog {

// ---------------------------------------------------------------------------
// Spellings
// ---------------------------------------------------------------------------

Mode parse_mode(std::string_view s) {
  if (s == "anilist") return Mode::AniList;
  if (s == "mal") return Mode::Mal;
  return Mode::Auto;
}

std::string_view mode_str(Mode m) {
  switch (m) {
    case Mode::Auto: return "auto";
    case Mode::AniList: return "anilist";
    case Mode::Mal: return "mal";
  }
  return "auto";  // unreachable (closed enum).
}

std::string_view source_name(Source s) {
  switch (s) {
    case Source::AniList: return "AniList";
    case Source::Mal: return "MAL";
  }
  return "AniList";  // unreachable (closed enum).
}

// ---------------------------------------------------------------------------
// FailoverState
// ---------------------------------------------------------------------------

Source FailoverState::first(std::int64_t now) const {
  switch (mode) {
    case Mode::AniList: return Source::AniList;
    case Mode::Mal: return Source::Mal;
    case Mode::Auto: break;
  }
  if (active == Source::Mal && mal_available && anilist_retry_at.has_value() &&
      now < *anilist_retry_at) {
    return Source::Mal;
  }
  return Source::AniList;
}

std::optional<Source> FailoverState::fallback_for(Source tried) const {
  if (mode != Mode::Auto) return std::nullopt;
  if (tried == Source::AniList) {
    return mal_available ? std::optional<Source>(Source::Mal) : std::nullopt;
  }
  return Source::AniList;
}

void FailoverState::on_result(Source s, bool ok, std::int64_t now) {
  if (s != Source::AniList) return;
  if (ok) {
    active = Source::AniList;
    anilist_retry_at.reset();
    return;
  }
  if (mode == Mode::Auto && mal_available) {
    active = Source::Mal;
    anilist_retry_at = now + kRetryAfterSecs;
  }
}

// ---------------------------------------------------------------------------
// Copy
// ---------------------------------------------------------------------------

std::string failure_copy(Source s, const ProviderError& e) {
  std::string out(source_name(s));
  out += ' ';
  out += provider_error_copy(e.kind);
  const bool carries_status = e.kind == ProviderError::Kind::Forbidden ||
                              e.kind == ProviderError::Kind::Server ||
                              e.kind == ProviderError::Kind::Http ||
                              e.kind == ProviderError::Kind::RateLimited;
  if (carries_status && e.status != 0) {
    out += " (" + std::to_string(e.status) + ")";
  }
  return out;
}

std::string describe(const ProviderError& e) {
  if (!e.detail.empty()) return e.detail;
  return std::string(provider_error_copy(e.kind));
}

namespace {

constexpr const char* kNoClientId = "MAL: no client id (set one in Settings)";

// The error handed back when neither source answered: the first source's
// kind (what the caller most likely cares about), the composed copy as detail.
ProviderError composed(Source first, const ProviderError& e1,
                       const std::optional<Source>& second, const ProviderError* e2,
                       bool mal_available) {
  ProviderError out = e1;
  std::string text = failure_copy(first, e1);
  if (second.has_value() && e2 != nullptr) {
    text += "; " + failure_copy(*second, *e2);
  } else if (first == Source::AniList && !second.has_value() && !mal_available) {
    text += "; ";
    text += kNoClientId;
  }
  out.detail = std::move(text);
  return out;
}

ProviderError no_mal_backend() {
  ProviderError e = ProviderError::forbidden(403);
  e.detail = kNoClientId;
  return e;
}

// The two-step dance shared by every operation. `call(source)` runs the
// backend; Unsupported from the fallback means "no answer here" and the first
// failure is what gets reported.
template <class T, class Call>
Result<T, ProviderError> run(Catalog& cat, Call&& call, bool mal_only) {
  if (mal_only) {
    if (cat.backend(Source::Mal) == nullptr) return err(no_mal_backend());
    auto r = call(Source::Mal);
    cat.report(Source::Mal, r.has_value());
    if (!r.has_value()) {
      ProviderError e = r.error();
      e.detail = failure_copy(Source::Mal, r.error());
      return err(std::move(e));
    }
    return r;
  }

  const Catalog::Plan plan = cat.plan();
  const bool mal_available = cat.backend(Source::Mal) != nullptr;

  // A missing backend (mode mal, no client id) is reported as such, not
  // dressed up as a MAL transport failure.
  if (cat.backend(plan.first) == nullptr) return err(no_mal_backend());
  Result<T, ProviderError> r1 = call(plan.first);
  cat.report(plan.first, r1.has_value());
  if (r1.has_value()) return r1;

  if (!plan.second.has_value()) {
    return err(composed(plan.first, r1.error(), std::nullopt, nullptr, mal_available));
  }
  Result<T, ProviderError> r2 = call(*plan.second);
  cat.report(*plan.second, r2.has_value());
  if (r2.has_value()) return r2;

  if (r2.error().kind == ProviderError::Kind::Unsupported) {
    // The fallback had nothing to say about this row: report the first
    // source's failure, which is the one that matters.
    return err(composed(plan.first, r1.error(), std::nullopt, nullptr, true));
  }
  return err(composed(plan.first, r1.error(), plan.second, &r2.error(), mal_available));
}

Page from_anilist(anilist::CatalogPage&& p) {
  Page out;
  out.entries = std::move(p.entries);
  out.has_next = p.has_next;
  out.source = Source::AniList;
  return out;
}

Page from_mal(mal_catalog::Page&& p) {
  Page out;
  out.entries = std::move(p.entries);
  out.has_next = p.has_next;
  out.source = Source::Mal;
  return out;
}

// The MAL id for a row: the caller's hint, the synthetic id's own payload, or
// the offline table.
std::optional<std::int64_t> mal_id_for(std::int64_t anilist_id, std::optional<std::int64_t> hint) {
  if (hint.has_value() && *hint > 0) return hint;
  if (anilist_id < 0) return -anilist_id;
  return idmap::to_mal(anilist_id);
}

}  // namespace

// ---------------------------------------------------------------------------
// Backends
// ---------------------------------------------------------------------------

Backend anilist_backend(const http::Client& client) {
  Backend b;
  b.search = [&client](std::string_view query, std::uint32_t page) -> Result<Page, ProviderError> {
    auto p = anilist::search(client, query, page);
    if (!p.has_value()) return err(p.error());
    return from_anilist(std::move(*p));
  };
  b.discover = [&client](DiscoverAxis axis, std::uint32_t page,
                         const DiscoverFilters& filters) -> Result<Page, ProviderError> {
    auto p = anilist::discover(client, axis, page, filters);
    if (!p.has_value()) return err(p.error());
    return from_anilist(std::move(*p));
  };
  b.enrich = [&client](std::int64_t anilist_id, std::optional<std::int64_t>)
      -> Result<std::optional<Enrichment>, ProviderError> {
    if (anilist_id <= 0) return err(ProviderError::unsupported());
    return anilist::enrich(client, anilist_id);
  };
  b.char_recs = [&client](std::int64_t anilist_id, std::optional<std::int64_t>)
      -> Result<std::optional<CharactersAndRecommendations>, ProviderError> {
    if (anilist_id <= 0) return err(ProviderError::unsupported());
    return anilist::characters_and_recommendations(client, anilist_id);
  };
  b.genres = [&client]() { return anilist::genre_collection(client); };
  return b;
}

Backend mal_backend(const http::Client& client, std::string client_id) {
  Backend b;
  b.search = [&client, client_id](std::string_view query,
                                  std::uint32_t page) -> Result<Page, ProviderError> {
    auto p = mal_catalog::search(client, client_id, query, page);
    if (!p.has_value()) return err(p.error());
    return from_mal(std::move(*p));
  };
  b.discover = [&client, client_id](DiscoverAxis axis, std::uint32_t page,
                                    const DiscoverFilters& filters) -> Result<Page, ProviderError> {
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    auto p = mal_catalog::discover(client, client_id, axis, page, now, filters);
    if (!p.has_value()) return err(p.error());
    return from_mal(std::move(*p));
  };
  b.enrich = [&client, client_id](std::int64_t anilist_id, std::optional<std::int64_t> hint)
      -> Result<std::optional<Enrichment>, ProviderError> {
    const auto mal_id = mal_id_for(anilist_id, hint);
    if (!mal_id.has_value()) return err(ProviderError::unsupported());
    auto d = mal_catalog::by_id(client, client_id, *mal_id, anilist_id);
    if (!d.has_value()) return err(d.error());
    if (!d->has_value()) return std::optional<Enrichment>{};
    return std::optional<Enrichment>(std::move((*d)->show));
  };
  b.char_recs = [&client, client_id](std::int64_t anilist_id, std::optional<std::int64_t> hint)
      -> Result<std::optional<CharactersAndRecommendations>, ProviderError> {
    const auto mal_id = mal_id_for(anilist_id, hint);
    if (!mal_id.has_value()) return err(ProviderError::unsupported());
    auto d = mal_catalog::by_id(client, client_id, *mal_id, anilist_id);
    if (!d.has_value()) return err(d.error());
    if (!d->has_value()) return std::optional<CharactersAndRecommendations>{};
    CharactersAndRecommendations out;
    out.recommendations = std::move((*d)->recommendations);
    return std::optional<CharactersAndRecommendations>(std::move(out));
  };
  b.genres = []() -> Result<std::vector<std::string>, ProviderError> {
    return mal_catalog::genre_vocabulary();
  };
  return b;
}

// ---------------------------------------------------------------------------
// Catalog
// ---------------------------------------------------------------------------

Catalog::Catalog(Mode mode, Backend anilist, std::optional<Backend> mal, Clock clock)
    : anilist_(std::move(anilist)), mal_(std::move(mal)), clock_(std::move(clock)) {
  state_.mode = mode;
  state_.mal_available = mal_.has_value();
  if (!clock_) clock_ = []() { return static_cast<std::int64_t>(std::time(nullptr)); };
}

Catalog::Plan Catalog::plan() {
  const std::int64_t now = clock_();
  std::lock_guard<std::mutex> lock(mu_);
  const Source first = state_.first(now);
  return Plan{first, state_.fallback_for(first)};
}

void Catalog::report(Source s, bool ok) {
  const std::int64_t now = clock_();
  std::lock_guard<std::mutex> lock(mu_);
  state_.on_result(s, ok, now);
}

const Backend* Catalog::backend(Source s) const {
  if (s == Source::AniList) return &anilist_;
  return mal_.has_value() ? &*mal_ : nullptr;
}

Status Catalog::status() const {
  std::lock_guard<std::mutex> lock(mu_);
  Status st;
  st.mode = state_.mode;
  st.active = state_.active;
  st.mal_available = state_.mal_available;
  return st;
}

Result<Page, ProviderError> Catalog::search(std::string_view query, std::uint32_t page) {
  return run<Page>(
      *this, [&](Source s) { return backend(s)->search(query, page); }, /*mal_only=*/false);
}

Result<Page, ProviderError> Catalog::discover(DiscoverAxis axis, std::uint32_t page,
                                              const DiscoverFilters& filters) {
  return run<Page>(
      *this, [&](Source s) { return backend(s)->discover(axis, page, filters); },
      /*mal_only=*/false);
}

Result<std::optional<Enrichment>, ProviderError> Catalog::enrich(
    std::int64_t anilist_id, std::optional<std::int64_t> mal_id) {
  return run<std::optional<Enrichment>>(
      *this, [&](Source s) { return backend(s)->enrich(anilist_id, mal_id); },
      /*mal_only=*/anilist_id < 0);
}

Result<std::optional<CharactersAndRecommendations>, ProviderError> Catalog::char_recs(
    std::int64_t anilist_id, std::optional<std::int64_t> mal_id) {
  return run<std::optional<CharactersAndRecommendations>>(
      *this, [&](Source s) { return backend(s)->char_recs(anilist_id, mal_id); },
      /*mal_only=*/anilist_id < 0);
}

Result<std::vector<std::string>, ProviderError> Catalog::genres() {
  return run<std::vector<std::string>>(
      *this, [&](Source s) { return backend(s)->genres(); }, /*mal_only=*/false);
}

}  // namespace shigoku::catalog
