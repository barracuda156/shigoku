// auth.cpp — ported from sabigoku src/auth.rs (06 §3).

#include "auth.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace shigoku {

namespace {

using json = nlohmann::json;

// Same JSON choice and per-field-defaulting rationale as config.cpp: no TOML
// library is vendored, and json::value(key, default) never throws, so one
// bad field degrades alone rather than failing the whole file.
Auth from_json(const json& j) {
  Auth a;
  if (j.contains("anilist") && j.at("anilist").is_object()) {
    const json& al = j.at("anilist");
    a.anilist.access_token = al.value("access_token", a.anilist.access_token);
    a.anilist.token_type = al.value("token_type", a.anilist.token_type);
    a.anilist.expires_at = al.value("expires_at", a.anilist.expires_at);
    a.anilist.user_id = al.value("user_id", a.anilist.user_id);
    a.anilist.user_name = al.value("user_name", a.anilist.user_name);
    a.anilist.score_format =
        parse_score_format(al.value("score_format", std::string(to_string(a.anilist.score_format))));
  }
  if (j.contains("mal") && j.at("mal").is_object()) {
    const json& ma = j.at("mal");
    a.mal.access_token = ma.value("access_token", a.mal.access_token);
    a.mal.refresh_token = ma.value("refresh_token", a.mal.refresh_token);
    a.mal.token_type = ma.value("token_type", a.mal.token_type);
    a.mal.expires_at = ma.value("expires_at", a.mal.expires_at);
    a.mal.user_id = ma.value("user_id", a.mal.user_id);
  }
  return a;
}

json to_json(const Auth& a) {
  return json{
      {"anilist",
       {
           {"access_token", a.anilist.access_token},
           {"token_type", a.anilist.token_type},
           {"expires_at", a.anilist.expires_at},
           {"user_id", a.anilist.user_id},
           {"user_name", a.anilist.user_name},
           {"score_format", std::string(to_string(a.anilist.score_format))},
       }},
      {"mal",
       {
           {"access_token", a.mal.access_token},
           {"refresh_token", a.mal.refresh_token},
           {"token_type", a.mal.token_type},
           {"expires_at", a.mal.expires_at},
           {"user_id", a.mal.user_id},
       }},
  };
}

std::string with_tmp_suffix(const std::string& path) { return path + ".tmp"; }

// Create `path` fresh at 0600 and write `bytes`. A stale temp from a prior
// crash is removed first: O_EXCL on an existing file would fail, and reusing
// it would inherit its old (possibly readable) mode.
Result<Unit, std::string> write_new_0600(const std::string& path, const std::string& bytes) {
  ::unlink(path.c_str());
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0) return err(std::string("open failed: ") + path);
  const char* data = bytes.data();
  std::size_t remaining = bytes.size();
  while (remaining > 0) {
    const ssize_t n = ::write(fd, data, remaining);
    if (n < 0) {
      ::close(fd);
      return err(std::string("write failed: ") + path);
    }
    data += n;
    remaining -= static_cast<std::size_t>(n);
  }
  ::close(fd);
  return Unit{};
}

}  // namespace

std::optional<std::string_view> AniListAuth::bearer() const {
  if (access_token.empty()) return std::nullopt;
  for (unsigned char c : access_token) {
    if (c < 0x20) return std::nullopt;
  }
  return std::string_view(access_token);
}

bool AniListAuth::is_expired(std::int64_t now) const {
  return expires_at != 0 && now >= expires_at;
}

std::optional<std::string_view> MalAuth::bearer() const {
  if (access_token.empty()) return std::nullopt;
  for (unsigned char c : access_token) {
    if (c < 0x20) return std::nullopt;
  }
  return std::string_view(access_token);
}

bool MalAuth::is_expired(std::int64_t now) const {
  return expires_at != 0 && now >= expires_at;
}

Auth Auth::load(const std::string& path) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0) return Auth{};
  // Regular files only: reading a FIFO/device blocks forever on open, which
  // would wedge startup (a "total load" invariant). A non-regular file is
  // signed-out.
  if (!S_ISREG(st.st_mode) || static_cast<std::uint64_t>(st.st_size) > kMaxAuthBytes) {
    return Auth{};
  }

  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return Auth{};
  std::ostringstream buf;
  buf << in.rdbuf();
  if (!in.good() && !in.eof()) return Auth{};

  json parsed;
  try {
    parsed = json::parse(buf.str());
  } catch (const json::parse_error&) {
    return Auth{};
  }
  if (!parsed.is_object()) return Auth{};
  return from_json(parsed);
}

Result<Unit, std::string> Auth::save(const std::string& path) const {
  const std::string tmp = with_tmp_suffix(path);
  const std::string text = to_json(*this).dump(2);
  auto w = write_new_0600(tmp, text);
  if (!w.has_value()) return err(w.error());
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    return err(std::string("rename failed: ") + path);
  }
  return Unit{};
}

}  // namespace shigoku
