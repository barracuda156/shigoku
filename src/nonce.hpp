// nonce.hpp — 128-bit hex nonces from the OS CSPRNG (P12). Ported from
// sabigoku src/nonce.rs.
//
// Reading /dev/urandom keeps this std-only (the port is Unix; Windows
// unsupported, 06 §1). Used for the de-cloak proxy's per-playback token
// (ROD-447): without an unguessable token any local process that scans the
// ephemeral port gets a free SSRF-guarded relay.

#pragma once

#include <string>

#include "result.hpp"

namespace shigoku::nonce {

// 32 lowercase hex chars (128 bits). Err carries an errno-style message on a
// /dev/urandom read failure.
[[nodiscard]] Result<std::string, std::string> mint();

}  // namespace shigoku::nonce
