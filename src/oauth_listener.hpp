// oauth_listener.hpp — socket and HTTP plumbing shared by the AniList and
// MyAnimeList OAuth callback listeners: bind, read one request head, write
// one HTML page, the callback pages' common markup, and the query-string
// parser. The two listener classes stay separate (see mal_loopback.hpp).

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "result.hpp"

namespace shigoku::oauth_listener {

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

struct Listening {
  int fd;
  std::uint16_t port;
};

// Bind 127.0.0.1:`port` (0 = ephemeral) and listen. The error text is the
// failing step ("socket: ...", "bind 127.0.0.1:<port>: ...", "listen: ...");
// the fd is closed on every failure.
[[nodiscard]] Result<Listening, std::string> listen_loopback(std::uint16_t port);

// Parse an HTTP/1.1 request head off a fixed byte buffer, returning the
// request-line target (e.g. "/callback?access_token=..."), or empty on a
// malformed/absent request line.
[[nodiscard]] std::string request_target(const std::uint8_t* data, std::size_t len);

// Arm the read deadline on `fd`, read one request head and return its target.
// Empty on every drop case: the deadline could not be set, nothing was
// received, or the request line is malformed.
[[nodiscard]] std::string read_target(int fd);

// Send `body` as a 200 text/html response with Connection: close.
bool write_html(int fd, const std::string& body);

inline constexpr const char* kWordmark = "shigoku";

// Shared <style> for the callback pages (DESIGN 5.5a): theme-aware via
// prefers-color-scheme, dark reusing theme.hpp's terminal_ghost hex. One
// constant so a contrast fix can't drift between pages.
inline constexpr const char* kPageStyle =
    "<style>:root{--bg:#fff;--card:#f4f6f4;--hair:#d7ddd7;--fg:#0b3d1e;--muted:#5a6b5f;--accent:#0b7a3b}"
    "@media(prefers-color-scheme:dark){:root{--bg:#020d06;--card:#0b1f18;--hair:#1a4030;--fg:#39ff6a;"
    "--muted:#2a6040;--accent:#20ffdd}}*{box-sizing:border-box}body{margin:0;min-height:100vh;display:flex;"
    "align-items:center;justify-content:center;background:var(--bg);color:var(--fg);"
    "font-family:ui-monospace,SFMono-Regular,Menlo,monospace}.card{background:var(--card);"
    "border:1px solid var(--hair);border-radius:10px;padding:32px 40px;text-align:center;max-width:360px}"
    ".mark{color:var(--muted);font-size:13px;letter-spacing:.08em;margin-bottom:18px}"
    ".headline{font-size:18px;margin:0 0 8px}.sub{color:var(--muted);font-size:14px;margin:0}"
    ".cursor{color:var(--accent);animation:blink 1s steps(1,end) infinite}"
    "@keyframes blink{50%{opacity:0}}</style>";

// A result page that scrubs the address bar (history.replaceState) as the
// first thing inside <head>.
[[nodiscard]] std::string scrubbed_page(const std::string& headline, const std::string& sub);

// Value of `key=` in a redirect/query/fragment, up to the next delimiter.
// The match is anchored to a param boundary (start or after &/?/#) so a key
// that is a suffix of another (xstate= vs state=) can never be read as it: a
// query parser in a security callback must not be a substring search.
[[nodiscard]] std::optional<std::string> param(std::string_view raw, std::string_view key);

}  // namespace shigoku::oauth_listener
