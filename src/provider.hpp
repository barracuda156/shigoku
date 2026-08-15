// provider.hpp — StreamProvider ABC + tier vocabulary + ProviderRegistry (P1, A8;
// registry grown to the full N=4 roster over P12a/P13/P25).
//
// Ported from sabigoku src/providers.rs:114 (the trait) and the tier semantics
// in 03 §4. This is THE seam every deferred feature plugs into (A8): the ABC
// surface and the full tier/origin enums compile in v0 even where only
// AKey/CSearch are ever constructed, so -Werror=switch forces every future
// call-site through the fence design instead of bolting it on later.
//
// App code talks only to this ABC; no concrete site type escapes the registry
// (03 §1). Providers depend on domain + http only, NEVER tui or store (01 §5).

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "domain.hpp"
#include "error.hpp"
#include "result.hpp"

namespace shigoku {

// Full page for provider tier-C pagination (03 §2.1, ROD-201).
inline constexpr std::uint32_t kSearchPageSize = 26;
// Cover ref ceiling for cover_request guards.
inline constexpr std::size_t kMaxCoverRefLen = 2048;

// --- Binding tiers (03 §4). The classifier vocabulary. --------------------
//
// v0 only ever constructs AKey and CSearch (PORT_CPP.md A8). Bound and
// BIdMatch compile in so the classifier can grow to the full walk without a
// type change, and every switch over Tier is forced to consider them.
enum class Tier {
  Bound,     // 0: a stored provider_binding row exists (deferred: no store walk in v0)
  AKey,      // A: canonical_key(show) yields an id  <-- v0 primary path
  BIdMatch,  // B: MAL/AniList id agreement inside tier-C results (deferred)
  CSearch,   // C: title/catalog search + scorer     <-- v0 fallback path
};

// Walk origin (03 §5.3): the reason a single-provider walk was armed. The
// K-2 fence distinguishes these — forced_preferred continues to a full walk on
// miss; pin_flip stops and keeps the pin. v0 arms neither (no pins/routes), but
// the enum compiles so the fence is a build error to ignore later, not a bolt-on.
enum class WalkOrigin {
  ForcedPreferred,  // §5.3 stale-stamp re-route; on miss -> K-2 full walk
  PinFlip,          // Path 3 / `v`; on miss -> stop, keep pin, no borrow
  Fallback,         // §6.4 ordinary failure-driven hop
};

// --- Related types (providers.rs) -----------------------------------------

struct SearchOptions {
  Translation translation = Translation::Sub;
  std::uint32_t limit = kSearchPageSize;
  std::uint32_t page = 1;  // 1-indexed.
};

// Tier-C candidate. Ids/titles/counts feed the tier-B/C scorers (03 §4.2);
// they are provider CLAIMS, not truth.
struct SearchHit {
  std::string provider_id;
  std::string title;
  std::optional<std::string> title_english;
  std::optional<std::string> title_native;
  std::optional<std::int64_t> anilist_id;
  std::optional<std::int64_t> mal_id;
  std::optional<std::uint32_t> total_episodes;  // nullopt -> per-track counts.
  std::uint32_t eps_sub = 0;
  std::uint32_t eps_dub = 0;
  std::optional<std::uint32_t> year;
  friend bool operator==(const SearchHit&, const SearchHit&) = default;
};

// Stored cover ref turned into an absolute fetch (03 §2).
struct CoverRequest {
  std::string url;
  std::optional<std::string> referer;
  std::optional<std::string> user_agent;
  friend bool operator==(const CoverRequest&, const CoverRequest&) = default;
};

// --- Shared provider guards (providers.rs) --------------------------------

// Digit-only id guard shared by MAL-keyed providers (senshi, megaplay).
// Rejects empty, path-smuggle ("../7"), mixed ("12a") before the id splices
// into a URL. Err(Decode) on reject, matching the Rust.
[[nodiscard]] Result<Unit, ProviderError> guard_show_id(std::string_view show_id);

// Printable-ASCII-only argv guard (0x21..=0x7e): rejects controls, spaces, and
// empty before a value reaches mpv's command line or an HTTP header splice.
[[nodiscard]] bool clean_arg(std::string_view s);

// --- The ABC (providers.rs:114) -------------------------------------------
//
// Enum-dispatch was fine in Rust while N<=3-4 (03 O2); we use a virtual ABC
// because C++ virtual dispatch is the idiomatic seam and the registry holds
// owned providers. The surface is 1:1 with the trait.
class StreamProvider {
 public:
  virtual ~StreamProvider() = default;

  // Stable persistence key ("senshi"). Renaming orphans every stored binding.
  // Returns a static string (§7: Arc<str> -> static constexpr const char*).
  [[nodiscard]] virtual std::string_view name() const = 0;

  // User-facing (toasts, UI). Free to change.
  [[nodiscard]] virtual std::string_view display_name() const = 0;

  // Tier A: pure derivation from AniList/MAL metadata (e.g. stringified
  // mal_id). nullopt = "does not id-key on canonical", NOT "not stocked".
  [[nodiscard]] virtual std::optional<std::string> canonical_key(
      const Enrichment& show) const = 0;

  // Tier-C binding search ONLY, never Browse (03 §1).
  [[nodiscard]] virtual Result<std::vector<SearchHit>, ProviderError> search(
      std::string_view query, const SearchOptions& opts) const = 0;

  // Whether search can EVER answer (structural capability, not "failed this
  // time"). Defaults true (tier-C search is the norm); a provider that cannot
  // search MUST override, or the CLI binds it and dies at runtime (03 §3.2).
  [[nodiscard]] virtual bool supports_search() const { return true; }

  // Sorted labels. Ok(empty) = authoritative NOT STOCKED (mark absence);
  // cannot-answer MUST be an error (03 §4.3). count_hint mints a 1..N grid on
  // listing-less providers; real listings ignore it.
  [[nodiscard]] virtual Result<std::vector<std::string>, ProviderError> episodes(
      std::string_view provider_id, Translation translation,
      std::optional<std::uint32_t> count_hint) const = 0;

  // Quality may be accepted-and-ignored when the provider has no variants (A8:
  // v0 always hands mpv the master playlist — keep the parameter, don't cap).
  [[nodiscard]] virtual Result<StreamLink, ProviderError> resolve(
      std::string_view provider_id, std::string_view episode,
      Translation translation, Quality quality) const = 0;

  // Err = ref unusable (empty, oversize, header-injection bytes); callers skip
  // the fetch, never "sanitize" and proceed.
  [[nodiscard]] virtual Result<CoverRequest, ProviderError> cover_request(
      std::string_view cover_ref) const = 0;
};

// --- Registry (providers.rs:170) ------------------------------------------
//
// Process-immutable provider set; construction order IS the default fallback
// order (03 §3.1). v0 holds exactly one provider (senshi), but the shape is
// the multiprovider one so M2 is additive (PORT_CPP.md §2.1/§2.4).
class ProviderRegistry {
 public:
  explicit ProviderRegistry(std::vector<std::unique_ptr<StreamProvider>> providers)
      : providers_(std::move(providers)) {
    // Rust asserts non-empty; a registry with no provider is a program bug.
  }

  [[nodiscard]] bool empty() const { return providers_.empty(); }
  [[nodiscard]] std::size_t size() const { return providers_.size(); }

  // providers[0]: primary / default (03 §3.2).
  [[nodiscard]] const StreamProvider* primary() const {
    return providers_.empty() ? nullptr : providers_.front().get();
  }

  // Indexed access in construction order (03 §3.1). The walk transport and the
  // meta-rail refresh iterate the registry by index (ordered()/avail snapshot).
  // Undefined past size() — callers loop i < size().
  [[nodiscard]] const StreamProvider* at(std::size_t i) const {
    return providers_[i].get();
  }

  // Owner of a persisted binding key. nullptr = retired provider; callers MUST
  // NOT fall back to primary() for bound rows (03 §3.2).
  [[nodiscard]] const StreamProvider* by_name(std::string_view name) const {
    for (const auto& p : providers_) {
      if (p->name() == name) return p.get();
    }
    return nullptr;
  }

  // Preferred first (when named + known), then construction order for the rest
  // (03 §3.2, ROD-344). An empty/unknown pref just yields construction order.
  // The resolve walk uses WorldView::ordered over names; this pointer form is
  // the CLI play path's registry view (providers.rs ordered).
  [[nodiscard]] std::vector<const StreamProvider*> ordered(
      std::optional<std::string_view> pref) const {
    const StreamProvider* p =
        (pref.has_value() && !pref->empty()) ? by_name(*pref) : nullptr;
    const std::string_view pref_name = p != nullptr ? p->name() : std::string_view{};
    std::vector<const StreamProvider*> out;
    out.reserve(providers_.size());
    if (p != nullptr) out.push_back(p);
    for (const auto& e : providers_) {
      if (p == nullptr || e->name() != pref_name) out.push_back(e.get());
    }
    return out;
  }

  // First entry of ordered(pref) that can search, nullptr if none can (03 §3.2,
  // ROD-491 deviation from zigoku's hard-fail). The CLI play path binds this:
  // an explicit preferred_provider is honoured when it can search, and when it
  // cannot the run notes the substitution and proceeds. Search-only — every
  // other CLI path still binds exactly one provider (a provider id is
  // meaningless on another).
  [[nodiscard]] const StreamProvider* preferred_searchable(
      std::optional<std::string_view> pref) const {
    for (const StreamProvider* p : ordered(pref)) {
      if (p->supports_search()) return p;
    }
    return nullptr;
  }

 private:
  std::vector<std::unique_ptr<StreamProvider>> providers_;
};

}  // namespace shigoku
