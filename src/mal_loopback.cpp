// mal_loopback.cpp — the MAL OAuth callback listener (P31 §9.1 slice 2). No
// sabigoku reference (see mal_loopback.hpp). Socket plumbing comes from
// oauth_listener.hpp; the request handler differs from loopback.cpp's
// (single-hit code+state extract, no relay page).

#include "mal_loopback.hpp"

#include <sys/socket.h>
#include <sys/types.h>

#include <unistd.h>

#include <optional>
#include <string_view>
#include <utility>

#include "nonce.hpp"
#include "oauth_listener.hpp"

namespace shigoku::mal_loopback {

namespace {

using oauth_listener::scrubbed_page;
using oauth_listener::write_html;

std::string result_page(const mal_login::MalConnectResult& result) {
  if (result.kind == mal_login::MalConnectResult::Kind::Ok) {
    return scrubbed_page("\xE2\x9C\x93 signed in to MyAnimeList", "you can close this tab");
  }
  return scrubbed_page("sign-in didn't complete", "check your terminal");
}

}  // namespace

namespace detail {

std::string success_page() {
  return scrubbed_page("\xE2\x9C\x93 signed in to MyAnimeList", "you can close this tab");
}

std::string fail_page() { return scrubbed_page("sign-in didn't complete", "check your terminal"); }

}  // namespace detail

Result<Loopback, StartError> Loopback::start(std::string client_id) {
  return start_on(mal_login::kLoopbackPort, std::move(client_id));
}

Result<Loopback, StartError> Loopback::start_ephemeral(std::string client_id) {
  return start_on(0, std::move(client_id));
}

Result<Loopback, StartError> Loopback::start_on(std::uint16_t port, std::string client_id) {
  auto listening = oauth_listener::listen_loopback(port);
  if (!listening.has_value()) return err(StartError{listening.error()});
  const int fd = listening->fd;
  const std::uint16_t bound_port = listening->port;

  auto minted_state = nonce::mint();
  if (!minted_state.has_value()) {
    ::close(fd);
    return err(StartError{minted_state.error()});
  }
  auto pkce = mal_login::mint_pkce();
  if (!pkce.has_value()) {
    ::close(fd);
    return err(StartError{pkce.error()});
  }

  Loopback lp;
  lp.listen_fd_ = fd;
  lp.port_ = bound_port;
  lp.nonce_ = *minted_state;
  lp.client_id_ = std::move(client_id);
  lp.pkce_ = *pkce;
  lp.cancel_flag_ = std::make_shared<std::atomic<bool>>(false);
  return lp;
}

Loopback::Loopback(Loopback&& other) noexcept
    : listen_fd_(other.listen_fd_),
      port_(other.port_),
      nonce_(std::move(other.nonce_)),
      client_id_(std::move(other.client_id_)),
      pkce_(std::move(other.pkce_)),
      cancel_flag_(std::move(other.cancel_flag_)) {
  other.listen_fd_ = -1;
}

Loopback& Loopback::operator=(Loopback&& other) noexcept {
  if (this != &other) {
    if (listen_fd_ >= 0) ::close(listen_fd_);
    listen_fd_ = other.listen_fd_;
    port_ = other.port_;
    nonce_ = std::move(other.nonce_);
    client_id_ = std::move(other.client_id_);
    pkce_ = std::move(other.pkce_);
    cancel_flag_ = std::move(other.cancel_flag_);
    other.listen_fd_ = -1;
  }
  return *this;
}

Loopback::~Loopback() {
  if (listen_fd_ >= 0) ::close(listen_fd_);
}

namespace {

std::optional<mal_login::MalConnectResult> handle_conn(int fd, const std::string& client_id,
                                                        const std::string& state,
                                                        const std::string& code_verifier,
                                                        const mal_login::Exchanger& exchanger,
                                                        const std::string& auth_path,
                                                        std::int64_t now) {
  const std::string target = oauth_listener::read_target(fd);
  if (target.empty()) return std::nullopt;

  constexpr std::string_view kCallbackPrefix = "/mal/callback?";
  if (target.rfind(kCallbackPrefix, 0) != 0) {
    // Any other path (favicon.ico, a stray probe) is silently ignored: unlike
    // AniList's relay-page dance, MAL's redirect lands directly on the
    // callback path, so a non-callback hit here is never a legitimate step
    // in the flow.
    return std::nullopt;
  }

  const std::string_view query = std::string_view(target).substr(kCallbackPrefix.size());
  auto got_state = oauth_listener::param(query, "state");
  mal_login::MalConnectResult result;
  if (!got_state.has_value() || *got_state != state) {
    result = mal_login::MalConnectResult::bad_state();
  } else {
    auto code = oauth_listener::param(query, "code");
    result = code.has_value() ? mal_login::complete_mal_login(client_id, *code, code_verifier,
                                                               exchanger, auth_path, now)
                               : mal_login::MalConnectResult::no_code();
  }
  (void)write_html(fd, result_page(result));
  return result;
}

}  // namespace

mal_login::MalConnectResult Loopback::serve(const mal_login::Exchanger& exchanger,
                                             const std::string& auth_path, std::int64_t now,
                                             const std::function<void()>& on_bad_state) {
  for (;;) {
    const int cfd = ::accept(listen_fd_, nullptr, nullptr);
    if (cfd < 0) return mal_login::MalConnectResult::network_error();
    if (cancel_flag_->load(std::memory_order_acquire)) {
      ::close(cfd);
      return mal_login::MalConnectResult::canceled();
    }
    auto outcome =
        handle_conn(cfd, client_id_, nonce_, pkce_.code_verifier, exchanger, auth_path, now);
    ::close(cfd);
    if (!outcome.has_value()) continue;
    if (outcome->kind == mal_login::MalConnectResult::Kind::BadState) {
      on_bad_state();
      continue;
    }
    return *outcome;
  }
}

}  // namespace shigoku::mal_loopback
