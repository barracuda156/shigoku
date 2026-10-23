// mal_refresh.hpp — the mirror's refresh seam over the real token endpoint:
// mal_login's rollover (the stored refresh_token redeemed for a new pair),
// shaped as the Refresher mal_mirror::pull_mirror takes, plus the save that
// follows a roll. Header-only glue between two modules that otherwise never
// meet (mal_login speaks OAuth, mal_mirror speaks the list calls); the two
// sync entry points — the interface's worker and `shigoku sync` — are its
// only users, and both already link both.

#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "auth.hpp"
#include "http.hpp"
#include "mal_login.hpp"
#include "mal_mirror.hpp"

namespace shigoku::mal_mirror {

// `client_id` is the registration the stored pair was issued under — the
// user's own when set, else the app's baked one, the same choice the connect
// flow makes; MAL refuses a refresh under any other. An empty id, or a pair
// with no refresh_token to redeem, is Refused: a reconnect is the way back
// either way.
inline Refresher make_http_refresher(const http::Client& client, std::string client_id) {
  return [&client, client_id = std::move(client_id)](const MalAuth& current,
                                                     std::int64_t now) -> Refresh {
    const mal_login::Exchanger exchanger = mal_login::make_http_exchanger(client);
    auto rolled = mal_login::refresh_mal_token(client_id, current, exchanger, now);
    if (rolled.has_value()) return Refresh::rolled(std::move(*rolled));
    using K = mal_login::MalConnectResult::Kind;
    return rolled.error().kind == K::NetworkError ? Refresh::unreachable() : Refresh::refused();
  };
}

// Saves a renewed pair over the auth file's .mal block, the file's other
// block kept as it is on disk (the connect flow's own load-merge rule: the
// file is written whole). False when it could not be written; the run that
// rolled still has the pair in memory, but the next one will find the old
// refresh_token, which MAL has by then retired.
inline bool save_rolled(const std::string& auth_path, const MalAuth& rolled) {
  Auth disk = Auth::load(auth_path);
  disk.mal = rolled;
  return disk.save(auth_path).has_value();
}

}  // namespace shigoku::mal_mirror
