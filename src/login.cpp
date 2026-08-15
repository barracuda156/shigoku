// login.cpp — ported from sabigoku src/login.rs (06 §4).

#include "login.hpp"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <optional>
#include <utility>

namespace shigoku::login {

namespace detail {

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

}  // namespace detail

namespace {

std::optional<std::pair<std::string, std::optional<std::int64_t>>> extract_token(
    std::string_view raw) {
  auto token = detail::param(raw, "access_token");
  if (!token.has_value()) return std::nullopt;
  std::optional<std::int64_t> expires_in;
  if (auto s = detail::param(raw, "expires_in"); s.has_value()) {
    try {
      expires_in = std::stoll(*s);
    } catch (...) {
      expires_in = std::nullopt;
    }
  }
  return std::make_pair(std::move(*token), expires_in);
}

Auth build_auth(std::string token, std::optional<std::int64_t> expires_in,
                const anilist::Viewer& viewer, std::int64_t now) {
  Auth a;
  a.anilist.access_token = std::move(token);
  a.anilist.token_type = kDefaultTokenType;
  a.anilist.expires_at = expires_in.has_value() ? now + *expires_in : 0;
  a.anilist.user_id = viewer.id;
  a.anilist.user_name = viewer.name;
  a.anilist.score_format = viewer.score_format;
  return a;
}

}  // namespace

std::string authorize_url_bare() {
  return std::string("https://anilist.co/api/v2/oauth/authorize?client_id=") + kClientId +
         "&response_type=token";
}

std::string authorize_url(std::string_view state) {
  return authorize_url_bare() + "&state=" + std::string(state);
}

std::string normalize_paste(std::string_view line) {
  if (line.rfind("eyJ", 0) == 0) {
    return "access_token=" + std::string(line);
  }
  return std::string(line);
}

void open_browser(const std::string& url) {
#if defined(__APPLE__)
  constexpr const char* kOpenCmd = "open";
#else
  constexpr const char* kOpenCmd = "xdg-open";
#endif
  const pid_t pid = ::fork();
  if (pid < 0) return;
  if (pid == 0) {
    // Child: redirect std streams to /dev/null, then exec. _exit on any
    // failure so a fork()ed copy of the TUI process never runs on.
    const int devnull = ::open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      ::dup2(devnull, STDIN_FILENO);
      ::dup2(devnull, STDOUT_FILENO);
      ::dup2(devnull, STDERR_FILENO);
      if (devnull > STDERR_FILENO) ::close(devnull);
    }
    ::execlp(kOpenCmd, kOpenCmd, url.c_str(), static_cast<char*>(nullptr));
    ::_exit(127);
  }
  // Parent: reap the child without blocking the caller (best-effort browser
  // launch must never stall the UI thread).
  int status = 0;
  ::waitpid(pid, &status, 0);
}

Verifier make_anilist_verifier(const http::Client& client) {
  return [&client](std::string_view token) { return anilist::viewer(client, token); };
}

ConnectResult complete_login(std::string_view raw, const Verifier& verifier,
                              const std::string& auth_path, std::int64_t now) {
  auto extracted = extract_token(raw);
  if (!extracted.has_value()) return ConnectResult::no_token();
  auto& [token, expires_in] = *extracted;
  if (token.size() < kTokenFloor) return ConnectResult::no_token();

  auto verified = verifier(token);
  if (!verified.has_value()) return ConnectResult::network_error();
  if (!verified->has_value()) return ConnectResult::rejected();

  const Auth auth = build_auth(token, expires_in, **verified, now);
  auto saved = auth.save(auth_path);
  if (!saved.has_value()) return ConnectResult::save_failed();
  return ConnectResult::ok(auth.anilist.user_name);
}

}  // namespace shigoku::login
