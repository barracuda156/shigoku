// loopback.cpp — ported from sabigoku src/loopback.rs (06 §4.4).

#include "loopback.hpp"

#include <sys/socket.h>
#include <sys/types.h>

#include <unistd.h>

#include <optional>
#include <string_view>
#include <utility>

#include "nonce.hpp"
#include "oauth_listener.hpp"

namespace shigoku::loopback {

namespace {

using oauth_listener::kPageStyle;
using oauth_listener::kWordmark;
using oauth_listener::scrubbed_page;
using oauth_listener::write_html;

std::string result_page(const login::ConnectResult& result) {
  if (result.kind == login::ConnectResult::Kind::Ok) {
    return scrubbed_page("\xE2\x9C\x93 signed in to AniList", "you can close this tab");
  }
  return scrubbed_page("sign-in didn't complete", "check your terminal");
}

}  // namespace

namespace detail {

std::string relay_page() {
  std::string out = "<!doctype html><html><head><meta charset=\"utf-8\"><title>";
  out += kWordmark;
  out += "</title>";
  out += kPageStyle;
  out += "</head><body><div class=\"card\"><div class=\"mark\">";
  out += kWordmark;
  out +=
      "</div><p class=\"headline\">finishing sign-in <span class=\"cursor\">\xE2\x96\x8C</span>"
      "</p></div><script>location.replace(\"/callback?\" + location.hash.substring(1));"
      "</script></body></html>";
  return out;
}

std::string success_page() { return scrubbed_page("\xE2\x9C\x93 signed in to AniList", "you can close this tab"); }

std::string fail_page() { return scrubbed_page("sign-in didn't complete", "check your terminal"); }

}  // namespace detail

Result<Loopback, StartError> Loopback::start() { return start_on(login::kLoopbackPort); }

Result<Loopback, StartError> Loopback::start_ephemeral() { return start_on(0); }

Result<Loopback, StartError> Loopback::start_on(std::uint16_t port) {
  auto listening = oauth_listener::listen_loopback(port);
  if (!listening.has_value()) return err(StartError{listening.error()});
  const int fd = listening->fd;
  const std::uint16_t bound_port = listening->port;

  auto minted = nonce::mint();
  if (!minted.has_value()) {
    ::close(fd);
    return err(StartError{minted.error()});
  }

  Loopback lp;
  lp.listen_fd_ = fd;
  lp.port_ = bound_port;
  lp.nonce_ = *minted;
  lp.cancel_flag_ = std::make_shared<std::atomic<bool>>(false);
  return lp;
}

Loopback::Loopback(Loopback&& other) noexcept
    : listen_fd_(other.listen_fd_),
      port_(other.port_),
      nonce_(std::move(other.nonce_)),
      cancel_flag_(std::move(other.cancel_flag_)) {
  other.listen_fd_ = -1;
}

Loopback& Loopback::operator=(Loopback&& other) noexcept {
  if (this != &other) {
    if (listen_fd_ >= 0) ::close(listen_fd_);
    listen_fd_ = other.listen_fd_;
    port_ = other.port_;
    nonce_ = std::move(other.nonce_);
    cancel_flag_ = std::move(other.cancel_flag_);
    other.listen_fd_ = -1;
  }
  return *this;
}

Loopback::~Loopback() {
  if (listen_fd_ >= 0) ::close(listen_fd_);
}

namespace {

// Handle one accepted connection. Returns nullopt to keep serving (a relay
// hit, or a socket-level failure worth dropping silently); Some(result) when
// this connection reached a terminal outcome (including BadState, which the
// caller treats as non-terminal for the OVERALL wait per ROD-283 — only
// serve()'s loop knows that distinction).
std::optional<login::ConnectResult> handle_conn(int fd, const std::string& nonce,
                                                 const login::Verifier& verifier,
                                                 const std::string& auth_path, std::int64_t now) {
  const std::string target = oauth_listener::read_target(fd);
  if (target.empty()) return std::nullopt;

  constexpr std::string_view kCallbackPrefix = "/callback?";
  if (target.rfind(kCallbackPrefix, 0) == 0) {
    const std::string_view query = std::string_view(target).substr(kCallbackPrefix.size());
    // CSRF: the state must match the minted nonce; never verify/persist otherwise.
    auto state = oauth_listener::param(query, "state");
    login::ConnectResult result = (!state.has_value() || *state != nonce)
                                       ? login::ConnectResult::bad_state()
                                       : login::complete_login(query, verifier, auth_path, now);
    (void)write_html(fd, result_page(result));
    return result;
  }
  (void)write_html(fd, detail::relay_page());
  return std::nullopt;
}

}  // namespace

login::ConnectResult Loopback::serve(const login::Verifier& verifier, const std::string& auth_path,
                                      std::int64_t now, const std::function<void()>& on_bad_state) {
  for (;;) {
    const int cfd = ::accept(listen_fd_, nullptr, nullptr);
    if (cfd < 0) return login::ConnectResult::network_error();
    if (cancel_flag_->load(std::memory_order_acquire)) {
      ::close(cfd);
      return login::ConnectResult::canceled();
    }
    std::optional<login::ConnectResult> outcome = handle_conn(cfd, nonce_, verifier, auth_path, now);
    ::close(cfd);
    if (!outcome.has_value()) continue;
    if (outcome->kind == login::ConnectResult::Kind::BadState) {
      on_bad_state();
      continue;
    }
    return *outcome;
  }
}

}  // namespace shigoku::loopback
