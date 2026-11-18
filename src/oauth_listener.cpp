// oauth_listener.cpp — see oauth_listener.hpp.

#include "oauth_listener.hpp"

#include <sys/socket.h>
#include <sys/types.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

// macOS (and other BSDs) has no MSG_NOSIGNAL; write_html() sets SO_NOSIGPIPE
// on the fd where that option exists.
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#include <cerrno>
#include <cstring>

namespace shigoku::oauth_listener {

namespace {

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

}  // namespace

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

Result<Listening, std::string> listen_loopback(std::uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return err(std::string("socket: ") + std::strerror(errno));
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
    return err(msg);
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0 || ::listen(fd, 16) != 0) {
    const std::string msg = std::string("listen: ") + std::strerror(errno);
    ::close(fd);
    return err(msg);
  }
  return Listening{fd, ntohs(addr.sin_port)};
}

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

std::string read_target(int fd) {
  // The whole "no overall timeout" design (06 §4.4) leans on this
  // per-connection deadline; if it cannot be set, drop the socket rather than
  // risk a silent unbounded read wedging the single accept loop.
  timeval tv{};
  tv.tv_sec = kReadDeadlineSecs;
  tv.tv_usec = 0;
  if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) return {};

  std::uint8_t buf[8192];
  const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
  if (n <= 0) return {};

  return request_target(buf, static_cast<std::size_t>(n));
}

bool write_html(int fd, const std::string& body) {
#ifdef SO_NOSIGPIPE
  const int one_nosigpipe = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one_nosigpipe, sizeof(one_nosigpipe));
#endif
  std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n";
  resp += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  resp += "Connection: close\r\n\r\n";
  resp += body;
  return write_all(fd, resp.data(), resp.size());
}

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

std::optional<std::string> param(std::string_view raw, std::string_view key) {
  const std::string needle = std::string(key) + "=";
  std::size_t from = 0;
  std::size_t idx;
  for (;;) {
    const std::size_t hit_rel = raw.substr(from).find(needle);
    if (hit_rel == std::string_view::npos) return std::nullopt;
    const std::size_t hit = from + hit_rel;
    if (hit == 0 || raw[hit - 1] == '&' || raw[hit - 1] == '?' || raw[hit - 1] == '#') {
      idx = hit;
      break;
    }
    from = hit + 1;
  }
  const std::string_view rest = raw.substr(idx + needle.size());
  const std::size_t end = rest.find_first_of("&# \r\n");
  const std::string_view value = end == std::string_view::npos ? rest : rest.substr(0, end);
  if (value.empty()) return std::nullopt;
  return std::string(value);
}

}  // namespace shigoku::oauth_listener
