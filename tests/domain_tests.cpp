// domain_tests.cpp — P1 type-vocabulary tests.
//
// Ports the relevant sabigoku domain.rs #[test] cases 1:1 (§8: golden tests are
// the port contract), plus coverage for the enums, StreamLink field meaning,
// the error taxonomy, and the exhaustiveness fence (A2). A missing visitor
// lambda / switch arm must fail the BUILD — that half is proved by
// tests/fence_fail.cpp compiled on demand (see NOTES.md P1), not at runtime.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <string>
#include <variant>
#include <vector>

#include "../src/domain.hpp"
#include "../src/error.hpp"
#include "../src/event.hpp"
#include "../src/provider.hpp"
#include "../src/result.hpp"

using namespace shigoku;

// Test helper: append the UTF-8 encoding of a codepoint. Hostile codepoints
// (bidi controls, zero-width) are built this way rather than pasted as raw
// glyphs — a literal U+202E in this source would itself trip
// -Werror=bidi-chars, and the whole point of strip_controls is that these
// bytes are dangerous inside any buffer at all.
static void put_cp(std::string& out, unsigned long c) {
  if (c < 0x80) {
    out.push_back(static_cast<char>(c));
  } else if (c < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (c >> 6)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  } else if (c < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (c >> 12)));
    out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (c >> 18)));
    out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  }
}

// --- Translation / Quality / TitleLanguage round-trips ---------------------

TEST_CASE("translation string round-trip") {
  CHECK(to_string(Translation::Sub) == "sub");
  CHECK(to_string(Translation::Dub) == "dub");
  CHECK(parse_translation("sub") == Translation::Sub);
  CHECK(parse_translation("dub") == Translation::Dub);
  CHECK_FALSE(parse_translation("uwu").has_value());
}

TEST_CASE("quality parse defaults to best; cap matches rungs") {
  CHECK(parse_quality("worst") == Quality::Worst);
  CHECK(parse_quality("480") == Quality::P480);
  CHECK(parse_quality("720") == Quality::P720);
  CHECK(parse_quality("1080") == Quality::P1080);
  CHECK(parse_quality("garbage") == Quality::Best);  // unknown -> best.
  CHECK(quality_cap(Quality::P1080) == 1080u);
  CHECK(quality_cap(Quality::P720) == 720u);
  CHECK(quality_cap(Quality::P480) == 480u);
  CHECK_FALSE(quality_cap(Quality::Best).has_value());
  CHECK_FALSE(quality_cap(Quality::Worst).has_value());
}

TEST_CASE("title language parse defaults to romaji") {
  CHECK(parse_title_language("english") == TitleLanguage::English);
  CHECK(parse_title_language("native") == TitleLanguage::Native);
  CHECK(parse_title_language("romaji") == TitleLanguage::Romaji);
  CHECK(parse_title_language("xx") == TitleLanguage::Romaji);
}

// --- Season::from_month + current_cour (domain.rs, ROD-186) -----------------

TEST_CASE("season_from_month folds months to cours; out-of-range is winter") {
  CHECK(season_from_month(12) == Season::Winter);
  CHECK(season_from_month(1) == Season::Winter);
  CHECK(season_from_month(4) == Season::Spring);
  CHECK(season_from_month(7) == Season::Summer);
  CHECK(season_from_month(10) == Season::Fall);
  CHECK(season_from_month(0) == Season::Winter);
}

TEST_CASE("current_cour mid-year") {
  // 2026-07-18: summer 2026.
  CHECK(current_cour(1'784'332'800) == Cour{Season::Summer, 2026});
}

TEST_CASE("current_cour December rolls into next winter") {
  // 2025-12-15: winter 2026 (ROD-186 — December carries the year roll).
  CHECK(current_cour(1'765'756'800) == Cour{Season::Winter, 2026});
}

TEST_CASE("current_cour pre-epoch clamps to 1970 winter") {
  CHECK(current_cour(-1) == Cour{Season::Winter, 1970});
}

// --- weekday_of + schedule (P37 slice 1) ------------------------------------

TEST_CASE("weekday_of: epoch is Thursday, known dates check out") {
  CHECK(weekday_of(0) == Weekday::Thursday);              // 1970-01-01
  CHECK(weekday_of(86400) == Weekday::Friday);             // 1970-01-02
  CHECK(weekday_of(1'784'332'800) == Weekday::Saturday);   // 2026-07-18
  CHECK(weekday_of(1'765'756'800) == Weekday::Monday);     // 2025-12-15
}

TEST_CASE("weekday_of: pre-epoch floors toward the correct day, not zero") {
  // 1969-12-31 (one day before epoch) was a Wednesday.
  CHECK(weekday_of(-1) == Weekday::Wednesday);
  CHECK(weekday_of(-86400) == Weekday::Wednesday);
  CHECK(weekday_of(-86401) == Weekday::Tuesday);
}

namespace {

Show schedule_show(std::int64_t aid, std::optional<std::int64_t> next_airing_at,
                   std::optional<std::uint32_t> next_airing_episode) {
  Show s;
  s.enrichment.anilist_id = aid;
  s.enrichment.title_romaji = "Show " + std::to_string(aid);
  s.enrichment.next_airing_at = next_airing_at;
  s.enrichment.next_airing_episode = next_airing_episode;
  return s;
}

}  // namespace

TEST_CASE("schedule: groups by weekday, drops past/absent airings") {
  const std::int64_t now = 1'765'756'800;  // 2025-12-15 (Monday)
  const std::vector<Show> shows{
      schedule_show(1, now + 3600, 5),           // +1h, same day (Monday)
      schedule_show(2, now - 3600, 5),            // already aired: excluded
      schedule_show(3, std::nullopt, std::nullopt),  // unknown: excluded
      schedule_show(4, now + 86400 * 2, 8),       // +2d (Wednesday)
      schedule_show(5, now, 1),                   // == now: excluded (not future)
  };
  const auto groups = schedule(shows, now);
  REQUIRE(groups.size() == 2);
  CHECK(groups[0].weekday == Weekday::Monday);
  CHECK(groups[0].entries.size() == 1);
  CHECK(groups[0].entries[0].show_index == 0);
  CHECK(groups[0].entries[0].episode == 5);
  CHECK(groups[0].entries[0].seconds_until == 3600);
  CHECK(groups[1].weekday == Weekday::Wednesday);
  CHECK(groups[1].entries[0].show_index == 3);
}

TEST_CASE("schedule: entries within a weekday are airing-time ordered") {
  const std::int64_t now = 1'765'756'800;  // 2025-12-15 00:00 UTC (Monday)
  const std::vector<Show> shows{
      schedule_show(1, now + 86400 * 3, 1),       // Thursday 00:00, middle
      schedule_show(2, now + 86400 * 2, 1),       // Wednesday
      schedule_show(3, now + 86400 * 3 + 60, 1),  // Thursday 00:01, latest
      schedule_show(4, now + 86400 * 3 + 30, 1),  // Thursday 00:00:30, earliest of the two
  };
  const auto groups = schedule(shows, now);
  REQUIRE(groups.size() == 2);
  CHECK(groups[0].weekday == Weekday::Wednesday);
  REQUIRE(groups[1].weekday == Weekday::Thursday);
  REQUIRE(groups[1].entries.size() == 3);
  CHECK(groups[1].entries[0].show_index == 0);  // now+3d exactly, earliest
  CHECK(groups[1].entries[1].show_index == 3);
  CHECK(groups[1].entries[2].show_index == 2);  // latest
}

TEST_CASE("schedule: no future airings yields no groups") {
  const std::int64_t now = 1'765'756'800;
  const std::vector<Show> shows{
      schedule_show(1, std::nullopt, std::nullopt),
      schedule_show(2, now - 100, 1),
  };
  CHECK(schedule(shows, now).empty());
}

// --- detect_schedule_notices (P37 slice 3) ----------------------------------

namespace {

Show notice_show(std::int64_t aid, std::optional<std::int64_t> next_airing_at,
                 std::optional<std::uint32_t> next_airing_episode,
                 std::optional<std::uint32_t> notice_last_episode) {
  Show s = schedule_show(aid, next_airing_at, next_airing_episode);
  s.notice_last_episode = notice_last_episode;
  return s;
}

}  // namespace

TEST_CASE("detect_schedule_notices: a passed next_airing_at means THAT episode aired") {
  // The stored pair is AniList's nextAiringEpisode {episode airingAt}:
  // "episode N airs at T". T in the past -> N itself aired (the old N-1
  // reading missed every premiere and marked notices one episode low — P37
  // review).
  const std::int64_t now = 1'765'756'800;
  const std::vector<Show> shows{
      notice_show(1, now - 3600, 6, std::nullopt),
  };
  const auto notices = detect_schedule_notices(shows, now);
  REQUIRE(notices.size() == 1);
  CHECK(notices[0].show_index == 0);
  CHECK(notices[0].episode == 6);
}

TEST_CASE("detect_schedule_notices: a series premiere raises for episode 1") {
  const std::int64_t now = 1'765'756'800;
  const std::vector<Show> shows{
      // (T-1h, ep 1): the premiere aired an hour ago, mark already seeded at 0
      // is impossible (0 = value_or default) — an explicit 0 mark still raises.
      notice_show(1, now - 3600, 1, 0),
  };
  const auto notices = detect_schedule_notices(shows, now);
  REQUIRE(notices.size() == 1);
  CHECK(notices[0].episode == 1);
}

TEST_CASE("detect_schedule_notices: never re-raises an episode already at the high-water mark") {
  const std::int64_t now = 1'765'756'800;
  const std::vector<Show> shows{
      notice_show(1, now - 3600, 6, 6),   // ep 6 aired, already notified.
      notice_show(2, now - 3600, 6, 4),   // ep 6 aired, mark is stale (4 < 6): fresh notice.
  };
  const auto notices = detect_schedule_notices(shows, now);
  REQUIRE(notices.size() == 1);
  CHECK(notices[0].show_index == 1);
  CHECK(notices[0].episode == 6);
}

TEST_CASE("detect_schedule_notices: still-future airings and unknowns raise nothing") {
  const std::int64_t now = 1'765'756'800;
  const std::vector<Show> shows{
      notice_show(1, now + 3600, 6, std::nullopt),         // still in the future.
      notice_show(2, std::nullopt, std::nullopt, std::nullopt),  // finished or unknown.
      notice_show(3, now + 60, 1, std::nullopt),           // premiere still ahead.
  };
  CHECK(detect_schedule_notices(shows, now).empty());
}

TEST_CASE("detect_schedule_notices: multiple shows all report") {
  const std::int64_t now = 1'765'756'800;
  const std::vector<Show> shows{
      notice_show(1, now - 60, 3, std::nullopt),
      notice_show(2, now - 120, 9, 7),
  };
  const auto notices = detect_schedule_notices(shows, now);
  REQUIRE(notices.size() == 2);
  CHECK(notices[0].episode == 3);
  CHECK(notices[1].episode == 9);
}

// --- preferred_title (ROD-205 fallback chain) ------------------------------

TEST_CASE("preferred_title honors pref then falls back; blank never wins") {
  const std::string rom = "Sousou no Frieren";
  const std::string eng = "Frieren: Beyond Journey's End";
  std::string nat;  // built by byte to avoid pasting raw kanji.
  put_cp(nat, 0x8449); put_cp(nat, 0x9001); put_cp(nat, 0x306E);
  put_cp(nat, 0x30D5); put_cp(nat, 0x30EA); put_cp(nat, 0x30FC);
  put_cp(nat, 0x30EC); put_cp(nat, 0x30F3);  // "葬送のフリーレン"

  CHECK(preferred_title(rom, eng, nat, TitleLanguage::Romaji) == rom);
  CHECK(preferred_title(rom, eng, nat, TitleLanguage::English) == eng);
  CHECK(preferred_title(rom, eng, nat, TitleLanguage::Native) == nat);

  // English preferred but absent -> romaji backstop.
  CHECK(preferred_title(rom, std::nullopt, nat, TitleLanguage::English) == rom);
  // Native preferred, native blank -> romaji (blank never wins).
  CHECK(preferred_title(rom, eng, std::string_view{""}, TitleLanguage::Native) == rom);
  // Romaji preferred but empty, english present -> english.
  CHECK(preferred_title("", eng, nat, TitleLanguage::Romaji) == eng);
  // Everything empty -> empty romaji returned last.
  CHECK(preferred_title("", std::nullopt, std::nullopt, TitleLanguage::English) == "");
}

// --- strip_controls (ROD-247/435/439) --------------------------------------

TEST_CASE("strip_controls drops C0/C1/bidi/zero-width, keeps normal text") {
  CHECK(strip_controls("Frieren") == "Frieren");
  // C0 control (tab, newline) dropped.
  CHECK(strip_controls("a\tb\nc") == "abc");

  // DEL (U+007F) dropped. Built by byte so no raw control lands in source.
  {
    std::string in = "x";
    in.push_back(static_cast<char>(0x7F));
    in += "y";
    CHECK(strip_controls(in) == "xy");
  }

  // Bidi override (U+202E) and isolate (U+2066) dropped.
  {
    std::string in = "a";
    put_cp(in, 0x202E);
    in += "b";
    put_cp(in, 0x2066);
    in += "c";
    CHECK(strip_controls(in) == "abc");
  }

  // Zero-width space (U+200B), joiner (U+200D), BOM (U+FEFF) dropped.
  {
    std::string in = "a";
    put_cp(in, 0x200B);
    in += "b";
    put_cp(in, 0x200D);
    in += "c";
    put_cp(in, 0xFEFF);
    in += "d";
    CHECK(strip_controls(in) == "abcd");
  }

  // Legitimate multibyte (kanji) preserved. U+9280 = the kanji "gin".
  std::string kanji;
  put_cp(kanji, 0x9280);
  put_cp(kanji, 0x9280);
  CHECK(strip_controls(kanji) == kanji);
}

// --- episode_sort_key / cmp / map (03 §6.6, 02 §4b) ------------------------

TEST_CASE("episode_sort_key: numeric prefix, specials to +inf") {
  CHECK(episode_sort_key("1") == doctest::Approx(1.0));
  CHECK(episode_sort_key("1.5") == doctest::Approx(1.5));
  CHECK(episode_sort_key("12") == doctest::Approx(12.0));
  CHECK(std::isinf(episode_sort_key("SP1")));
  CHECK(std::isinf(episode_sort_key("OVA")));
  CHECK(std::isinf(episode_sort_key("")));
  // Malformed numeric prefix -> +inf (mirrors Rust parse().unwrap_or(INF)).
  CHECK(std::isinf(episode_sort_key("1.2.3")));
  CHECK(std::isinf(episode_sort_key(".")));
}

TEST_CASE("episode_label_cmp orders numbered before specials") {
  CHECK(episode_label_cmp("1", "2") < 0);
  CHECK(episode_label_cmp("2", "1.5") > 0);
  CHECK(episode_label_cmp("10", "SP1") < 0);  // numbered before special.
  CHECK(episode_label_cmp("SP1", "OVA") == 0);  // both +inf -> equal (stable).
  CHECK(episode_label_cmp("3", "3") == 0);
}

// --- parse_user_score (P34 slice 1, History `s` prompt) --------------------

TEST_CASE("parse_user_score: whole and decimal values scale x10") {
  auto v = parse_user_score("7");
  CHECK(v.kind == ScoreParse::Value);
  CHECK(v.value == 70);

  v = parse_user_score("7.5");
  CHECK(v.kind == ScoreParse::Value);
  CHECK(v.value == 75);

  v = parse_user_score("10");
  CHECK(v.kind == ScoreParse::Value);
  CHECK(v.value == 100);

  v = parse_user_score("0");
  CHECK(v.kind == ScoreParse::Value);
  CHECK(v.value == 0);

  // Rounds to nearest, not truncates.
  v = parse_user_score("6.66");
  CHECK(v.kind == ScoreParse::Value);
  CHECK(v.value == 67);
}

TEST_CASE("parse_user_score: whitespace and empty input clears") {
  CHECK(parse_user_score("").kind == ScoreParse::Clear);
  CHECK(parse_user_score("   ").kind == ScoreParse::Clear);
  CHECK(parse_user_score("\t").kind == ScoreParse::Clear);
  // Leading/trailing whitespace around a real value still parses (Clear wins
  // only when the trimmed string is fully empty).
  auto v = parse_user_score("  7.5  ");
  CHECK(v.kind == ScoreParse::Value);
  CHECK(v.value == 75);
}

TEST_CASE("parse_user_score: junk and out-of-range are Invalid, never throw") {
  CHECK(parse_user_score("junk").kind == ScoreParse::Invalid);
  CHECK(parse_user_score("7.5x").kind == ScoreParse::Invalid);   // trailing garbage.
  CHECK(parse_user_score("1.2.3").kind == ScoreParse::Invalid);  // malformed.
  CHECK(parse_user_score(".").kind == ScoreParse::Invalid);
  CHECK(parse_user_score("-1").kind == ScoreParse::Invalid);     // below range.
  CHECK(parse_user_score("10.1").kind == ScoreParse::Invalid);   // above range.
  CHECK(parse_user_score("11").kind == ScoreParse::Invalid);
  CHECK(parse_user_score("nan").kind == ScoreParse::Invalid);    // std::stod("nan") parses.
  CHECK(parse_user_score("inf").kind == ScoreParse::Invalid);
}

// --- to_anilist_score / from_anilist_score (P34 slice 2) --------------------

TEST_CASE("to_anilist_score: converts raw 0..100 to each scoreFormat's wire value") {
  CHECK(to_anilist_score(75, ScoreFormat::Point100) == 75.0);
  CHECK(to_anilist_score(75, ScoreFormat::Point10Decimal) == 7.5);
  CHECK(to_anilist_score(75, ScoreFormat::Point10) == 8.0);   // round(7.5) -> 8.
  CHECK(to_anilist_score(70, ScoreFormat::Point10) == 7.0);   // round(7.0) -> 7.
  CHECK(to_anilist_score(75, ScoreFormat::Point5) == 4.0);    // round(3.75) -> 4.
  CHECK(to_anilist_score(60, ScoreFormat::Point3) == 2.0);    // round(1.8) -> 2.
  CHECK(to_anilist_score(0, ScoreFormat::Point100) == 0.0);
  CHECK(to_anilist_score(0, ScoreFormat::Point10Decimal) == 0.0);
}

TEST_CASE("to_anilist_score: clamps overshoot past 100") {
  CHECK(to_anilist_score(150, ScoreFormat::Point100) == 100.0);
  CHECK(to_anilist_score(150, ScoreFormat::Point10) == 10.0);
}

TEST_CASE("from_anilist_score: converts each scoreFormat's wire value back to raw 0..100") {
  CHECK(from_anilist_score(75.0, ScoreFormat::Point100) == 75);
  CHECK(from_anilist_score(7.5, ScoreFormat::Point10Decimal) == 75);
  CHECK(from_anilist_score(8.0, ScoreFormat::Point10) == 80);
  CHECK(from_anilist_score(4.0, ScoreFormat::Point5) == 80);
  CHECK(from_anilist_score(2.0, ScoreFormat::Point3) == static_cast<std::uint32_t>(std::lround(2.0 / 3.0 * 100.0)));
}

TEST_CASE("from_anilist_score: non-positive/non-finite wire values are unset (0)") {
  CHECK(from_anilist_score(0.0, ScoreFormat::Point100) == 0);
  CHECK(from_anilist_score(-1.0, ScoreFormat::Point10) == 0);
  CHECK(from_anilist_score(std::numeric_limits<double>::quiet_NaN(), ScoreFormat::Point100) == 0);
  CHECK(from_anilist_score(std::numeric_limits<double>::infinity(), ScoreFormat::Point10) == 0);
}

TEST_CASE("to_anilist_score/from_anilist_score: round-trips at each format's own resolution") {
  for (ScoreFormat fmt : {ScoreFormat::Point100, ScoreFormat::Point10Decimal, ScoreFormat::Point10,
                          ScoreFormat::Point5, ScoreFormat::Point3}) {
    for (std::uint32_t raw : {10u, 25u, 50u, 75u, 100u}) {
      const double wire = to_anilist_score(raw, fmt);
      const std::uint32_t back = from_anilist_score(wire, fmt);
      // Lossy formats (Point5/Point3) don't round-trip exactly; only assert
      // the round trip lands within that format's own step size.
      double step = 1.0;
      switch (fmt) {
        case ScoreFormat::Point100:      step = 1.0; break;
        case ScoreFormat::Point10Decimal: step = 1.0; break;
        case ScoreFormat::Point10:       step = 10.0; break;
        case ScoreFormat::Point5:        step = 20.0; break;
        case ScoreFormat::Point3:        step = 34.0; break;
      }
      CHECK(std::abs(static_cast<double>(back) - static_cast<double>(raw)) <= step);
    }
  }
}

TEST_CASE("parse_score_format: known strings map, unknown falls back to Point100") {
  CHECK(parse_score_format("POINT_100") == ScoreFormat::Point100);
  CHECK(parse_score_format("POINT_10_DECIMAL") == ScoreFormat::Point10Decimal);
  CHECK(parse_score_format("POINT_10") == ScoreFormat::Point10);
  CHECK(parse_score_format("POINT_5") == ScoreFormat::Point5);
  CHECK(parse_score_format("POINT_3") == ScoreFormat::Point3);
  CHECK(parse_score_format("") == ScoreFormat::Point100);
  CHECK(parse_score_format("garbage") == ScoreFormat::Point100);
}

TEST_CASE("map_episode_index: exact label first, then ordinal") {
  std::vector<std::string> eps = {"1", "2", "3", "SP1"};
  CHECK(map_episode_index(eps, "2", 99) == std::optional<std::size_t>(1));  // exact.
  CHECK(map_episode_index(eps, "SP1", 1) == std::optional<std::size_t>(3));  // exact special.
  // No exact match -> 1-based ordinal.
  CHECK(map_episode_index(eps, "nope", 1) == std::optional<std::size_t>(0));
  CHECK(map_episode_index(eps, "nope", 4) == std::optional<std::size_t>(3));
  // Ordinal 0 or out of range -> nullopt.
  CHECK_FALSE(map_episode_index(eps, "nope", 0).has_value());
  CHECK_FALSE(map_episode_index(eps, "nope", 5).has_value());
}

TEST_CASE("is_still_airing settles only on FINISHED/CANCELLED") {
  CHECK_FALSE(is_still_airing(std::string_view{"FINISHED"}));
  CHECK_FALSE(is_still_airing(std::string_view{"finished"}));  // case-insensitive.
  CHECK_FALSE(is_still_airing(std::string_view{"CANCELLED"}));
  CHECK(is_still_airing(std::string_view{"RELEASING"}));
  CHECK(is_still_airing(std::string_view{"HIATUS"}));
  CHECK(is_still_airing(std::string_view{"NOT_YET_RELEASED"}));
  CHECK(is_still_airing(std::string_view{"whatever"}));
  CHECK(is_still_airing(std::nullopt));  // None -> still airing.
}

// --- StreamLink field meaning (P1 DoD) -------------------------------------

TEST_CASE("StreamLink carries cloaked/decloak/sub with defaults off") {
  StreamLink link;
  CHECK(link.cloaked_segments == false);
  CHECK(link.decloak_segments == false);
  CHECK_FALSE(link.sub_url.has_value());

  // The senshi shape (A8): cloaked true, decloak false, no proxy in v0.
  link.url = "https://cdn.example/master.m3u8";
  link.cloaked_segments = true;
  link.referer = "https://senshi.live/";
  link.sub_url = "https://cdn.example/en.vtt";
  CHECK(link.cloaked_segments);
  CHECK_FALSE(link.decloak_segments);
  CHECK(link.sub_url.has_value());
}

// --- error taxonomy (03 §7) ------------------------------------------------

TEST_CASE("ProviderError::from_status classifies like providers.rs") {
  CHECK(ProviderError::from_status(403).kind == ProviderError::Kind::Forbidden);
  CHECK(ProviderError::from_status(451).kind == ProviderError::Kind::Forbidden);
  CHECK(ProviderError::from_status(500).kind == ProviderError::Kind::Server);
  CHECK(ProviderError::from_status(599).kind == ProviderError::Kind::Server);
  CHECK(ProviderError::from_status(404).kind == ProviderError::Kind::Http);
  CHECK(ProviderError::from_status(418).kind == ProviderError::Kind::Http);
  CHECK(ProviderError::from_status(403).status == 403);
  CHECK(ProviderError::from_status(429).kind == ProviderError::Kind::RateLimited);
  CHECK(ProviderError::from_status(429).status == 429);
}

TEST_CASE("provider_error_copy is total over the taxonomy") {
  // If a new Kind is added, this switch fails to compile (-Werror=switch-enum),
  // which is the point: a new class forces a copy decision.
  CHECK(provider_error_copy(ProviderError::Kind::Network) == "network unreachable");
  CHECK(provider_error_copy(ProviderError::Kind::Unsupported) == "unsupported operation");
  CHECK(provider_error_copy(ProviderError::Kind::RateLimited) == "is rate-limiting us");
}

// --- Result<T,E> ergonomics -------------------------------------------------

TEST_CASE("Result carries value or error") {
  Result<int, ProviderError> good = 7;
  Result<int, ProviderError> bad = err(ProviderError::network());
  CHECK(good.has_value());
  CHECK(good.value() == 7);
  CHECK_FALSE(bad.has_value());
  CHECK(bad.error().kind == ProviderError::Kind::Network);
}

// --- provider guards (providers.rs) ----------------------------------------

TEST_CASE("guard_show_id accepts digits, rejects the rest") {
  CHECK(guard_show_id("52991").has_value());
  CHECK(guard_show_id("0").has_value());
  CHECK_FALSE(guard_show_id("").has_value());
  CHECK_FALSE(guard_show_id("../7").has_value());
  CHECK_FALSE(guard_show_id("12a").has_value());
  CHECK(guard_show_id("12a").error().kind == ProviderError::Kind::Decode);
}

TEST_CASE("clean_arg: printable ASCII only, non-empty") {
  CHECK(clean_arg("https://x/y.m3u8"));
  CHECK_FALSE(clean_arg(""));
  CHECK_FALSE(clean_arg("has space"));
  CHECK_FALSE(clean_arg(std::string("tab\there")));
  // A high byte (0x80) must be rejected; build it by byte so the source has no
  // ambiguous \x escape swallowing the following letters.
  {
    std::string in = "high";
    in.push_back(static_cast<char>(0x80));
    in += "byte";
    CHECK_FALSE(clean_arg(in));
  }
}

// --- Event variant + exhaustive visit (A2) ---------------------------------

TEST_CASE("Event visit with overloaded{} dispatches every alternative") {
  // A visitor covering EVERY alternative. If Event gains one and this set
  // doesn't, it FAILS TO COMPILE — the runtime side of the fence.
  auto classify = [](const Event& e) -> std::string {
    return std::visit(overloaded{
        [](const KeyEvent&) { return std::string("key"); },
        [](const MouseEvent&) { return std::string("mouse"); },
        [](const Resize&) { return std::string("resize"); },
        [](const Tick&) { return std::string("tick"); },
        [](const SearchDone&) { return std::string("search-done"); },
        [](const SearchError&) { return std::string("search-error"); },
        [](const DiscoverFeedDone&) { return std::string("discover-feed-done"); },
        [](const DiscoverFeedError&) { return std::string("discover-feed-error"); },
        [](const GridCoverDone&) { return std::string("grid-cover-done"); },
        [](const GridCoverError&) { return std::string("grid-cover-error"); },
        [](const EpisodesDone&) { return std::string("episodes-done"); },
        [](const EpisodesError&) { return std::string("episodes-error"); },
        [](const ProviderSearchDone&) { return std::string("provider-search-done"); },
        [](const ProviderSearchError&) { return std::string("provider-search-error"); },
        [](const CoverDone&) { return std::string("cover-done"); },
        [](const CoverError&) { return std::string("cover-error"); },
        [](const PositionUpdate&) { return std::string("position"); },
        [](const PlayDone&) { return std::string("play-done"); },
        [](const PlayErrorEvent&) { return std::string("play-error"); },
        [](const PlayRetry&) { return std::string("play-retry"); },
        [](const PrewarmResult&) { return std::string("prewarm-result"); },
        [](const ConnectDone&) { return std::string("connect-done"); },
        [](const MalConnectDone&) { return std::string("mal-connect-done"); },
        [](const SyncFlushed&) { return std::string("sync-flushed"); },
        [](const EnrichmentRefreshed&) { return std::string("enrichment-refreshed"); },
        [](const EnrichmentNull&) { return std::string("enrichment-null"); },
        [](const EnrichmentFailed&) { return std::string("enrichment-failed"); },
        [](const CharactersRecsDone&) { return std::string("characters-recs-done"); },
        [](const CharactersRecsNull&) { return std::string("characters-recs-null"); },
        [](const CharactersRecsFailed&) { return std::string("characters-recs-failed"); },
        [](const GenreCollectionDone&) { return std::string("genre-collection-done"); },
        [](const GenreCollectionFailed&) { return std::string("genre-collection-failed"); },
        [](const UpdateAvailable&) { return std::string("update-available"); },
        [](const DownloadProgress&) { return std::string("download-progress"); },
        [](const DownloadDone&) { return std::string("download-done"); },
        [](const DownloadFailed&) { return std::string("download-failed"); },
    }, e);
  };

  CHECK(classify(Event{Tick{}}) == "tick");
  CHECK(classify(Event{KeyEvent{U'q', KeyEvent::Special::None, false}}) == "key");
  CHECK(classify(Event{MouseEvent{MouseEvent::Kind::Press, MouseEvent::Button::Left,
                                  3, 7}}) == "mouse");
  CHECK(classify(Event{SearchDone{{}, "frieren", 1}}) == "search-done");
  CHECK(classify(Event{PlayRetry{2, 3}}) == "play-retry");
  CHECK(classify(Event{EnrichmentNull{700}}) == "enrichment-null");
  CHECK(classify(Event{EnrichmentFailed{700}}) == "enrichment-failed");
  CHECK(classify(Event{CharactersRecsDone{700, {}}}) == "characters-recs-done");
  CHECK(classify(Event{CharactersRecsNull{700}}) == "characters-recs-null");
  CHECK(classify(Event{CharactersRecsFailed{700}}) == "characters-recs-failed");
  CHECK(classify(Event{GenreCollectionDone{{"Action"}}}) == "genre-collection-done");
  CHECK(classify(Event{GenreCollectionFailed{}}) == "genre-collection-failed");
  CHECK(classify(Event{UpdateAvailable{"v0.5.0"}}) == "update-available");
  CHECK(classify(Event{DownloadProgress{512, 1024, 700, "7"}}) == "download-progress");
  CHECK(classify(Event{DownloadDone{700, "7", "/dl/700/sub/7.mp4"}}) == "download-done");
  CHECK(classify(Event{DownloadFailed{DownloadOutcome::FfmpegNotFound, {}, 700, "7"}}) ==
        "download-failed");
}

// --- after_play_status (domain.rs after_play tests, ported 1:1 — P30/R-12) --

TEST_CASE("after_play: completed sticks") {
  CHECK(after_play_status(ListStatus::Completed, 1, 12u, false) == ListStatus::Completed);
  CHECK(after_play_status(ListStatus::Completed, 0, std::nullopt, true) ==
        ListStatus::Completed);
}

TEST_CASE("after_play: still-airing never auto-completes (ROD-296)") {
  CHECK(after_play_status(ListStatus::Watching, 24, 24u, true) == ListStatus::Watching);
}

TEST_CASE("after_play: completes only at a known positive total") {
  CHECK(after_play_status(ListStatus::Watching, 12, 12u, false) == ListStatus::Completed);
  CHECK(after_play_status(ListStatus::Planning, 13, 12u, false) == ListStatus::Completed);
  CHECK(after_play_status(ListStatus::Watching, 11, 12u, false) == ListStatus::Watching);
  CHECK(after_play_status(ListStatus::Watching, 5, 0u, false) == ListStatus::Watching);
  CHECK(after_play_status(ListStatus::Watching, 5, std::nullopt, false) ==
        ListStatus::Watching);
}
