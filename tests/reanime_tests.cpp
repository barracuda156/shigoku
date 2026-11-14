// reanime_tests.cpp — the WebAssembly interpreter (hand-assembled modules,
// then the captured flixcloud key mixer), the pure Re:ANIME parsers and the
// full unlock against fixtures captured live on 2026-11-14 (Frieren ep 1,
// HD-2), plus transport cases over a loopback fixture server.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <sys/socket.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../src/reanime.hpp"
#include "../src/wasm.hpp"

using namespace shigoku;
using namespace shigoku::reanime;
using namespace shigoku::reanime::detail;

namespace {

using Bytes = std::vector<std::uint8_t>;

std::string read_fixture(const std::string& name) {
  const std::string path = std::string(SHIGOKU_TEST_FIXTURES_DIR) + "/reanime/" + name;
  std::ifstream f(path, std::ios::binary);
  REQUIRE_MESSAGE(f.good(), "missing fixture: " << path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string hex(const Bytes& v) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (const std::uint8_t b : v) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0F]);
  }
  return out;
}

// Values captured with the fixtures (the page's own JS logic, re-run in
// Python over the same bytes).
constexpr const char* kSeed = "342e23c23afbf0c8";
constexpr const char* kToken = "33e22b68b8869d5eb7db2eba";
constexpr const char* kMixed = "399097bb5eeddd51c3a1cb72003e511962945cccce41eb2effde32f4cef4ef38";
constexpr const char* kPlaylistKey =
    "01a9fbc8ae8f2733a1716ad2ca0c300759e289063d41b2792f13a7c005abaacf";
constexpr const char* kMaster =
    "https://fetch8.flixcloud.cc/_v8/0CVzLUG5XzCIowwXsx_8nrmipv8QzC3Pcpcnlz-kytcNxUaclYDt36L_/"
    "master.m3u8";
constexpr const char* kSubsBase =
    "https://vault-94.spectralcdn.top/subtitles/60aa496b-cea9-45a8-a4f4-8395990c0d06/"
    "60aa496b-cea9-45a8-a4f4-8395990c0d06";

// --- A tiny module assembler ------------------------------------------------

void leb(Bytes& b, std::uint32_t v) {
  do {
    std::uint8_t byte = v & 0x7f;
    v >>= 7;
    if (v != 0) byte |= 0x80;
    b.push_back(byte);
  } while (v != 0);
}

void append(Bytes& to, const Bytes& from) { to.insert(to.end(), from.begin(), from.end()); }

Bytes section(std::uint8_t id, const Bytes& body) {
  Bytes s{id};
  leb(s, static_cast<std::uint32_t>(body.size()));
  append(s, body);
  return s;
}

struct Fn {
  std::uint32_t type;
  Bytes locals;  // the local-declaration vector, count first
  Bytes code;    // instructions through the final end
  std::string name;
};

Bytes build(const std::vector<Bytes>& types, const std::vector<Fn>& fns, bool memory = false,
            const Bytes& data = {}) {
  Bytes m{0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
  Bytes t;
  leb(t, static_cast<std::uint32_t>(types.size()));
  for (const auto& ty : types) append(t, ty);
  append(m, section(1, t));
  Bytes f;
  leb(f, static_cast<std::uint32_t>(fns.size()));
  for (const auto& fn : fns) leb(f, fn.type);
  append(m, section(3, f));
  if (memory) append(m, section(5, {1, 0, 1}));
  Bytes e;
  leb(e, static_cast<std::uint32_t>(fns.size()));
  for (std::uint32_t i = 0; i < fns.size(); ++i) {
    leb(e, static_cast<std::uint32_t>(fns[i].name.size()));
    e.insert(e.end(), fns[i].name.begin(), fns[i].name.end());
    e.push_back(0);
    leb(e, i);
  }
  append(m, section(7, e));
  Bytes c;
  leb(c, static_cast<std::uint32_t>(fns.size()));
  for (const auto& fn : fns) {
    Bytes body = fn.locals;
    append(body, fn.code);
    leb(c, static_cast<std::uint32_t>(body.size()));
    append(c, body);
  }
  append(m, section(10, c));
  if (!data.empty()) append(m, section(11, data));
  return m;
}

const Bytes kI32I32ToI32 = {0x60, 2, 0x7f, 0x7f, 1, 0x7f};
const Bytes kI32ToI32 = {0x60, 1, 0x7f, 1, 0x7f};
const Bytes kVoidToI32 = {0x60, 0, 1, 0x7f};
const Bytes kVoidToVoid = {0x60, 0, 0};

std::optional<wasm::Instance> load(const Bytes& m) { return wasm::Instance::load(m.data(), m.size()); }

std::optional<std::uint32_t> call1(wasm::Instance& inst, std::string_view name,
                                   std::vector<std::uint32_t> args) {
  auto r = inst.call(name, args);
  if (!r.has_value() || r->size() != 1) return std::nullopt;
  return (*r)[0];
}

}  // namespace

// ===========================================================================
// The interpreter
// ===========================================================================

TEST_CASE("wasm_runs_arithmetic_and_honours_the_signature") {
  auto inst = load(build({kI32I32ToI32}, {{0, {0}, {0x20, 0, 0x20, 1, 0x6a, 0x0b}, "add"}}));
  REQUIRE(inst.has_value());
  CHECK(call1(*inst, "add", {2, 40}) == 42u);
  CHECK(call1(*inst, "add", {0xFFFFFFFFu, 2}) == 1u);  // wraps
  CHECK_FALSE(inst->call("add", {1}).has_value());     // arity
  CHECK_FALSE(inst->call("nope", {1, 2}).has_value());
}

TEST_CASE("wasm_block_loop_and_br_if") {
  // sum 1..n: block { loop { if n==0 break; acc += n; n -= 1; continue } } acc
  const Bytes code = {0x02, 0x40, 0x03, 0x40, 0x20, 0,    0x45, 0x0d, 1,    0x20, 1, 0x20,
                      0,    0x6a, 0x21, 1,    0x20, 0,    0x41, 1,    0x6b, 0x21, 0, 0x0c,
                      0,    0x0b, 0x0b, 0x20, 1,    0x0b};
  auto inst = load(build({kI32ToI32}, {{0, {1, 1, 0x7f}, code, "sum"}}));
  REQUIRE(inst.has_value());
  CHECK(call1(*inst, "sum", {10}) == 55u);
  CHECK(call1(*inst, "sum", {0}) == 0u);
}

TEST_CASE("wasm_if_else_with_a_result_and_signed_compare") {
  const Bytes code = {0x20, 0, 0x20, 1, 0x4a, 0x04, 0x7f, 0x20, 0, 0x05, 0x20, 1, 0x0b, 0x0b};
  auto inst = load(build({kI32I32ToI32}, {{0, {0}, code, "max"}}));
  REQUIRE(inst.has_value());
  CHECK(call1(*inst, "max", {3, 7}) == 7u);
  CHECK(call1(*inst, "max", {9, 7}) == 9u);
  CHECK(call1(*inst, "max", {0xFFFFFFFFu, 2}) == 2u);  // -1 < 2 signed
}

TEST_CASE("wasm_memory_data_segments_and_little_endian_loads") {
  // data at 16: 01 02 03 04 'b'
  const Bytes data = {1, 0, 0x41, 16, 0x0b, 5, 1, 2, 3, 4, 'b'};
  const Bytes word = {0x41, 16, 0x28, 2, 0, 0x0b};          // i32.load
  const Bytes byte = {0x41, 16, 0x2d, 0, 4, 0x0b};          // i32.load8_u offset 4
  const Bytes poke = {0x41, 0xe4, 0x00, 0x20, 0, 0x3a, 0, 0, 0x41, 0xe4, 0x00, 0x2d, 0, 0, 0x0b};
  const Bytes oob = {0x41, 0xff, 0xff, 0x03, 0x28, 2, 0, 0x0b};  // 65535: 3 bytes short
  auto inst = load(build({kVoidToI32, kI32ToI32},
                         {{0, {0}, word, "word"},
                          {0, {0}, byte, "byte"},
                          {1, {0}, poke, "poke"},
                          {0, {0}, oob, "oob"}},
                         true, data));
  REQUIRE(inst.has_value());
  CHECK(call1(*inst, "word", {}) == 0x04030201u);
  CHECK(call1(*inst, "byte", {}) == static_cast<std::uint32_t>('b'));
  CHECK(call1(*inst, "poke", {0x1234}) == 0x34u);
  CHECK_FALSE(inst->call("oob", {}).has_value());
  auto read = inst->read(100, 1);
  REQUIRE(read.has_value());
  CHECK((*read)[0] == 0x34);
  CHECK_FALSE(inst->read(65535, 2).has_value());
  CHECK(inst->write(0, data.data(), 3));
  CHECK_FALSE(inst->write(65535, data.data(), 2));
}

TEST_CASE("wasm_traps_and_the_step_budget") {
  const Bytes div = {0x20, 0, 0x20, 1, 0x6e, 0x0b};
  const Bytes spin = {0x03, 0x40, 0x0c, 0, 0x0b, 0x0b};
  const Bytes unreachable = {0x00, 0x0b};
  auto inst = load(build({kI32I32ToI32, kVoidToVoid},
                         {{0, {0}, div, "div"}, {1, {0}, spin, "spin"}, {1, {0}, unreachable, "die"}}));
  REQUIRE(inst.has_value());
  CHECK(call1(*inst, "div", {7, 2}) == 3u);
  CHECK_FALSE(inst->call("div", {7, 0}).has_value());
  CHECK_FALSE(inst->call("spin", {}).has_value());
  CHECK_FALSE(inst->call("die", {}).has_value());
}

TEST_CASE("wasm_refuses_what_it_does_not_support") {
  const Bytes add = {0x20, 0, 0x20, 1, 0x6a, 0x0b};
  // f64 in a signature.
  CHECK_FALSE(load(build({{0x60, 1, 0x7c, 0}}, {{0, {0}, {0x0b}, "f"}})).has_value());
  // An opcode outside the subset (f32.const).
  CHECK_FALSE(load(build({kVoidToVoid}, {{0, {0}, {0x43, 0, 0, 0, 0, 0x1a, 0x0b}, "f"}})).has_value());
  // A memory access with no memory.
  CHECK_FALSE(load(build({kVoidToI32}, {{0, {0}, {0x41, 0, 0x28, 2, 0, 0x0b}, "f"}})).has_value());
  // An import.
  Bytes with_import = build({kI32I32ToI32}, {{0, {0}, add, "add"}});
  const Bytes import = section(2, {1, 1, 'm', 1, 'f', 0, 0});
  with_import.insert(with_import.begin() + 8 + section(1, {1, 0x60, 2, 0x7f, 0x7f, 1, 0x7f}).size(),
                     import.begin(), import.end());
  CHECK_FALSE(load(with_import).has_value());
  // Not a module at all, and a truncated one.
  const Bytes junk = {1, 2, 3};
  CHECK_FALSE(load(junk).has_value());
  Bytes good = build({kI32I32ToI32}, {{0, {0}, add, "add"}});
  good.pop_back();
  CHECK_FALSE(load(good).has_value());
}

// ===========================================================================
// Pure parsers
// ===========================================================================

TEST_CASE("parse_servers_reads_the_captured_flix_list") {
  auto servers = parse_servers(read_fixture("flix.json"));
  REQUIRE(servers.has_value());
  REQUIRE(servers->size() == 5);
  const auto sub = servers_for(*servers, Translation::Sub);
  REQUIRE(sub.size() == 3);
  CHECK(sub[0].name == "HD-2");
  CHECK(sub[0].link == "https://flixcloud.cc/e/rjyccuaq8h5n?v=2");
  CHECK(sub[1].name == "HD-1");
  CHECK(sub[2].name == "H-SUB");
  CHECK(sub[2].hardsub);
  const auto dub = servers_for(*servers, Translation::Dub);
  REQUIRE(dub.size() == 2);
  CHECK(dub[0].name == "HD-2");
  CHECK_FALSE(dub[0].hardsub);
}

TEST_CASE("parse_servers_drops_bad_entries_and_rejects_bad_bodies") {
  auto s = parse_servers(
      R"({"success":true,"servers":[)"
      R"({"serverName":"HD-1","dataLink":"https://x.example/e/a?v=1","dataType":"raw"},)"
      R"({"serverName":"HD-1","dataLink":"/e/a?v=1","dataType":"sub"},)"
      R"({"serverName":"HD-1","dataLink":"https://x.example/e/a b","dataType":"sub"},)"
      R"(7,)"
      R"({"serverName":"HD-1","dataLink":"https://x.example/e/a?v=1","dataType":"sub"}]})");
  REQUIRE(s.has_value());
  REQUIRE(s->size() == 1);
  CHECK((*s)[0].link == "https://x.example/e/a?v=1");
  auto empty = parse_servers(R"({"success":true,"servers":[]})");
  REQUIRE(empty.has_value());
  CHECK(empty->empty());
  CHECK_FALSE(parse_servers(R"({"message":"anilist_id, mal or tmdb+season required"})").has_value());
  CHECK_FALSE(parse_servers("<html>").has_value());
}

TEST_CASE("data_url_maps_an_embed_to_its_page_data") {
  CHECK(data_url("https://flixcloud.cc/e/rjyccuaq8h5n?v=2") ==
        "https://flixcloud.cc/e/rjyccuaq8h5n/__data.json?v=2");
  CHECK(data_url("https://flixcloud.cc/e/abc") == "https://flixcloud.cc/e/abc/__data.json");
  CHECK(data_url("https://flixcloud.cc/e/abc?v=1#t") == "https://flixcloud.cc/e/abc/__data.json?v=1");
  CHECK_FALSE(data_url("http://flixcloud.cc/e/abc").has_value());
  CHECK_FALSE(data_url("https://127.0.0.1/e/abc").has_value());
  CHECK_FALSE(data_url("https://flixcloud.cc/d/abc").has_value());
  CHECK_FALSE(data_url("https://flixcloud.cc/e/../x").has_value());
  CHECK_FALSE(data_url("https://flixcloud.cc/e/").has_value());
}

TEST_CASE("field_names_follow_the_seed") {
  const FieldNames n = field_names(kSeed);
  CHECK(n.key_frag == "kf_2a43fa35");
  CHECK(n.iv == "ivf_ce3db1a8");
  CHECK(n.container == "cd_d6d6be3f");
  CHECK(n.array == "ad_e3ead6ee");
  CHECK(n.object == "od_7afc0518");
  CHECK(n.token == "3894b6e1fb76db72_fb76db72");
  CHECK(n.key_frag2 == "9185fc6f674ac721_6cf77fb7");
  CHECK(field_names("").token.empty());
}

TEST_CASE("parse_embed_reads_the_captured_page_data") {
  auto page = parse_embed(read_fixture("embed_data.json"));
  REQUIRE(page.has_value());
  CHECK(page->seed == kSeed);
  CHECK(page->token == kToken);
  CHECK(page->audio_type == "dual");
  CHECK(page->wasm.size() == 391);
  CHECK(page->frag1.size() == 32);
  CHECK(page->frag2.size() == 32);
  CHECK(page->iv.size() == 16);
  REQUIRE(page->subtitles.size() == 3);
  CHECK(page->subtitles[0].label == "English (Full Subtitles [9volt])");
  CHECK(page->subtitles[0].is_default);
  CHECK(page->subtitles[0].url == std::string(kSubsBase) + "_eng_3.ass");
  CHECK_FALSE(page->subtitles[1].is_default);
}

TEST_CASE("parse_embed_names_what_is_missing") {
  std::string raw = read_fixture("embed_data.json");
  CHECK_FALSE(parse_embed("{}").has_value());
  CHECK_FALSE(parse_embed(R"({"type":"data","nodes":[{"type":"skip"}]})").has_value());
  // The seed renamed: no player node at all.
  std::string renamed = raw;
  renamed.replace(renamed.find("obfuscation_seed"), 16, "obfuscation_seex");
  auto none = parse_embed(renamed);
  REQUIRE_FALSE(none.has_value());
  CHECK(none.error().detail == "embed: no player data");
  // A different seed: every derived field name misses.
  std::string reseeded = raw;
  reseeded.replace(reseeded.find(kSeed), 16, "0000000000000000");
  auto missing = parse_embed(reseeded);
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error().kind == ProviderError::Kind::Decode);
}

TEST_CASE("parse_token_reply_finds_the_hashed_fields") {
  auto reply = parse_token_reply(read_fixture("token_reply.json"), kToken);
  REQUIRE(reply.has_value());
  CHECK(reply->frag3.size() == 32);
  CHECK(!reply->ciphertext.empty());
  CHECK(reply->ciphertext.size() % 16 == 0);
  CHECK_FALSE(parse_token_reply(read_fixture("token_reply.json"), "other").has_value());
  CHECK_FALSE(parse_token_reply("[]", kToken).has_value());
}

TEST_CASE("the_captured_module_mixes_the_fragments_and_yields_the_playlist_key") {
  auto page = parse_embed(read_fixture("embed_data.json"));
  auto reply = parse_token_reply(read_fixture("token_reply.json"), kToken);
  REQUIRE(page.has_value());
  REQUIRE(reply.has_value());
  auto inst = wasm::Instance::load(page->wasm.data(), page->wasm.size());
  REQUIRE(inst.has_value());
  REQUIRE(inst->ensure_pages(1));
  REQUIRE(inst->write(1000, page->frag1.data(), 32));
  REQUIRE(inst->write(1032, page->frag2.data(), 32));
  REQUIRE(inst->write(1064, reply->frag3.data(), 32));
  REQUIRE(inst->call("_s", {0x342e23c2u}).has_value());
  REQUIRE(inst->call("_r", {1000, 1032, 1064, 1096, 32}).has_value());
  auto mixed = inst->read(1096, 32);
  REQUIRE(mixed.has_value());
  CHECK(hex(*mixed) == kMixed);
  auto at = inst->call("_c", {});
  REQUIRE(at.has_value());
  REQUIRE(at->size() == 1);
  auto key = inst->read((*at)[0], 32);
  REQUIRE(key.has_value());
  CHECK(hex(*key) == kPlaylistKey);
}

TEST_CASE("unlock_opens_the_captured_master") {
  auto page = parse_embed(read_fixture("embed_data.json"));
  auto reply = parse_token_reply(read_fixture("token_reply.json"), kToken);
  REQUIRE(page.has_value());
  REQUIRE(reply.has_value());
  auto unlocked = unlock(*page, *reply);
  REQUIRE(unlocked.has_value());
  CHECK(unlocked->master == kMaster);
  CHECK(hex(unlocked->playlist_key) == kPlaylistKey);

  auto tampered = *reply;
  tampered.frag3[0] ^= 1;
  auto bad = unlock(*page, tampered);
  REQUIRE_FALSE(bad.has_value());
  CHECK(bad.error().kind == ProviderError::Kind::Decode);

  auto no_module = *page;
  no_module.wasm.resize(10);
  CHECK_FALSE(unlock(no_module, *reply).has_value());
}

TEST_CASE("pick_subtitle_by_track") {
  const std::string base = kSubsBase;
  const std::vector<Subtitle> subs = {
      {base + "_eng_3.ass", "English (Full Subtitles [9volt])", true},
      {base + "_eng_4.ass", "English (Signs & Songs)", false},
      {base + ".srt", "English (Dubtitle(AI))", false},
  };
  CHECK(pick_subtitle(subs, Translation::Sub) == base + "_eng_3.ass");
  CHECK(pick_subtitle(subs, Translation::Dub) == base + "_eng_4.ass");

  // No default: a "full" one, then anything but signs/dubtitles.
  const std::vector<Subtitle> no_default = {
      {"https://h.example/s.ass", "English (Signs & Songs)", false},
      {"https://h.example/f.ass", "English (Full)", false},
  };
  CHECK(pick_subtitle(no_default, Translation::Sub) == "https://h.example/f.ass");
  const std::vector<Subtitle> plain = {
      {"https://h.example/d.srt", "English (Dubtitle(AI))", false},
      {"https://h.example/e.ass", "English", false},
  };
  CHECK(pick_subtitle(plain, Translation::Sub) == "https://h.example/e.ass");

  // Non-English only, or unsafe urls only: nothing.
  CHECK_FALSE(
      pick_subtitle({{"https://h.example/es.ass", "Spanish", true}}, Translation::Sub).has_value());
  CHECK_FALSE(
      pick_subtitle({{"https://127.0.0.1/s.ass", "English", true}}, Translation::Sub).has_value());
  CHECK_FALSE(
      pick_subtitle({{"https://h.example/f.ass", "English (Full)", true}}, Translation::Dub).has_value());
}

TEST_CASE("valid_show_id_and_labels") {
  CHECK(valid_show_id("154587"));
  CHECK(valid_show_id("1"));
  CHECK_FALSE(valid_show_id(""));
  CHECK_FALSE(valid_show_id("0"));
  CHECK_FALSE(valid_show_id("012"));
  CHECK_FALSE(valid_show_id("-5"));
  CHECK_FALSE(valid_show_id("12a"));
  CHECK_FALSE(valid_show_id("12345678901"));
  CHECK(labels(3) == std::vector<std::string>{"1", "2", "3"});
  CHECK(labels(0) == std::vector<std::string>{"1"});
}

// ===========================================================================
// Provider surface
// ===========================================================================

TEST_CASE("canonical_key_is_a_real_anilist_id") {
  auto p = ReAnime::create();
  REQUIRE(p.has_value());
  Enrichment show;
  show.anilist_id = 154587;
  show.mal_id = 52991;
  CHECK(p->canonical_key(show) == "154587");
  show.anilist_id = -52991;  // a MAL-only row's synthetic key
  CHECK_FALSE(p->canonical_key(show).has_value());
  CHECK_FALSE(p->supports_search());
  auto s = p->search("frieren", SearchOptions{});
  REQUIRE_FALSE(s.has_value());
  CHECK(s.error().kind == ProviderError::Kind::Unsupported);
}

namespace {

struct SequenceServer {
  int listen_fd = -1;
  std::uint16_t port = 0;
  std::thread thread;

  explicit SequenceServer(std::vector<std::vector<std::uint8_t>> responses) {
    listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listen_fd >= 0);
    int one = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    REQUIRE(::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    REQUIRE(::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    port = ntohs(addr.sin_port);
    REQUIRE(::listen(listen_fd, static_cast<int>(responses.size()) + 1) == 0);

    thread = std::thread([this, responses = std::move(responses)]() mutable {
      for (auto& resp : responses) {
        const int cfd = ::accept(listen_fd, nullptr, nullptr);
        if (cfd < 0) return;
        char buf[16384];
        (void)::read(cfd, buf, sizeof(buf));
        (void)::write(cfd, resp.data(), resp.size());
        ::close(cfd);
      }
    });
  }

  std::string url() const { return "http://127.0.0.1:" + std::to_string(port); }

  ~SequenceServer() {
    if (thread.joinable()) thread.join();
    if (listen_fd >= 0) ::close(listen_fd);
  }
};

std::vector<std::uint8_t> ok(std::string_view body) {
  std::string head = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                     std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
  std::vector<std::uint8_t> out(head.begin(), head.end());
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

constexpr const char* kSubOnly =
    R"({"success":true,"servers":[{"serverName":"HD-1","dataLink":"https://flixcloud.cc/e/a?v=1",)"
    R"("dataType":"sub","hardsub":false}]})";

}  // namespace

TEST_CASE("transport_resolve_sub_walks_flix_embed_and_token") {
  std::vector<std::vector<std::uint8_t>> r;
  r.push_back(ok(read_fixture("flix.json")));
  r.push_back(ok(read_fixture("embed_data.json")));
  r.push_back(ok(read_fixture("token_reply.json")));
  SequenceServer srv(std::move(r));
  auto p = ReAnime::with_endpoints(srv.url(), srv.url());
  REQUIRE(p.has_value());
  auto link = p->resolve("154587", "1", Translation::Sub, Quality::Best);
  REQUIRE(link.has_value());
  CHECK(link->url == kMaster);
  CHECK(link->referer == "https://flixcloud.cc/");
  REQUIRE(link->playlist_cipher.has_value());
  CHECK(link->playlist_cipher->kind == StreamLink::PlaylistCipher::Kind::Xor);
  CHECK(hex(link->playlist_cipher->key) == kPlaylistKey);
  CHECK(link->audio_language == "jpn");
  CHECK(link->sub_url == std::string(kSubsBase) + "_eng_3.ass");
  CHECK(link->cloaked_segments);
  CHECK_FALSE(link->decloak_segments);
}

TEST_CASE("transport_resolve_dub_takes_the_english_audio_and_signs") {
  std::vector<std::vector<std::uint8_t>> r;
  r.push_back(ok(read_fixture("flix.json")));
  r.push_back(ok(read_fixture("embed_data.json")));
  r.push_back(ok(read_fixture("token_reply.json")));
  SequenceServer srv(std::move(r));
  auto p = ReAnime::with_endpoints(srv.url(), srv.url());
  REQUIRE(p.has_value());
  auto link = p->resolve("154587", "1", Translation::Dub, Quality::Best);
  REQUIRE(link.has_value());
  CHECK(link->url == kMaster);
  CHECK(link->audio_language == "eng");
  CHECK(link->sub_url == std::string(kSubsBase) + "_eng_4.ass");
}

TEST_CASE("transport_resolve_dub_on_native_only_audio_fails_every_server") {
  std::string native = read_fixture("embed_data.json");
  native.replace(native.find("\"dual\""), 6, "\"native\"");
  std::vector<std::vector<std::uint8_t>> r;
  r.push_back(ok(read_fixture("flix.json")));
  r.push_back(ok(native));
  r.push_back(ok(native));
  SequenceServer srv(std::move(r));
  auto p = ReAnime::with_endpoints(srv.url(), srv.url());
  REQUIRE(p.has_value());
  auto link = p->resolve("154587", "1", Translation::Dub, Quality::Best);
  REQUIRE_FALSE(link.has_value());
  CHECK(link.error().detail == "no dub audio");
}

TEST_CASE("transport_episodes_probe_ep1_per_track") {
  {
    std::vector<std::vector<std::uint8_t>> r;
    r.push_back(ok(read_fixture("flix.json")));
    SequenceServer srv(std::move(r));
    auto p = ReAnime::with_endpoints(srv.url(), "");
    REQUIRE(p.has_value());
    auto eps = p->episodes("154587", Translation::Sub, 28);
    REQUIRE(eps.has_value());
    CHECK(eps->size() == 28);
  }
  {
    std::vector<std::vector<std::uint8_t>> r;
    r.push_back(ok(kSubOnly));
    SequenceServer srv(std::move(r));
    auto p = ReAnime::with_endpoints(srv.url(), "");
    REQUIRE(p.has_value());
    auto eps = p->episodes("10495", Translation::Dub, 12);
    REQUIRE(eps.has_value());
    CHECK(eps->empty());  // authoritative: no dub
  }
}

TEST_CASE("transport_resolve_without_servers_and_bad_input") {
  std::vector<std::vector<std::uint8_t>> r;
  r.push_back(ok(R"({"success":true,"servers":[]})"));
  SequenceServer srv(std::move(r));
  auto p = ReAnime::with_endpoints(srv.url(), srv.url());
  REQUIRE(p.has_value());
  auto none = p->resolve("154587", "99", Translation::Sub, Quality::Best);
  REQUIRE_FALSE(none.has_value());
  CHECK(none.error().detail == "no such episode");
  // Rejected before any request (the server above has no more answers).
  CHECK_FALSE(p->resolve("154587", "1.5", Translation::Sub, Quality::Best).has_value());
  CHECK_FALSE(p->resolve("154587", "0", Translation::Sub, Quality::Best).has_value());
  CHECK_FALSE(p->resolve("../1", "1", Translation::Sub, Quality::Best).has_value());
  CHECK_FALSE(p->episodes("0", Translation::Sub, 3).has_value());
}
