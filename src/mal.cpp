// mal.cpp — MAL API v2 REST client (P31 §9.1 slice 3). See mal.hpp.

#include "mal.hpp"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace shigoku::mal {

namespace {

bool is_unreserved(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
         c == '.' || c == '_' || c == '~';
}

}  // namespace

namespace detail {

const char* list_status_to_mal(ListStatus s) {
  switch (s) {
    case ListStatus::Planning:  return "plan_to_watch";
    case ListStatus::Watching:  return "watching";
    case ListStatus::Paused:    return "on_hold";
    case ListStatus::Completed: return "completed";
    case ListStatus::Dropped:   return "dropped";
  }
  return "plan_to_watch";  // unreachable (closed enum).
}

ListStatus list_status_from_mal(std::optional<std::string_view> s) {
  if (!s.has_value()) return ListStatus::Planning;
  if (*s == "watching") return ListStatus::Watching;
  if (*s == "on_hold") return ListStatus::Paused;
  if (*s == "completed") return ListStatus::Completed;
  if (*s == "dropped") return ListStatus::Dropped;
  if (*s == "plan_to_watch") return ListStatus::Planning;
  return ListStatus::Planning;
}

std::string percent_encode(std::string_view s) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    if (is_unreserved(c)) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[(c >> 4) & 0x0f]);
      out.push_back(kHex[c & 0x0f]);
    }
  }
  return out;
}

Result<std::optional<std::tuple<ListStatus, std::uint32_t, std::uint32_t>>, ProviderError>
parse_get_response(std::string_view json_body) {
  json resp;
  try {
    resp = json::parse(json_body.begin(), json_body.end());
  } catch (const json::parse_error& e) {
    return err(ProviderError::decode(e.what()));
  }
  if (!resp.is_object()) return err(ProviderError::decode("malformed response"));
  if (!resp.contains("my_list_status") || !resp.at("my_list_status").is_object()) {
    return std::optional<std::tuple<ListStatus, std::uint32_t, std::uint32_t>>(std::nullopt);
  }
  const json& mls = resp.at("my_list_status");
  const std::optional<std::string> status =
      mls.contains("status") && mls.at("status").is_string()
          ? std::optional<std::string>(mls.at("status").get<std::string>())
          : std::nullopt;
  const ListStatus ls = list_status_from_mal(
      status.has_value() ? std::optional<std::string_view>(*status) : std::nullopt);
  const std::uint32_t progress = mls.value("num_episodes_watched", std::uint32_t{0});
  const std::uint32_t score = mls.value("score", std::uint32_t{0});
  return std::optional<std::tuple<ListStatus, std::uint32_t, std::uint32_t>>(
      std::make_tuple(ls, progress, score));
}

std::string put_body(ListStatus status, std::uint32_t progress,
                     std::optional<std::uint32_t> score) {
  std::string body = "status=";
  body += percent_encode(list_status_to_mal(status));
  body += "&num_watched_episodes=";
  body += std::to_string(progress);
  if (score.has_value()) {
    // Withheld (nullopt) is NOT sent as 0: MAL reads score=0 as "remove the
    // score", which would wipe a rating the user set on the MAL site.
    body += "&score=";
    body += std::to_string(*score);
  }
  body += "&is_rewatching=false";
  return body;
}

}  // namespace detail

Result<std::optional<std::tuple<ListStatus, std::uint32_t, std::uint32_t>>, ProviderError>
get_list_entry(const http::Client& client, std::string_view token, std::int64_t mal_id) {
  http::Request req;
  req.method = http::Method::Get;
  req.url = std::string(kApiBase) + "/anime/" + std::to_string(mal_id) +
            "?fields=my_list_status";
  req.accept = http::Accept::Any2xx;
  req.extra_headers.push_back(http::Header{"Authorization", "Bearer " + std::string(token)});

  auto resp = client.fetch(req);
  if (!resp.has_value()) return err(resp.error());

  const std::string_view raw(reinterpret_cast<const char*>(resp->data()), resp->size());
  return detail::parse_get_response(raw);
}

Result<Unit, ProviderError> update_list_entry(const http::Client& client, std::string_view token,
                                               std::int64_t mal_id, ListStatus status,
                                               std::uint32_t progress,
                                               std::optional<std::uint32_t> score) {
  http::Request req;
  req.method = http::Method::Put;
  req.url = std::string(kApiBase) + "/anime/" + std::to_string(mal_id) + "/my_list_status";
  req.content_type = "application/x-www-form-urlencoded";
  const std::string body = detail::put_body(status, progress, score);
  req.body.assign(body.begin(), body.end());
  req.accept = http::Accept::Any2xx;
  req.extra_headers.push_back(http::Header{"Authorization", "Bearer " + std::string(token)});

  auto resp = client.fetch(req);
  if (!resp.has_value()) return err(resp.error());
  return Unit{};
}

}  // namespace shigoku::mal
