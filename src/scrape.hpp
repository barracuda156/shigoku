// scrape.hpp — pure string helpers shared by the scraping providers.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace shigoku::scrape {

// Value of `name="..."` anywhere in `block` (nullopt if absent/unterminated).
// Whitespace-agnostic by construction — attributes on their own line match the
// same as inline ones.
[[nodiscard]] std::optional<std::string> attr_value(std::string_view block, std::string_view name);

// Single-pass HTML entity decode (named amp/lt/gt/quot/apos/nbsp + &#NN; /
// &#xNN;). Unknown entities stay literal; a double-escaped literal decodes one
// level only (`Journey&amp;#039;s` -> `Journey&#039;s`).
[[nodiscard]] std::string decode_entities(std::string_view s);

// Strip leading and trailing space, tab, CR and LF.
[[nodiscard]] std::string_view trim(std::string_view s);

// Percent-encode a query value (RFC 3986 unreserved passes, everything else
// %XX; no '+' for space — this is a URL query string, not a form body).
[[nodiscard]] std::string url_encode(std::string_view s);

// application/x-www-form-urlencoded byte_serialize: unreserved (A-Za-z0-9 and
// *-._) pass, ' ' -> '+', everything else -> %XX (uppercase hex).
[[nodiscard]] std::string form_urlencode(std::string_view s);

// ASCII lowercase via std::tolower.
[[nodiscard]] std::string to_lower(std::string_view s);

// `scheme://host[:port]` of an absolute url, no trailing slash (nullopt if not
// absolute or the host is empty).
[[nodiscard]] std::optional<std::string> origin_of(std::string_view absolute_url);

// Positional "1".."n", clamped to [1, kMaxEpisodeHint].
[[nodiscard]] std::vector<std::string> numbered_labels(std::uint32_t n);

}  // namespace shigoku::scrape
