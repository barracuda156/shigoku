// mal_loopback.hpp — the MAL OAuth callback listener (P31 §9.1 slice 2). No
// sabigoku reference (MAL never existed there). Deliberately a SEPARATE
// listener class from loopback.hpp's `Loopback`, not a shared one extended
// with a second route: MAL's Authorization Code grant hands the `code` back
// on a normal server-side redirect query string, so there is no
// location.hash relay-page trick to run (loopback.hpp's whole reason for a
// two-hit protocol) — folding both grant types into one class would tangle
// an Implicit-Grant-specific dance into a handler that doesn't need it.
//
// Binds the SAME port as AniList's Loopback (login::kLoopbackPort, 8767):
// only one connect session is ever active at a time (App::ConnectSession),
// so the two listeners never run concurrently, and MAL's registered
// redirect URI (mal_login::kRedirectUri) is pinned to that exact port. The
// callback PATH (/mal/callback, not /callback) is what tells the two flows
// apart if both are ever bound at different times against the same log.
//
// Threading / cancel / read-deadline shape is copied from loopback.hpp
// (start() binds fast on the caller's thread; serve() blocks and belongs on
// a worker thread; cancel() self-dials to unblock a parked accept()) — see
// that header for the full rationale, not repeated here.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "mal_login.hpp"
#include "result.hpp"

namespace shigoku::mal_loopback {

inline constexpr int kReadDeadlineSecs = 2;

class Canceler {
 public:
  Canceler(std::uint16_t port, std::shared_ptr<std::atomic<bool>> flag)
      : port_(port), flag_(std::move(flag)) {}

  void cancel();

 private:
  std::uint16_t port_;
  std::shared_ptr<std::atomic<bool>> flag_;
};

struct StartError {
  std::string detail;
};

class Loopback {
 public:
  // `client_id` is Config::mal_client_id, captured at start() (the modal's
  // lifetime is one connect attempt; a mid-flight client_id edit in Settings
  // cannot retroactively change an authorize URL the browser already has).
  [[nodiscard]] static Result<Loopback, StartError> start(std::string client_id);
  // Test-only: bind an ephemeral port (0) instead of kLoopbackPort.
  [[nodiscard]] static Result<Loopback, StartError> start_ephemeral(std::string client_id);

  Loopback(Loopback&&) noexcept;
  Loopback& operator=(Loopback&&) noexcept;
  Loopback(const Loopback&) = delete;
  Loopback& operator=(const Loopback&) = delete;
  ~Loopback();

  [[nodiscard]] std::uint16_t port() const { return port_; }
  [[nodiscard]] const std::string& state() const { return nonce_; }
  [[nodiscard]] const std::string& code_verifier() const { return pkce_.code_verifier; }
  [[nodiscard]] std::string authorize_url() const {
    return mal_login::authorize_url(client_id_, nonce_, pkce_.code_challenge);
  }
  [[nodiscard]] Canceler canceler() const { return Canceler(port_, cancel_flag_); }

  // Blocking accept loop (run off the UI thread). Same non-terminal/terminal
  // shape as loopback::Loopback::serve: a bad `state` calls on_bad_state and
  // keeps waiting (ROD-283 precedent — a forged/stray local hit must not
  // abort a login in flight); Canceled once cancel() unblocks accept().
  [[nodiscard]] mal_login::MalConnectResult serve(const mal_login::Exchanger& exchanger,
                                                   const std::string& auth_path, std::int64_t now,
                                                   const std::function<void()>& on_bad_state);

 private:
  Loopback() = default;
  static Result<Loopback, StartError> start_on(std::uint16_t port, std::string client_id);

  int listen_fd_ = -1;
  std::uint16_t port_ = 0;
  std::string nonce_;
  std::string client_id_;
  mal_login::Pkce pkce_;
  std::shared_ptr<std::atomic<bool>> cancel_flag_;
};

// --- Internals exposed for the golden tests --------------------------------
namespace detail {

[[nodiscard]] std::string success_page();
[[nodiscard]] std::string fail_page();

// Parse an HTTP/1.1 request head off a fixed byte buffer, returning the
// request-line target (e.g. "/mal/callback?code=...&state=..."), or empty
// on a malformed/absent request line. Byte-identical helper to
// loopback::detail::request_target, duplicated rather than shared: the two
// listeners are deliberately independent translation units (see the header
// comment), and this one function is too small to justify a shared leaf
// module over that independence.
[[nodiscard]] std::string request_target(const std::uint8_t* data, std::size_t len);

}  // namespace detail

}  // namespace shigoku::mal_loopback
