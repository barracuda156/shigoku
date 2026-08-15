// fixture_provider.hpp — a StreamProvider with canned data (P7 DoD): "a
// FixtureProvider (canned search/episodes/resolve) drives an automated pty
// test that walks search -> grid -> play against a stub mpv script".
//
// Test-only; lives in tests/ (not src/) since nothing in the real app links
// it. Net-new — PORT_CPP.md names the pattern but there is no Rust precedent
// to port (confirmed: no FixtureProvider/pty harness anywhere in the bible).

#pragma once

#include "../src/provider.hpp"

namespace shigoku::test {

// Fixed catalog: one show ("Fixture Frieren", mal_id 1) with 2 episodes and a
// canned resolve() so the pty test never touches the network.
class FixtureProvider final : public StreamProvider {
 public:
  [[nodiscard]] std::string_view name() const override { return "fixture"; }
  [[nodiscard]] std::string_view display_name() const override { return "Fixture"; }

  [[nodiscard]] std::optional<std::string> canonical_key(const Enrichment& show) const override {
    if (!show.mal_id.has_value()) return std::nullopt;
    return std::to_string(*show.mal_id);
  }

  [[nodiscard]] Result<std::vector<SearchHit>, ProviderError> search(
      std::string_view, const SearchOptions&) const override {
    return std::vector<SearchHit>{};  // Tier-A only in this fixture.
  }

  [[nodiscard]] Result<std::vector<std::string>, ProviderError> episodes(
      std::string_view provider_id, Translation, std::optional<std::uint32_t>) const override {
    if (provider_id != "1") return err(ProviderError::decode("unknown fixture id"));
    return std::vector<std::string>{"1", "2"};
  }

  [[nodiscard]] Result<StreamLink, ProviderError> resolve(
      std::string_view provider_id, std::string_view episode, Translation,
      Quality) const override {
    if (provider_id != "1") return err(ProviderError::decode("unknown fixture id"));
    StreamLink link;
    link.url = "https://fixture.invalid/" + std::string(episode) + ".m3u8";
    return link;
  }

  [[nodiscard]] Result<CoverRequest, ProviderError> cover_request(
      std::string_view) const override {
    return err(ProviderError::unsupported());
  }
};

}  // namespace shigoku::test
