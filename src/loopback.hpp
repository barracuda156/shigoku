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
#include "result.hpp"

namespace shigoku::loopback {

// Per-connection read deadline (ROD-448, tightened from the frozen 06 §4.4
// doc's 5s): bounds one stalled LOCAL socket so it cannot wedge the
// single-threaded accept/serve loop forever. There is deliberately no
// overall timeout on the whole OAuth wait (the browser may be slow) —only
// the per-connection deadline below.
inline constexpr int kReadDeadlineSecs = 2;

// Wakes a blocked serve() from the UI thread. A separate handle (not the
// Loopback itself) so the UI thread can hold this while the worker thread
// owns the Loopback across the blocking serve() call.
class Canceler {
 public:
  Canceler(std::uint16_t port, std::shared_ptr<std::atomic<bool>> flag)
      : port_(port), flag_(std::move(flag)) {}

  // Set the cancel flag, then dial 127.0.0.1:port once to unblock a parked
  // accept(). Best-effort: a refused dial means serve() already left.
  void cancel();

 private:
  std::uint16_t port_;
  std::shared_ptr<std::atomic<bool>> flag_;
};

// Reason start() could not bind. Mirrors login.rs's LoopbackUnavailable path
// (06 §4.3): the caller falls back to the paste flow / a toast.
struct StartError {
  std::string detail;
};

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

// Parse an HTTP/1.1 request head off a fixed byte buffer, returning the
// request-line target (e.g. "/callback?access_token=..."), or empty on a
// malformed/absent request line.
[[nodiscard]] std::string request_target(const std::uint8_t* data, std::size_t len);

}  // namespace detail

}  // namespace shigoku::loopback
