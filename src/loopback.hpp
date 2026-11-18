// loopback.hpp — the OAuth callback listener (06 §4.4, P19). Ported from
// sabigoku src/loopback.rs.
//
// Implicit Grant puts the token in `location.hash` (browser-only) — the
// server never sees the hash on the first GET. Shape: bind the well-known
// port, mint a CSRF nonce, open the browser at the authorize URL, then block
// in accept(). The first hit (any non-/callback path) gets a relay page whose
// script re-navigates to `/callback?<hash-as-query>`; THAT request carries
// the token where the server can read it. A bad/missing `state` on /callback
// is a forged or stray local request (ROD-283): warn, serve a fail page, and
// KEEP WAITING — never abort the login in flight over one bad hit.
//
// Threading: start() only binds (fast, local, non-blocking) — the caller runs
// serve() on a worker thread, off the UI thread (A1). cancel() sets a flag
// then dials the loopback once to unblock a parked accept() — accept() has no
// native cancel, so this is the only way to wake it.
//
// Bind is IPv4 127.0.0.1 only, deliberately (06 §4.4): a pure-::1 localhost
// host cannot reach it (rare; Happy Eyeballs falls back to IPv4).

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "login.hpp"
#include "oauth_listener.hpp"
#include "result.hpp"

namespace shigoku::loopback {

using oauth_listener::kReadDeadlineSecs;
using Canceler = oauth_listener::Canceler;
using StartError = oauth_listener::StartError;

class Loopback {
 public:
  [[nodiscard]] static Result<Loopback, StartError> start();
  // Test-only: bind an ephemeral port (0) instead of kLoopbackPort, so
  // parallel test runs never collide on the well-known port.
  [[nodiscard]] static Result<Loopback, StartError> start_ephemeral();

  Loopback(Loopback&&) noexcept;
  Loopback& operator=(Loopback&&) noexcept;
  Loopback(const Loopback&) = delete;
  Loopback& operator=(const Loopback&) = delete;
  ~Loopback();

  [[nodiscard]] std::uint16_t port() const { return port_; }
  [[nodiscard]] std::string authorize_url() const { return login::authorize_url(nonce_); }
  [[nodiscard]] Canceler canceler() const { return Canceler(port_, cancel_flag_); }

  // Blocking accept loop (run this off the UI thread). Returns the terminal
  // ConnectResult: a relay hit is not terminal (loop continues); a bad-state
  // callback is not terminal either (calls on_bad_state, keeps waiting).
  // Returns Canceled once cancel() has fired and unblocked accept().
  [[nodiscard]] login::ConnectResult serve(const login::Verifier& verifier,
                                            const std::string& auth_path, std::int64_t now,
                                            const std::function<void()>& on_bad_state);

 private:
  Loopback() = default;
  static Result<Loopback, StartError> start_on(std::uint16_t port);

  int listen_fd_ = -1;
  std::uint16_t port_ = 0;
  std::string nonce_;
  std::shared_ptr<std::atomic<bool>> cancel_flag_;
};

// --- Internals exposed for the golden tests (loopback.rs tests mod) -------
namespace detail {

// The relay page: byte-critical script that turns the fragment into a query
// on /callback. Do not reformat/rewrap this script.
[[nodiscard]] std::string relay_page();

// Terminal result pages. Never name the specific failure reason (only the
// terminal does, via the ConnectResult); both scrub the address bar
// (history.replaceState) as literally the first thing inside <head>.
[[nodiscard]] std::string success_page();
[[nodiscard]] std::string fail_page();

using oauth_listener::request_target;

}  // namespace detail

}  // namespace shigoku::loopback
