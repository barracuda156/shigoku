// senshi_oct.hpp — the senshi stream hop through the site's own player
// runtime. vendor.js (vidcloud) defines `window.__oct`, whose open(id)
// negotiates the sources over an ECDH / AES-GCM handshake with a WASM
// helper in the loop; nothing short of running that script reproduces it, so
// the hop runs it under Node behind a browser-shaped shim and reads the JSON
// back. Node is a runtime-only dependency of this one source: missing means
// the hop is unavailable, never a build or config error.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "error.hpp"
#include "result.hpp"

namespace shigoku::senshi::oct {

// The shim fed to `node -` on stdin: enough of window/document for vendor.js
// to load, every fetch dressed as the site's own page, then `__oct.open(id)`
// printed as JSON (or `{"error": "..."}` with exit 1). Exposed so the tests
// and the live smoke run exactly what the provider runs.
extern const char* const kShim;

// One handshake's wall-clock bound: the site answers in a second or two on
// a fast box, so this only guards against a hung runtime on a slow one.
inline constexpr int kTimeoutMs = 60000;

// `node_argv` + ["-", id, user_agent] -> the runtime's answer, raw JSON (an
// array of sources; Senshi::resolve's parse_sources reads it). Node missing
// = Unsupported with the reason in detail (this machine lacks the hop); a
// crash, a timeout, a non-zero exit or an answer that is not JSON = Decode.
[[nodiscard]] Result<std::string, ProviderError> open(const std::vector<std::string>& node_argv,
                                                      std::int64_t remote_source_id,
                                                      std::string_view user_agent);

}  // namespace shigoku::senshi::oct
