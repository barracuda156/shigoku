// aniskip.cpp — P22. Ported from sabigoku src/aniskip.rs.

#include "aniskip.hpp"

#include <sys/stat.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>

#include <nlohmann/json.hpp>

namespace shigoku::aniskip {

namespace {

using json = nlohmann::json;

// Min OP/ED length. The script announces then seeks 0.4s later; a sub-second
// window can be overrun by natural playback and turn the absolute seek into a
// rewind.
constexpr double kMinIntervalSecs = 1.0;

bool valid_interval(double start, double end) {
  return start >= 0.0 && end - start >= kMinIntervalSecs;
}

// mkdir -p (mirrors paths.cpp's file-local helper; best-effort, real failure
// surfaces on the subsequent open).
void mkdir_p(const std::string& dir) {
  std::string prefix;
  std::size_t pos = 0;
  while (pos < dir.size()) {
    const std::size_t next = dir.find('/', pos + 1);
    prefix = (next == std::string::npos) ? dir : dir.substr(0, next);
    if (!prefix.empty()) ::mkdir(prefix.c_str(), 0755);
    if (next == std::string::npos) break;
    pos = next;
  }
}

// mpv user-script: OP/ED from --script-opts; -1 disables a segment; mode gates
// intro/outro. Announce before the seek so the jump reads intentional, not a
// glitch. `skipped` flags debounce the high-frequency time-pos observer;
// file-loaded resets them if episodes chain in one mpv.
constexpr const char* kLuaScript = R"LUA(local opts = require("mp.options")
local o = { op_start = -1, op_end = -1, ed_start = -1, ed_end = -1, mode = "both" }
opts.read_options(o, "aniskip")

local skipped = { op = false, ed = false }

local function skip_section(target, label)
    mp.osd_message(label, 2.0)
    mp.add_timeout(0.4, function()
        mp.commandv("seek", target, "absolute")
    end)
end

mp.observe_property("time-pos", "number", function(_, pos)
    if not pos then return end
    if (o.mode == "intro" or o.mode == "both") and o.op_start >= 0
        and not skipped.op and pos >= o.op_start and pos < o.op_end then
        skipped.op = true
        skip_section(o.op_end, "Skipping intro...")
    end
    if (o.mode == "outro" or o.mode == "both") and o.ed_start >= 0
        and not skipped.ed and pos >= o.ed_start and pos < o.ed_end then
        skipped.ed = true
        skip_section(o.ed_end, "Skipping ending...")
    end
end)

mp.register_event("file-loaded", function()
    skipped.op = false
    skipped.ed = false
end)
)LUA";

// Trim ASCII whitespace, mirroring Rust's str::trim for this ASCII-only need.
std::string_view trim(std::string_view s) {
  std::size_t b = 0;
  while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  std::size_t e = s.size();
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

// Strict decimal-integer parse of the WHOLE trimmed string (Rust's
// str::parse::<u32>() rejects trailing garbage like "12.5" outright, unlike
// atoi/stoi which would silently truncate to 12).
std::optional<std::uint32_t> parse_u32_strict(std::string_view s) {
  if (s.empty()) return std::nullopt;
  std::uint32_t v = 0;
  for (const char c : s) {
    if (c < '0' || c > '9') return std::nullopt;
    const std::uint32_t digit = static_cast<std::uint32_t>(c - '0');
    if (v > (std::numeric_limits<std::uint32_t>::max() - digit) / 10) {
      return std::nullopt;  // overflow
    }
    v = v * 10 + digit;
  }
  return v;
}

std::string format_secs(double v) {
  // "-1" / "1340" for integral values, else Rust's default float Display
  // (shortest round-tripping decimal) — {} formatting drops a trailing ".0".
  if (v == static_cast<double>(static_cast<long long>(v))) {
    return std::to_string(static_cast<long long>(v));
  }
  std::ostringstream oss;
  oss.precision(15);
  oss << v;
  return oss.str();
}

}  // namespace

namespace detail {

SkipTimes times_from_body(std::string_view body) {
  SkipTimes t;
  json resp;
  try {
    resp = json::parse(body.begin(), body.end());
  } catch (const json::exception&) {
    return t;
  }
  if (!resp.is_object() || !resp.contains("results") || !resp.at("results").is_array()) {
    return t;
  }
  for (const auto& r : resp.at("results")) {
    if (!r.is_object() || !r.contains("interval") || !r.at("interval").is_object()) continue;
    const auto& iv = r.at("interval");
    double start = 0.0, end = 0.0;
    if (iv.contains("startTime") && iv.at("startTime").is_number()) {
      start = iv.at("startTime").get<double>();
    }
    if (iv.contains("endTime") && iv.at("endTime").is_number()) {
      end = iv.at("endTime").get<double>();
    }
    if (!valid_interval(start, end)) continue;
    std::string skip_type;
    if (r.contains("skipType") && r.at("skipType").is_string()) {
      skip_type = r.at("skipType").get<std::string>();
    }
    if (skip_type == "op" && !t.op.has_value()) {
      t.op = std::make_pair(start, end);
    } else if (skip_type == "ed" && !t.ed.has_value()) {
      t.ed = std::make_pair(start, end);
    }
  }
  return t;
}

std::optional<std::string> build_opts(const SkipTimes& t, SkipMode mode) {
  if (mode == SkipMode::None) return std::nullopt;
  const bool want_op = mode == SkipMode::Intro || mode == SkipMode::Both;
  const bool want_ed = mode == SkipMode::Outro || mode == SkipMode::Both;
  const auto op = want_op ? t.op : std::nullopt;
  const auto ed = want_ed ? t.ed : std::nullopt;
  if (!op.has_value() && !ed.has_value()) return std::nullopt;
  const auto [op_start, op_end] = op.value_or(std::make_pair(-1.0, -1.0));
  const auto [ed_start, ed_end] = ed.value_or(std::make_pair(-1.0, -1.0));
  const char* mode_str = "both";
  if (mode == SkipMode::Intro) mode_str = "intro";
  else if (mode == SkipMode::Outro) mode_str = "outro";
  std::string out = "aniskip-op_start=" + format_secs(op_start);
  out += ",aniskip-op_end=" + format_secs(op_end);
  out += ",aniskip-ed_start=" + format_secs(ed_start);
  out += ",aniskip-ed_end=" + format_secs(ed_end);
  out += ",aniskip-mode=";
  out += mode_str;
  return out;
}

SkipTimes fetch(std::int64_t mal_id, std::uint32_t episode) {
  auto client = http::Client::create();
  if (!client.has_value()) return {};
  std::string url = kEndpoint;
  url += "/" + std::to_string(mal_id) + "/" + std::to_string(episode);
  url += "?types[]=op&types[]=ed&episodeLength=0";
  http::Request req;
  req.method = http::Method::Get;
  req.url = url;
  req.user_agent = kUserAgent;
  req.extra_headers.push_back({"Accept", "application/json"});
  req.accept = http::Accept::OkOnly;
  auto body = client->fetch(req);
  if (!body.has_value()) return {};
  return times_from_body(std::string_view(reinterpret_cast<const char*>(body->data()),
                                          body->size()));
}

std::optional<std::int64_t> jikan_mal_id(std::string_view title) {
  if (title.empty()) return std::nullopt;
  auto client = http::Client::create();
  if (!client.has_value()) return std::nullopt;

  // URL-encode the query (spaces and non-ASCII are common in anime titles).
  std::string encoded;
  encoded.reserve(title.size());
  for (const unsigned char c : title) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded.push_back(static_cast<char>(c));
    } else {
      char buf[4];
      std::snprintf(buf, sizeof(buf), "%%%02X", c);
      encoded += buf;
    }
  }

  std::string url = kJikanEndpoint;
  url += "?q=" + encoded + "&limit=1";
  http::Request req;
  req.method = http::Method::Get;
  req.url = url;
  req.user_agent = kUserAgent;
  req.extra_headers.push_back({"Accept", "application/json"});
  req.accept = http::Accept::OkOnly;
  auto body = client->fetch(req);
  if (!body.has_value()) return std::nullopt;

  json resp;
  try {
    resp = json::parse(body->begin(), body->end());
  } catch (const json::exception&) {
    return std::nullopt;
  }
  if (!resp.is_object() || !resp.contains("data") || !resp.at("data").is_array() ||
      resp.at("data").empty()) {
    return std::nullopt;
  }
  const auto& first = resp.at("data").at(0);
  if (!first.is_object() || !first.contains("mal_id") || !first.at("mal_id").is_number_integer()) {
    return std::nullopt;
  }
  return first.at("mal_id").get<std::int64_t>();
}

std::optional<std::string> ensure_script(std::string_view cache_dir) {
  mkdir_p(std::string(cache_dir));
  const std::string path = std::string(cache_dir) + "/skip.lua";
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) return std::nullopt;
  out << kLuaScript;
  if (!out.good()) return std::nullopt;
  out.close();
  return path;
}

}  // namespace detail

std::uint32_t episode_number(std::string_view raw, std::uint32_t ordinal) {
  return parse_u32_strict(trim(raw)).value_or(ordinal);
}

std::optional<player::SkipScript> prepare(std::optional<std::int64_t> mal_id,
                                          std::string_view title, std::uint32_t episode,
                                          SkipMode mode, std::string_view cache_dir) {
  if (mode == SkipMode::None) return std::nullopt;
  std::int64_t resolved_mal_id;
  if (mal_id.has_value()) {
    resolved_mal_id = *mal_id;
  } else {
    auto looked_up = detail::jikan_mal_id(title);
    if (!looked_up.has_value()) return std::nullopt;
    resolved_mal_id = *looked_up;
  }
  auto opts = detail::build_opts(detail::fetch(resolved_mal_id, episode), mode);
  if (!opts.has_value()) return std::nullopt;
  auto path = detail::ensure_script(cache_dir);
  if (!path.has_value()) return std::nullopt;
  return player::SkipScript{*path, *opts};
}

}  // namespace shigoku::aniskip
