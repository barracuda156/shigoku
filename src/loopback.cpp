// loopback.cpp — ported from sabigoku src/loopback.rs (06 §4.4).

#include "loopback.hpp"

#include <sys/socket.h>
#include <sys/types.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

// macOS (and other BSDs) has no MSG_NOSIGNAL; not needed here (this listener
// only ever writes to a local loopback peer with SO_NOSIGPIPE-equivalent
// exposure limited to the same host), but kept for parity with proxy.cpp's
// send() calls in case a future refactor shares the write helper.
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#include <cerrno>
#include <cstring>
#include <optional>
#include <string_view>
#include <utility>

#include "nonce.hpp"

namespace shigoku::loopback {

namespace {

constexpr const char* kWordmark = "shigoku";

// Shared <style> for all three callback pages (DESIGN 5.5a): theme-aware via
// prefers-color-scheme, dark reusing theme.hpp's terminal_ghost hex. One
// constant so a contrast fix can't drift between pages.
constexpr const char* kPageStyle =
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

std::string scrubbed_page(const std::string& headline, const std::string& sub) {
  std::string out = "<!doctype html><html><head><meta charset=\"utf-8\">";
  out += "<script>history.replaceState(null,'','/')</script><title>";
  out += kWordmark;
  out += "</title>";
  out += kPageStyle;
  out += "</head><body><div class=\"card\"><div class=\"mark\">";
  out += kWordmark;
  out += "</div><p class=\"headline\">";
  out += headline;
  out += "</p><p class=\"sub\">";
  out += sub;
  out += "</p></div></body></html>";
  return out;
}

std::string result_page(const login::ConnectResult& result) {
  if (result.kind == login::ConnectResult::Kind::Ok) {
    return scrubbed_page("\xE2\x9C\x93 signed in to AniList", "you can close this tab");
  }
  return scrubbed_page("sign-in didn't complete", "check your terminal");
}

bool write_all(int fd, const char* data, std::size_t len) {
  std::size_t off = 0;
  while (off < len) {
    const ssize_t n = ::send(fd, data + off, len - off, MSG_NOSIGNAL);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
    off += static_cast<std::size_t>(n);
  }
  return true;
}

bool write_html(int fd, const std::string& body) {
  std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n";
  resp += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  resp += "Connection: close\r\n\r\n";
  resp += body;
  return write_all(fd, resp.data(), resp.size());
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

std::string request_target(const std::uint8_t* data, std::size_t len) {
  const std::string_view req(reinterpret_cast<const char*>(data), len);
  const std::size_t eol = req.find_first_of("\r\n");
  const std::string_view line = eol == std::string_view::npos ? req : req.substr(0, eol);
  // The request line's second space-separated token: "GET <target> HTTP/1.1".
  const std::size_t sp1 = line.find(' ');
  if (sp1 == std::string_view::npos) return {};
  const std::size_t sp2 = line.find(' ', sp1 + 1);
  const std::string_view target =
      sp2 == std::string_view::npos ? line.substr(sp1 + 1) : line.substr(sp1 + 1, sp2 - sp1 - 1);
  return std::string(target);
}

}  // namespace detail

void Canceler::cancel() {
  flag_->store(true, std::memory_order_release);
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port_);
  (void)::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  ::close(fd);
}

Result<Loopback, StartError> Loopback::start() { return start_on(login::kLoopbackPort); }

Result<Loopback, StartError> Loopback::start_ephemeral() { return start_on(0); }

Result<Loopback, StartError> Loopback::start_on(std::uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return err(StartError{std::string("socket: ") + std::strerror(errno)});
  }
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1 ONLY (06 §4.4).
  addr.sin_port = htons(port);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    const std::string msg = std::string("bind 127.0.0.1:") + std::to_string(port) + ": " +
                            std::strerror(errno);
    ::close(fd);
    return err(StartError{msg});
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0 || ::listen(fd, 16) != 0) {
    const std::string msg = std::string("listen: ") + std::strerror(errno);
    ::close(fd);
    return err(StartError{msg});
  }
  const std::uint16_t bound_port = ntohs(addr.sin_port);

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
  // The whole "no overall timeout" design (06 §4.4) leans on this
  // per-connection deadline; if it cannot be set, drop the socket rather than
  // risk a silent unbounded read wedging the single accept loop.
  timeval tv{};
  tv.tv_sec = kReadDeadlineSecs;
  tv.tv_usec = 0;
  if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) return std::nullopt;

  std::uint8_t buf[8192];
  const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
  if (n <= 0) return std::nullopt;

  const std::string target = detail::request_target(buf, static_cast<std::size_t>(n));
  if (target.empty()) return std::nullopt;

  constexpr std::string_view kCallbackPrefix = "/callback?";
  if (target.rfind(kCallbackPrefix, 0) == 0) {
    const std::string_view query = std::string_view(target).substr(kCallbackPrefix.size());
    // CSRF: the state must match the minted nonce; never verify/persist otherwise.
    auto state = login::detail::param(query, "state");
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
