// manga_flow_tests.cpp — the read flow's mechanism half (pages
// fetch-to-cache over a loopback fixture server, the viewer argv table +
// report parse, config-lite) and the mapp state machine (pages/viewer event
// ordering, mark-read + undo transitions, the library view, store-backed
// progress, download-ahead and the update sweep) driven through tick() with
// hand-made events — no worker threads, no child processes.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../src/manga/mapp.hpp"
#include "../src/manga/mconfig.hpp"
#include "../src/manga/mstore.hpp"
#include "../src/manga/pages.hpp"
#include "../src/manga/viewer.hpp"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

using namespace shigoku;
using namespace shigoku::manga;

namespace {

// --- fixtures ---------------------------------------------------------------

std::string tmp_dir(const char* name) {
  std::string d = "/tmp/shigoku-manga-flow-";
  d += name;
  d += "-";
  d += std::to_string(::getpid());
  // Fresh per run: best-effort rm of stale page files from a prior crash.
  const int rc = ::system(("rm -rf " + d).c_str());
  (void)rc;
  return d;
}

bool exists(const std::string& p) {
  struct stat st{};
  return ::stat(p.c_str(), &st) == 0;
}

std::string http_ok(const std::string& body) {
  return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
         "\r\nConnection: close\r\n\r\n" + body;
}

std::string http_status(int code, const char* label) {
  std::string head = "HTTP/1.1 " + std::to_string(code) + " " + label +
                     "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
  return head;
}

// Serial fixture server: each accepted connection consumes the next scripted
// response (one request per connection — the client sends Connection: close
// semantics via our Connection: close replies). Closing the listen fd from
// the test thread unblocks accept and ends the loop.
struct PageServer {
  int listen_fd = -1;
  std::uint16_t port = 0;
  std::thread th;
  std::vector<std::string> responses;
  std::atomic<std::size_t> served{0};
  std::mutex mu;                       // guards requests (server thread vs test).
  std::vector<std::string> requests;   // raw request bytes, per connection.

  explicit PageServer(std::vector<std::string> rs) : responses(std::move(rs)) {
    listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listen_fd >= 0);
    const int one = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    REQUIRE(::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    REQUIRE(::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr),
                          &len) == 0);
    port = ntohs(addr.sin_port);
    REQUIRE(::listen(listen_fd, 8) == 0);
    th = std::thread([this]() {
      for (;;) {
        const int cfd = ::accept(listen_fd, nullptr, nullptr);
        if (cfd < 0) return;  // listen fd closed: shut down.
        char buf[2048];
        const ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
        if (n > 0) {
          std::lock_guard<std::mutex> lk(mu);
          requests.emplace_back(buf, static_cast<std::size_t>(n));
        }
        const std::size_t i = served.fetch_add(1);
        const std::string& resp =
            responses[i < responses.size() ? i : responses.size() - 1];
        (void)::send(cfd, resp.data(), resp.size(), MSG_NOSIGNAL);
        ::close(cfd);
      }
    });
  }
  ~PageServer() {
    // A bare close() does not unblock a thread parked in accept() on Linux;
    // shutdown() does (accept returns EINVAL), the dummy connect is the
    // belt-and-braces wake for kernels where it doesn't.
    ::shutdown(listen_fd, SHUT_RDWR);
    const int wake = ::socket(AF_INET, SOCK_STREAM, 0);
    if (wake >= 0) {
      sockaddr_in a{};
      a.sin_family = AF_INET;
      a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      a.sin_port = htons(port);
      (void)::connect(wake, reinterpret_cast<sockaddr*>(&a), sizeof(a));
      ::close(wake);
    }
    if (th.joinable()) th.join();
    ::close(listen_fd);
  }
  [[nodiscard]] std::string base() const {
    return "http://127.0.0.1:" + std::to_string(port);
  }
};

// A PageSet over the loopback server, one unit per filename (the shape a
// MangaSource::pages() hands the core — absolute URLs, name hints carried).
PageSet pages_for(const PageServer& s, const std::vector<std::string>& files,
                  const std::string& referer = "") {
  PageSet set;
  for (const std::string& f : files) {
    set.units.push_back(PageUnit{s.base() + "/data/cafebabe/" + f, referer, f});
  }
  return set;
}

std::string slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

}  // namespace

// --- chapter_dir / page_filename / listing ----------------------------------

TEST_CASE("chapter_dir layout is root/manga/chapter") {
  CHECK(chapter_dir("/data/chapters", "aaaa", "bbbb") ==
        "/data/chapters/aaaa/bbbb");
}

TEST_CASE("fs_name: one safe, human-readable segment") {
  CHECK(fs_name("One Piece") == "One Piece");
  CHECK(fs_name("Fate/stay night") == "Fate_stay night");   // '/' is the law.
  CHECK(fs_name("Re: Zero") == "Re_ Zero");                 // ':' (HFS+).
  CHECK(fs_name("a\\b\tc\nd") == "a_b c d");                // '\', ws collapse.
  CHECK(fs_name("  spaced   out  ") == "spaced out");
  CHECK(fs_name(".hack//SIGN") == "_.hack__SIGN");          // leading-dot shield.
  CHECK(fs_name("-flag") == "_-flag");
  CHECK(fs_name("dots...") == "dots");
  CHECK(fs_name("") == "untitled");
  CHECK(fs_name("///") == "___");
  CHECK(fs_name("週刊少年") == "週刊少年");  // UTF-8 kept.
  // 100-byte cap cuts on a codepoint boundary: byte 100 lands mid-'週'
  // (3-byte sequences) -> backs off to the boundary at 99.
  std::string long_jp;
  for (int i = 0; i < 40; ++i) long_jp += "週";
  const std::string cut = fs_name(long_jp);
  CHECK(cut.size() == 99);
  CHECK(cut == long_jp.substr(0, 99));
}

TEST_CASE("pad_chapter: leading integer run padded to 4") {
  CHECK(pad_chapter("5") == "0005");
  CHECK(pad_chapter("10.5") == "0010.5");
  CHECK(pad_chapter("1052") == "1052");
  CHECK(pad_chapter("12345") == "12345");   // wider than the pad: untouched.
  CHECK(pad_chapter("extra") == "extra");   // non-numeric passes through.
  CHECK(pad_chapter("") == "");
}

TEST_CASE("library_chapter_dir: title-keyed, id-anchored layout") {
  MdManga m;
  m.id = "a1b2c3d4-0000-4000-8000-000000000000";
  m.title = "Berserk";
  MdChapter ch;
  ch.id = "e5f6a7b8-0000-4000-8000-000000000000";
  ch.chapter = "364";
  ch.lang = "en";
  CHECK(library_chapter_dir("/data/library", m, ch) ==
        "/data/library/Berserk [a1b2c3d4]/ch 0364 [en e5f6a7b8]");
  ch.chapter = "";  // oneshot.
  CHECK(library_chapter_dir("/data/library", m, ch) ==
        "/data/library/Berserk [a1b2c3d4]/oneshot [en e5f6a7b8]");
  ch.chapter = "10.5";
  ch.lang = "";
  CHECK(library_chapter_dir("/data/library", m, ch) ==
        "/data/library/Berserk [a1b2c3d4]/ch 0010.5 [e5f6a7b8]");
  m.title = "Fate/Extra: CCC";  // the segment rides fs_name.
  CHECK(library_chapter_dir("/d", m, ch) ==
        "/d/Fate_Extra_ CCC [a1b2c3d4]/ch 0010.5 [e5f6a7b8]");
}

TEST_CASE("library_chapter_dir: hierarchical ids drop the manga prefix") {
  // Dynasty permalinks are "{series_slug}_{chapter}", so a raw first-8 anchor
  // would be the literal series slug for EVERY chapter — the four unnumbered
  // extras below would collide into one directory (mixed pages under one
  // .complete marker).
  MdManga m;
  m.id = "kase_san";
  m.title = "Kase-san";
  MdChapter ch;
  ch.lang = "en";
  ch.id = "kase_san_ch17";
  ch.chapter = "17";
  CHECK(library_chapter_dir("/lib", m, ch) ==
        "/lib/Kase-san [kase_san]/ch 0017 [en ch17]");
  ch.id = "kase_san_ch17_2";
  ch.chapter = "17.2";
  CHECK(library_chapter_dir("/lib", m, ch) ==
        "/lib/Kase-san [kase_san]/ch 0017.2 [en ch17_2]");
  ch.id = "kase_san_rival_and_kase_san";
  ch.chapter = "";
  CHECK(library_chapter_dir("/lib", m, ch) ==
        "/lib/Kase-san [kase_san]/oneshot [en rival_an]");
  ch.id = "kase_san_christmas_and_kase_san";
  CHECK(library_chapter_dir("/lib", m, ch) ==
        "/lib/Kase-san [kase_san]/oneshot [en christma]");
  // A chapter id that IS the manga id (never seen) keeps the id, not "".
  ch.id = "kase_san";
  CHECK(library_chapter_dir("/lib", m, ch) ==
        "/lib/Kase-san [kase_san]/oneshot [en kase_san]");
}

TEST_CASE("page_filename: 1-based zero-padded index + carried extension") {
  CHECK(detail::page_filename(0, "x1-abc.jpg") == "001.jpg");
  CHECK(detail::page_filename(11, "p.png") == "012.png");
  CHECK(detail::page_filename(999, "p.jpeg") == "1000.jpeg");  // pad overflow.
  CHECK(detail::page_filename(2, "noext") == "003.jpg");       // default ext.
}

TEST_CASE("list_pages: sorted finals only — no .part, no markers") {
  const std::string dir = tmp_dir("list");
  REQUIRE(detail::mkdir_p(dir).has_value());
  for (const char* n : {"002.jpg", "001.jpg", "003.jpg.part", ".complete",
                        ".report"}) {
    std::ofstream(dir + "/" + n) << "x";
  }
  const auto pages = list_pages(dir);
  REQUIRE(pages.size() == 2);
  CHECK(pages[0] == dir + "/001.jpg");
  CHECK(pages[1] == dir + "/002.jpg");
}

TEST_CASE("list_pages: .webp counts (Dynasty chapters are all webp)") {
  const std::string dir = tmp_dir("listwebp");
  REQUIRE(detail::mkdir_p(dir).has_value());
  for (const char* n : {"001.webp", "002.WEBP", "003.webp.part", "004.txt",
                        ".complete"}) {
    std::ofstream(dir + "/" + n) << "x";
  }
  const auto pages = list_pages(dir);
  REQUIRE(pages.size() == 2);  // an all-webp chapter must not read as empty.
  CHECK(pages[0] == dir + "/001.webp");
  CHECK(pages[1] == dir + "/002.WEBP");  // the ext compare is case-folded.
}

// --- fetch_chapter_pages over the loopback server ---------------------------

TEST_CASE("fresh chapter fetch: pages land atomically + .complete written") {
  PageServer srv({http_ok("AAA"), http_ok("BBBB")});
  const std::string dir = tmp_dir("fresh");
  std::vector<std::pair<std::uint32_t, std::uint32_t>> ticks;
  auto client = http::Client::create();
  REQUIRE(client.has_value());
  auto r = fetch_chapter_pages(
      *client, pages_for(srv, {"a.jpg", "b.png"}), dir,
      [&](std::uint32_t d, std::uint32_t t) { ticks.emplace_back(d, t); },
      nullptr);
  REQUIRE_MESSAGE(r.has_value(), (r.has_value() ? std::string() : r.error().detail));
  CHECK(*r == 2);
  CHECK(slurp(dir + "/001.jpg") == "AAA");
  CHECK(slurp(dir + "/002.png") == "BBBB");
  CHECK(chapter_complete(dir));
  CHECK(!exists(dir + "/001.jpg.part"));
  REQUIRE(ticks.size() == 2);
  CHECK(ticks[0] == std::pair<std::uint32_t, std::uint32_t>{1, 2});
  CHECK(ticks[1] == std::pair<std::uint32_t, std::uint32_t>{2, 2});
}

TEST_CASE("complete chapter short-circuits: zero requests, full progress") {
  PageServer srv({http_ok("X")});
  const std::string dir = tmp_dir("short");
  REQUIRE(detail::mkdir_p(dir).has_value());
  std::ofstream(dir + "/.complete");
  std::vector<std::uint32_t> dones;
  auto client = http::Client::create();
  REQUIRE(client.has_value());
  auto r = fetch_chapter_pages(
      *client, pages_for(srv, {"a.jpg", "b.jpg"}), dir,
      [&](std::uint32_t d, std::uint32_t) { dones.push_back(d); }, nullptr);
  REQUIRE(r.has_value());
  CHECK(*r == 2);
  CHECK(srv.served.load() == 0);
  REQUIRE(dones.size() == 1);
  CHECK(dones[0] == 2);
}

TEST_CASE("re-entry skips pages already on disk") {
  PageServer srv({http_ok("NEW")});
  const std::string dir = tmp_dir("reentry");
  REQUIRE(detail::mkdir_p(dir).has_value());
  std::ofstream(dir + "/001.jpg") << "OLD";
  auto client = http::Client::create();
  REQUIRE(client.has_value());
  auto r = fetch_chapter_pages(*client, pages_for(srv, {"a.jpg", "b.jpg"}),
                               dir, nullptr, nullptr);
  REQUIRE(r.has_value());
  CHECK(srv.served.load() == 1);          // only page 2 hit the wire.
  CHECK(slurp(dir + "/001.jpg") == "OLD");  // never re-fetched.
  CHECK(slurp(dir + "/002.jpg") == "NEW");
  CHECK(chapter_complete(dir));
}

TEST_CASE("404 rotates to a fresh grant immediately and resumes") {
  // Node A: page 1 ok, page 2 404s. Node B (post-rotation): page 2 ok.
  PageServer a({http_ok("ONE"), http_status(404, "Not Found")});
  PageServer b({http_ok("TWO")});
  const std::string dir = tmp_dir("rotate");
  auto client = http::Client::create();
  REQUIRE(client.has_value());
  int refetches = 0;
  auto r = fetch_chapter_pages(
      *client, pages_for(a, {"a.jpg", "b.jpg"}), dir, nullptr,
      [&]() -> Result<PageSet, ProviderError> {
        ++refetches;
        return pages_for(b, {"a.jpg", "b.jpg"});
      });
  REQUIRE_MESSAGE(r.has_value(), (r.has_value() ? std::string() : r.error().detail));
  CHECK(refetches == 1);
  CHECK(slurp(dir + "/001.jpg") == "ONE");
  CHECK(slurp(dir + "/002.jpg") == "TWO");
  CHECK(chapter_complete(dir));
}

TEST_CASE("second node failure fails the chapter, completed pages kept") {
  PageServer a({http_ok("ONE"), http_status(404, "Not Found"),
                http_status(404, "Not Found")});
  const std::string dir = tmp_dir("rotfail");
  auto client = http::Client::create();
  REQUIRE(client.has_value());
  auto r = fetch_chapter_pages(
      *client, pages_for(a, {"a.jpg", "b.jpg"}), dir, nullptr,
      [&]() -> Result<PageSet, ProviderError> {
        return pages_for(a, {"a.jpg", "b.jpg"});  // same dead node again.
      });
  REQUIRE(!r.has_value());
  CHECK(slurp(dir + "/001.jpg") == "ONE");  // the partial survives.
  CHECK(!chapter_complete(dir));
  CHECK(!exists(dir + "/002.jpg"));
  CHECK(!exists(dir + "/002.jpg.part"));  // failed page leaves no droppings.
}

// --- the multi-source seam: PageUnit referer, pick_source, adapters ---------

TEST_CASE("page units carry their referer onto the wire; none sent otherwise") {
  auto client = http::Client::create();
  REQUIRE(client.has_value());
  {
    PageServer srv({http_ok("A")});
    const std::string dir = tmp_dir("referer");
    auto r = fetch_chapter_pages(
        *client, pages_for(srv, {"a.jpg"}, "https://example.org/ch/1"), dir,
        nullptr, nullptr);
    REQUIRE(r.has_value());
    std::lock_guard<std::mutex> lk(srv.mu);
    REQUIRE(srv.requests.size() == 1);
    CHECK(srv.requests[0].find("Referer: https://example.org/ch/1") !=
          std::string::npos);
  }
  {
    PageServer srv({http_ok("B")});
    const std::string dir = tmp_dir("noreferer");
    auto r = fetch_chapter_pages(*client, pages_for(srv, {"a.jpg"}), dir,
                                 nullptr, nullptr);
    REQUIRE(r.has_value());
    std::lock_guard<std::mutex> lk(srv.mu);
    REQUIRE(srv.requests.size() == 1);
    CHECK(srv.requests[0].find("Referer:") == std::string::npos);
  }
}

namespace {

// The minimal MangaSource for registry tests: attrs only, ops are inert.
struct FakeSource final : MangaSource {
  std::string key_;
  std::string name_;
  bool nsfw_ = false;
  FakeSource(std::string k, std::string n, bool x = false)
      : key_(std::move(k)), name_(std::move(n)), nsfw_(x) {}
  [[nodiscard]] std::string_view key() const override { return key_; }
  [[nodiscard]] std::string_view name() const override { return name_; }
  [[nodiscard]] bool nsfw() const override { return nsfw_; }
  [[nodiscard]] Result<std::vector<MdManga>, ProviderError> search(
      std::string_view) const override {
    return std::vector<MdManga>{};
  }
  [[nodiscard]] Result<std::vector<MdChapter>, ProviderError> chapters(
      std::string_view, std::string_view) const override {
    return std::vector<MdChapter>{};
  }
  [[nodiscard]] Result<PageSet, ProviderError> pages(std::string_view,
                                                     bool) const override {
    return PageSet{};
  }
  [[nodiscard]] std::string cover_thumb_url(const MdManga&) const override {
    return {};
  }
};

}  // namespace

TEST_CASE("pick_source: key match, nsfw gate, first-eligible fallback") {
  FakeSource md("md", "MangaDex");
  FakeSource wc("wc", "WeebCentral");
  FakeSource nh("nh", "nhentai", /*nsfw=*/true);
  const std::vector<const MangaSource*> all = {&md, &wc, &nh};
  CHECK(pick_source(all, "wc", false) == &wc);
  CHECK(pick_source(all, "nh", false) == &md);  // gated: falls back, no match.
  CHECK(pick_source(all, "nh", true) == &nh);   // opted in.
  CHECK(pick_source(all, "zz", false) == &md);  // unknown key → first eligible.
  const std::vector<const MangaSource*> only_nh = {&nh};
  CHECK(pick_source(only_nh, "nh", false) == nullptr);  // fully gated list.
}

TEST_CASE("seam scoped_id prefixes the source key") {
  FakeSource wc("wc", "WeebCentral");
  CHECK(scoped_id(wc, "01J76XY7EF75DJNQCV04HTPDZK") ==
        "wc:01J76XY7EF75DJNQCV04HTPDZK");
}

TEST_CASE("MangaDex::pages adapts the at-home grant to absolute units") {
  const char* grant_json =
      R"({"baseUrl":"https://node.example","chapter":{"hash":"deadbeef",)"
      R"("data":["x1.jpg","x2.png"],"dataSaver":["s1.jpg","s2.jpg"]}})";
  {
    PageServer srv({http_ok(grant_json)});
    auto md = MangaDex::with_host(srv.base());
    REQUIRE(md.has_value());
    auto set = md->pages("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee", false);
    REQUIRE_MESSAGE(set.has_value(),
                    (set.has_value() ? std::string() : set.error().detail));
    REQUIRE(set->units.size() == 2);
    CHECK(set->units[0].url == "https://node.example/data/deadbeef/x1.jpg");
    CHECK(set->units[0].referer.empty());  // MangaDex@Home wants none.
    CHECK(set->units[0].name_hint == "x1.jpg");
    CHECK(set->units[1].url == "https://node.example/data/deadbeef/x2.png");
  }
  {
    PageServer srv({http_ok(grant_json)});
    auto md = MangaDex::with_host(srv.base());
    REQUIRE(md.has_value());
    auto set = md->pages("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee", true);
    REQUIRE(set.has_value());
    CHECK(set->units[0].url ==
          "https://node.example/data-saver/deadbeef/s1.jpg");
  }
  {
    // The saver-fallback law lives in the adapter now: an empty dataSaver
    // list serves the full-quality units even with data_saver on.
    const char* no_saver =
        R"({"baseUrl":"https://n2.example","chapter":{"hash":"deadbeef",)"
        R"("data":["x1.jpg"],"dataSaver":[]}})";
    PageServer srv({http_ok(no_saver)});
    auto md = MangaDex::with_host(srv.base());
    REQUIRE(md.has_value());
    auto set = md->pages("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee", true);
    REQUIRE(set.has_value());
    REQUIRE(set->units.size() == 1);
    CHECK(set->units[0].url == "https://n2.example/data/deadbeef/x1.jpg");
  }
}

// --- viewer: argv table / report / kind parse --------------------------------

TEST_CASE("build_viewer_argv: builtin carries the full flag surface") {
  ViewerOpts o;
  o.rtl = true;
  o.start_page = 7;
  o.report_file = "/d/.report";
  o.title = "T — ch 3";
  const auto argv = build_viewer_argv(ViewerKind::Builtin, "", "/bin/sv",
                                      "/d", {}, o);
  const std::vector<std::string> want = {"/bin/sv",        "--rtl",
                                         "--start-page",   "7",
                                         "--report-file",  "/d/.report",
                                         "--title",        "T — ch 3",
                                         "--",             "/d"};
  CHECK(argv == want);
}

TEST_CASE("build_viewer_argv: builtin default opts stay minimal") {
  const auto argv = build_viewer_argv(ViewerKind::Builtin, "", "shigoku-view",
                                      "/d", {}, ViewerOpts{});
  const std::vector<std::string> want = {"shigoku-view", "--", "/d"};
  CHECK(argv == want);
}

TEST_CASE("build_viewer_argv: preview opens the page files, custom gets the dir") {
  const auto prev = build_viewer_argv(ViewerKind::PreviewApp, "", "sv", "/d",
                                      {"/d/001.jpg", "/d/002.jpg"}, ViewerOpts{});
  const std::vector<std::string> want_prev = {"open", "-W", "-a", "Preview",
                                              "/d/001.jpg", "/d/002.jpg"};
  CHECK(prev == want_prev);

  const auto cust = build_viewer_argv(ViewerKind::Custom, "/usr/bin/feh", "sv",
                                      "/d", {"/d/001.jpg"}, ViewerOpts{});
  const std::vector<std::string> want_cust = {"/usr/bin/feh", "/d"};
  CHECK(cust == want_cust);

  // Unconfigured custom path degrades to builtin, never an empty argv[0].
  const auto degraded = build_viewer_argv(ViewerKind::Custom, "", "sv", "/d",
                                          {}, ViewerOpts{});
  CHECK(degraded[0] == "sv");
}

TEST_CASE("parse_viewer_kind degrades typos to builtin") {
  CHECK(parse_viewer_kind("preview") == ViewerKind::PreviewApp);
  CHECK(parse_viewer_kind("custom") == ViewerKind::Custom);
  CHECK(parse_viewer_kind("builtin") == ViewerKind::Builtin);
  CHECK(parse_viewer_kind("previeww") == ViewerKind::Builtin);
  CHECK(parse_viewer_kind("") == ViewerKind::Builtin);
}

TEST_CASE("parse_report_line: valid / garbage / bounds") {
  CHECK(parse_report_line("LAST_PAGE=12\n") == 12);
  CHECK(parse_report_line("LAST_PAGE=1") == 1);
  CHECK(!parse_report_line("LAST_PAGE=0").has_value());
  CHECK(!parse_report_line("LAST_PAGE=").has_value());
  CHECK(!parse_report_line("LAST_PAGE=12junk").has_value());
  CHECK(!parse_report_line("nonsense").has_value());
  CHECK(!parse_report_line("").has_value());
}

TEST_CASE("parse_report: missing file is nullopt") {
  CHECK(!parse_report("/nonexistent/shigoku/report").has_value());
}

TEST_CASE("resolve_builtin_viewer: bare argv0 stays a PATH lookup") {
  CHECK(resolve_builtin_viewer("shigoku-manga") == "shigoku-view");
  // A pathed argv0 whose sibling doesn't exist also falls back to PATH.
  CHECK(resolve_builtin_viewer("/nonexistent/dir/shigoku-manga") ==
        "shigoku-view");
}

// --- config-lite -------------------------------------------------------------

TEST_CASE("manga config: defaults, per-field degrade, round-trip") {
  const MangaConfig d;
  CHECK(parse_manga_config("") == d);           // not JSON → defaults.
  CHECK(parse_manga_config("[1,2]") == d);      // non-object → defaults.
  const MangaConfig partial =
      parse_manga_config(R"({"rtl": true, "viewer": 42, "chapter_lang": ""})");
  CHECK(partial.rtl == true);                   // good key applies…
  CHECK(partial.viewer == d.viewer);            // …ill-typed one degrades…
  CHECK(partial.chapter_lang == "en");          // …empty lang re-defaults.

  const std::string dir = tmp_dir("cfg");
  REQUIRE(detail::mkdir_p(dir).has_value());
  MangaConfig c;
  c.viewer = "custom";
  c.viewer_path = "/usr/bin/feh";
  c.rtl = true;
  c.data_saver = true;
  c.chapter_lang = "ja";
  c.source = "wc";
  c.nsfw_sources = true;
  REQUIRE(save_manga_config(dir + "/config.json", c).has_value());
  CHECK(load_manga_config(dir + "/config.json") == c);
  CHECK(load_manga_config(dir + "/absent.json") == d);

  // Registry-lite rows: empty source re-defaults; the nsfw gate defaults off.
  const MangaConfig src = parse_manga_config(R"({"source": ""})");
  CHECK(src.source == "md");
  CHECK(src.nsfw_sources == false);
}

// --- mapp state machine (tick-driven, no workers) ----------------------------

namespace {

MgApp& fresh_app() {
  static MgApp* app = nullptr;
  delete app;
  app = new MgApp();
  app->win = tui::WinSize{100, 30, 0, 0};
  return *app;
}

MdManga manga_row(const char* id, const char* title) {
  MdManga m;
  m.id = id;
  m.title = title;
  return m;
}

MdChapter chap(const char* id, const char* num, std::uint32_t pages) {
  MdChapter c;
  c.id = id;
  c.chapter = num;
  c.pages = pages;
  return c;
}

KeyEvent key_special(KeyEvent::Special s) {
  KeyEvent k;
  k.codepoint = 0;
  k.special = s;
  return k;
}

KeyEvent key_char(char32_t cp) {
  KeyEvent k;
  k.codepoint = cp;
  k.special = KeyEvent::Special::None;
  return k;
}

}  // namespace

TEST_CASE("chapters arrive → focus lands on the list; Esc walks back (HW #5/#6)") {
  MgApp& app = fresh_app();
  app.results = {manga_row("aaaa", "A")};
  app.cursor = 0;

  MgChaptersDone done;
  done.gen = app.chapters_gen.bump();
  done.manga_id = "aaaa";
  done.chapters = {chap("c1", "1", 10), chap("c2", "2", 12)};
  tick(app, MgEvent{done});
  CHECK(app.focus == MgFocus::Chapters);
  CHECK(app.chapter_cursor == 0);

  // j moves the CHAPTER cursor while the list is engaged.
  tick(app, MgEvent{key_char(U'j')});
  CHECK(app.chapter_cursor == 1);
  CHECK(app.cursor == 0);

  // Esc: back to the results list (the manga app's one back step).
  tick(app, MgEvent{key_special(KeyEvent::Special::Escape)});
  CHECK(app.focus == MgFocus::Results);
  tick(app, MgEvent{key_char(U'j')});
  CHECK(app.cursor == 0);  // single result: clamped, but routed to results.
  CHECK(app.chapter_cursor == 1);
}

TEST_CASE("pages fetch event ordering: progress → done clears the fetch") {
  MgApp& app = fresh_app();
  app.results = {manga_row("aaaa", "A")};
  app.chapters = {chap("c1", "1", 3)};
  app.chapters_for = "aaaa";
  app.pages_fetching = true;
  app.pages_chapter = "c1";
  const Generation gen = app.pages_gen.bump();

  tick(app, MgEvent{MgPagesProgress{gen, "c1", 1, 3}});
  CHECK(app.pages_done == 1);
  CHECK(app.pages_total == 3);
  CHECK(app.pages_fetching);

  // A stale generation's progress is dropped.
  tick(app, MgEvent{MgPagesProgress{gen - 1, "c1", 3, 3}});
  CHECK(app.pages_done == 1);

  // Done for a dir with no pages on disk: fetch cleared, viewer NOT opened
  // (open_viewer refuses an empty dir), no crash without deps.
  MgPagesDone d;
  d.gen = gen;
  d.chapter_id = "c1";
  d.dir = "/nonexistent/shigoku-pages";
  d.pages = 3;
  tick(app, MgEvent{d});
  CHECK(!app.pages_fetching);
  CHECK(app.open_viewers.empty());
}

TEST_CASE("pages failure surfaces a toast and clears the fetch") {
  MgApp& app = fresh_app();
  app.pages_fetching = true;
  const Generation gen = app.pages_gen.bump();
  tick(app, MgEvent{MgPagesFailed{gen, "c1", ProviderError::network()}});
  CHECK(!app.pages_fetching);
  CHECK(!app.toasts.empty());
}

TEST_CASE("viewer exit: read-to-end marks read, u undoes; mid-chapter resumes") {
  MgApp& app = fresh_app();

  // Mid-chapter exit → resume page recorded, nothing marked.
  MgViewerExited mid;
  mid.chapter_id = "c1";
  mid.total_pages = 10;
  mid.last_page = 4;
  app.open_viewers["c1"] = {};
  tick(app, MgEvent{mid});
  CHECK(app.open_viewers.empty());
  CHECK(app.read_chapters.empty());
  CHECK(app.resume_page.at("c1") == 4);

  // Read to the end → marked read, resume cleared, undo armed.
  MgViewerExited fin;
  fin.chapter_id = "c1";
  fin.total_pages = 10;
  fin.last_page = 10;
  app.open_viewers["c1"] = {};
  tick(app, MgEvent{fin});
  CHECK(app.read_chapters.count("c1") == 1);
  CHECK(app.resume_page.count("c1") == 0);
  REQUIRE(app.undo_read.has_value());

  // u undoes exactly once.
  tick(app, MgEvent{key_char(U'u')});
  CHECK(app.read_chapters.count("c1") == 0);
  CHECK(!app.undo_read.has_value());

  // A viewer spawn error is a toast, never a mark.
  MgViewerExited bad;
  bad.chapter_id = "c2";
  bad.error = "viewer not found: shigoku-view";
  app.open_viewers["c2"] = {};
  tick(app, MgEvent{bad});
  CHECK(app.open_viewers.empty());
  CHECK(app.read_chapters.count("c2") == 0);
}

TEST_CASE("two chapters can be read at once: each viewer's exit resolves its own pin") {
  MgApp& app = fresh_app();
  app.open_viewers["c1"].manga_scoped = "md:aaa";
  app.open_viewers["c2"].manga_scoped = "md:bbb";
  CHECK(app.open_viewers.size() == 2);

  // c2 finishes first (a shorter chapter, or just a faster reader) — c1
  // stays open, and its own pin is untouched.
  MgViewerExited c2done;
  c2done.chapter_id = "c2";
  c2done.total_pages = 5;
  c2done.last_page = 5;
  tick(app, MgEvent{c2done});
  CHECK(app.open_viewers.size() == 1);
  CHECK(app.open_viewers.count("c1") == 1);
  CHECK(app.read_chapters.count("c2") == 1);
  CHECK(app.read_chapters.count("c1") == 0);

  MgViewerExited c1mid;
  c1mid.chapter_id = "c1";
  c1mid.total_pages = 10;
  c1mid.last_page = 3;
  tick(app, MgEvent{c1mid});
  CHECK(app.open_viewers.empty());
  CHECK(app.resume_page.at("c1") == 3);
}

TEST_CASE("draw records the chapters plan; stale plans reset every frame") {
  MgApp& app = fresh_app();
  app.results = {manga_row("aaaa", "A")};
  app.chapters_for = "aaaa";
  app.focus = MgFocus::Chapters;
  for (int i = 0; i < 30; ++i) {
    app.chapters.push_back(chap(("c" + std::to_string(i)).c_str(),
                                std::to_string(i + 1).c_str(), 5));
  }
  tui::CellBuffer buf;
  draw(app, buf);
  REQUIRE(!app.chapters_plan.rect.empty());
  CHECK(app.chapters_plan.first_idx == 0);
  // Cursor deep in the list scrolls the window; the plan follows.
  app.chapter_cursor = 29;
  draw(app, buf);
  CHECK(app.chapters_plan.first_idx > 0);
  CHECK(app.chapters_plan.first_idx <= 29);

  // No chapters → no hit-testable rows.
  app.chapters.clear();
  app.chapters_for.clear();
  draw(app, buf);
  CHECK(app.chapters_plan.rect.empty());
}

// --- In-app source switching (`s` cycles eligible_sources) ------------------

TEST_CASE("eligible_sources honors the nsfw gate in registry order") {
  FakeSource md("md", "MangaDex");
  FakeSource wc("wc", "WeebCentral");
  FakeSource nh("nh", "nhentai", /*nsfw=*/true);
  const std::vector<const MangaSource*> all = {&md, &wc, &nh};
  CHECK(eligible_sources(all, false) ==
        std::vector<const MangaSource*>{&md, &wc});
  CHECK(eligible_sources(all, true) == all);
}

TEST_CASE("s cycles the source, re-arms search, persists the key") {
  FakeSource md("md", "MangaDex");
  FakeSource wc("wc", "WeebCentral");
  const std::string dir = tmp_dir("switch");
  REQUIRE(detail::mkdir_p(dir).has_value());

  MgApp& app = fresh_app();
  MgDeps deps;
  deps.source = &md;
  deps.sources = {&md, &wc};
  deps.config_path = dir + "/config.json";
  app.deps = &deps;

  // A held search surface for md…
  app.results = {manga_row("aaaa", "A")};
  app.last_query = "a";
  app.chapters = {chap("c1", "1", 10)};
  app.chapters_for = "aaaa";
  app.focus = MgFocus::Chapters;
  const Generation stale_search = app.search_gen.bump();  // "in flight".

  // …is torn down whole by the switch: ids don't cross sources.
  tick(app, MgEvent{key_char(U's')});
  CHECK(app.active_source == &wc);
  CHECK(app.results.empty());
  CHECK(app.last_query.empty());
  CHECK(app.chapters.empty());
  CHECK(app.chapters_for.empty());
  CHECK(app.focus == MgFocus::Results);

  // The in-flight search of the OLD source died by generation.
  MgSearchDone late;
  late.gen = stale_search;
  late.results = {manga_row("bbbb", "B")};
  tick(app, MgEvent{late});
  CHECK(app.results.empty());

  // The chosen key persisted (next boot starts on wc).
  CHECK(load_manga_config(deps.config_path).source == "wc");

  // Cycling wraps back around.
  tick(app, MgEvent{key_char(U's')});
  CHECK(app.active_source == &md);
  CHECK(load_manga_config(deps.config_path).source == "md");
}

TEST_CASE("s is inert without a second source; prompt keeps the letter") {
  FakeSource md("md", "MangaDex");
  MgApp& app = fresh_app();
  MgDeps deps;
  deps.source = &md;
  deps.sources = {&md};
  app.deps = &deps;
  tick(app, MgEvent{key_char(U's')});
  CHECK(app.active_source == nullptr);  // no cycle happened.

  // With the search prompt open, `s` is text, never a switch.
  deps.sources = {};
  app.prompt_open = true;
  tick(app, MgEvent{key_char(U's')});
  CHECK(app.prompt == "s");
  CHECK(app.active_source == nullptr);
}

// --- Library view + store-backed progress ------------------------------------

namespace {

// Every test here owns a blank in-memory manga.db.
MangaStore memory_store() {
  auto st = MangaStore::open_memory();
  REQUIRE(st.has_value());
  return std::move(*st);
}

MgChaptersDone feed(Generation gen, const char* manga_id, const char* scoped,
                    std::vector<MdChapter> chapters) {
  MgChaptersDone d;
  d.gen = gen;
  d.manga_id = manga_id;
  d.scoped_id = scoped;
  d.chapters = std::move(chapters);
  return d;
}

// The viewer-exit event the read flow posts (chapter read to the end when
// last == total).
MgViewerExited exited(const char* chapter_id, int last, std::uint32_t total) {
  MgViewerExited v;
  v.chapter_id = chapter_id;
  v.total_pages = total;
  v.last_page = last;
  return v;
}

}  // namespace

TEST_CASE("the library list owns the selection in MgView::Library") {
  MangaStore store = memory_store();
  REQUIRE(store.upsert_manga("md", manga_row("aaa", "Alpha"), 10).has_value());
  REQUIRE(store.upsert_manga("wc", manga_row("bbb", "Beta"), 20).has_value());
  REQUIRE(store.set_following("md:aaa", true, 10).has_value());
  REQUIRE(store.set_following("wc:bbb", true, 20).has_value());
  FakeSource md("md", "MangaDex");
  FakeSource wc("wc", "WeebCentral");

  MgApp& app = fresh_app();
  MgDeps deps;
  deps.source = &md;
  deps.sources = {&md, &wc};
  deps.store = &store;
  app.deps = &deps;
  app.results = {manga_row("zzz", "a search hit")};
  app.library = store.library().value();
  REQUIRE(app.library.size() == 2);
  app.view = MgView::Library;

  // Selection, motion and the detail pane all come from the library list.
  REQUIRE(selected(app) != nullptr);
  CHECK(selected(app)->title == (*app.library.begin()).manga.title);
  tick(app, MgEvent{key_char(U'j')});
  CHECK(app.library_cursor == 1);
  CHECK(app.cursor == 0);  // the search list is untouched underneath.
  CHECK(selected(app)->id == app.library[1].manga.id);

  // Rows carry their own source, so `s` (a search-view key) is inert here.
  tick(app, MgEvent{key_char(U's')});
  CHECK(app.active_source == nullptr);

  // L swaps back to the search list.
  tick(app, MgEvent{key_char(U'L')});
  CHECK(app.view == MgView::Search);
  REQUIRE(selected(app) != nullptr);
  CHECK(selected(app)->id == "zzz");
}

TEST_CASE("L refuses to open an empty library") {
  MangaStore store = memory_store();
  MgApp& app = fresh_app();
  MgDeps deps;
  deps.store = &store;
  app.deps = &deps;
  tick(app, MgEvent{key_char(U'L')});
  CHECK(app.view == MgView::Search);
  CHECK(!app.toasts.empty());
}

TEST_CASE("f follows the selection and seeds the chapter cache") {
  MangaStore store = memory_store();
  FakeSource md("md", "MangaDex");
  MgApp& app = fresh_app();
  MgDeps deps;
  deps.source = &md;
  deps.sources = {&md};
  deps.store = &store;
  app.deps = &deps;
  app.results = {manga_row("aaa", "Alpha")};
  app.chapters = {chap("c1", "1", 10), chap("c2", "2", 10)};
  app.chapters_for = "aaa";

  tick(app, MgEvent{key_char(U'f')});
  CHECK(store.is_following("md:aaa").value() == true);
  CHECK(app.library.size() == 1);
  // The visible feed seeds the cache so the unread counter is right at once.
  CHECK(store.cached_chapters("md:aaa").value().size() == 2);
  CHECK(app.library[0].unread() == 2);

  tick(app, MgEvent{key_char(U'f')});
  CHECK(store.is_following("md:aaa").value() == false);
  CHECK(app.library.empty());
  // Unfollowing keeps what was learned; re-following restores it.
  CHECK(store.cached_chapters("md:aaa").value().size() == 2);
}

TEST_CASE("read marks and resume pages survive a relaunch") {
  MangaStore store = memory_store();
  FakeSource md("md", "MangaDex");
  MgDeps deps;
  deps.source = &md;
  deps.sources = {&md};
  deps.store = &store;
  REQUIRE(store.upsert_manga("md", manga_row("aaa", "Alpha"), 10).has_value());
  const std::vector<MdChapter> chapters = {chap("c1", "1", 10), chap("c2", "2", 10),
                                           chap("c3", "3", 10)};

  {
    MgApp& app = fresh_app();
    app.deps = &deps;
    app.results = {manga_row("aaa", "Alpha")};
    tick(app, MgEvent{feed(app.chapters_gen.bump(), "aaa", "md:aaa", chapters)});
    CHECK(store.cached_chapters("md:aaa").value().size() == 3);
    CHECK(app.progress_for == "md:aaa");

    app.open_viewers["c1"].manga_scoped = "md:aaa";  // open_viewer pins this at spawn.
    tick(app, MgEvent{exited("c1", 10, 10)});   // read to the end.
    app.open_viewers["c2"].manga_scoped = "md:aaa";
    tick(app, MgEvent{exited("c2", 4, 10)});    // stopped on page 4.
    CHECK(app.read_chapters.count("c1") == 1);
    CHECK(app.resume_page.at("c2") == 4);
  }

  // "Relaunch": a brand-new app frame over the same DB.
  MgApp& next = fresh_app();
  next.deps = &deps;
  next.results = {manga_row("aaa", "Alpha")};
  tick(next, MgEvent{feed(next.chapters_gen.bump(), "aaa", "md:aaa", chapters)});
  CHECK(next.read_chapters.count("c1") == 1);
  CHECK(next.read_chapters.count("c2") == 0);
  CHECK(next.resume_page.at("c2") == 4);
  // The number rode along for the ordering column.
  CHECK(store.progress_row("md:aaa", "c1").value()->chapter == "1");
}

TEST_CASE("u restores the prior progress row exactly, store and screen") {
  MangaStore store = memory_store();
  FakeSource md("md", "MangaDex");
  MgDeps deps;
  deps.source = &md;
  deps.sources = {&md};
  deps.store = &store;
  REQUIRE(store.upsert_manga("md", manga_row("aaa", "Alpha"), 10).has_value());

  MgApp& app = fresh_app();
  app.deps = &deps;
  app.results = {manga_row("aaa", "Alpha")};
  tick(app, MgEvent{feed(app.chapters_gen.bump(), "aaa", "md:aaa",
                         {chap("c1", "1", 10)})});
  // Stop halfway, then finish it: undo must put the resume page back.
  app.open_viewers["c1"].manga_scoped = "md:aaa";
  tick(app, MgEvent{exited("c1", 4, 10)});
  app.open_viewers["c1"].manga_scoped = "md:aaa";
  tick(app, MgEvent{exited("c1", 10, 10)});
  CHECK(app.read_chapters.count("c1") == 1);
  REQUIRE(app.undo_read.has_value());
  CHECK(app.undo_read->had_row);

  tick(app, MgEvent{key_char(U'u')});
  CHECK(app.read_chapters.count("c1") == 0);
  CHECK(app.resume_page.at("c1") == 4);
  const auto row = store.progress_row("md:aaa", "c1").value();
  REQUIRE(row.has_value());
  CHECK(row->read == false);
  CHECK(row->last_page.value() == 4);
  CHECK(!app.undo_read.has_value());

  // A mark with NO prior row undoes by deleting it outright.
  app.open_viewers["c9"].manga_scoped = "md:aaa";
  tick(app, MgEvent{exited("c9", 5, 5)});
  REQUIRE(app.undo_read.has_value());
  CHECK(!app.undo_read->had_row);
  tick(app, MgEvent{key_char(U'u')});
  CHECK(!store.progress_row("md:aaa", "c9").value().has_value());
}

TEST_CASE("r continues after the highest read chapter, gaps and all") {
  MangaStore store = memory_store();
  FakeSource md("md", "MangaDex");
  MgDeps deps;
  deps.source = &md;
  deps.sources = {&md};
  deps.store = &store;
  REQUIRE(store.upsert_manga("md", manga_row("aaa", "Alpha"), 10).has_value());

  MgApp& app = fresh_app();
  app.deps = &deps;
  app.results = {manga_row("aaa", "Alpha")};
  tick(app, MgEvent{feed(app.chapters_gen.bump(), "aaa", "md:aaa",
                         {chap("c1", "1", 5), chap("c2", "2", 5),
                          chap("c3", "3", 5), chap("c4", "4", 5)})});
  REQUIRE(store.mark_read("md:aaa", "c1", "1", 100).has_value());
  REQUIRE(store.mark_read("md:aaa", "c3", "3", 200).has_value());
  app.progress_for.clear();                       // force a re-hydrate…
  tick(app, MgEvent{feed(app.chapters_gen.bump(), "aaa", "md:aaa",
                         {chap("c1", "1", 5), chap("c2", "2", 5),
                          chap("c3", "3", 5), chap("c4", "4", 5)})});
  REQUIRE(app.read_chapters.size() == 2);

  // Read 1 and 3 → continue at 4, not the gap at 2. (The read flow is not
  // wired in this frame, so open_chapter is a no-op: the pin is the cursor.)
  tick(app, MgEvent{key_char(U'r')});
  CHECK(app.focus == MgFocus::Chapters);
  CHECK(app.chapter_cursor == 3);

  // Nothing left → a toast, and the cursor stays where it was.
  REQUIRE(store.mark_read("md:aaa", "c4", "4", 300).has_value());
  app.read_chapters.insert("c4");
  app.chapter_cursor = 0;
  tick(app, MgEvent{key_char(U'r')});
  CHECK(app.chapter_cursor == 0);
  CHECK(!app.toasts.empty());
}

TEST_CASE("download-ahead queues the unread and skips what is on disk") {
  MangaStore store = memory_store();
  FakeSource md("md", "MangaDex");
  auto client = http::Client::create();
  REQUIRE(client.has_value());
  const std::string root = tmp_dir("dl");
  const MdManga m = manga_row("aaa", "Alpha");
  const std::vector<MdChapter> chapters = {chap("c1", "1", 3), chap("c2", "2", 3),
                                           chap("c3", "3", 3)};

  MgDeps deps;
  deps.source = &md;
  deps.sources = {&md};
  deps.store = &store;
  deps.pages_client = &client.value();
  deps.pages_root = root;

  // Every chapter already complete on disk: the queue drains with no fetch at
  // all (which is also what makes this test threadless).
  for (const MdChapter& c : chapters) {
    const std::string dir = library_chapter_dir(root, m, c);
    REQUIRE(detail::mkdir_p(dir).has_value());
    std::ofstream(dir + "/.complete").close();
  }

  MgApp& app = fresh_app();
  app.deps = &deps;
  app.results = {m};
  app.chapters = chapters;
  app.chapters_for = "aaa";
  app.read_chapters.insert("c1");  // read chapters are not re-downloaded.

  tick(app, MgEvent{key_char(U'D')});
  CHECK(!app.dl_active);
  CHECK(!app.pages_fetching);
  CHECK(app.dl_queue.empty());
  CHECK(!app.toasts.empty());

  // Nothing unread left → refused outright.
  app.read_chapters.insert("c2");
  app.read_chapters.insert("c3");
  tick(app, MgEvent{key_char(U'D')});
  CHECK(app.dl_queue.empty());
  CHECK(!app.dl_active);
}

TEST_CASE("a queued download advances the queue instead of opening the viewer") {
  MangaStore store = memory_store();
  FakeSource md("md", "MangaDex");
  auto client = http::Client::create();
  REQUIRE(client.has_value());
  const std::string root = tmp_dir("dl-advance");
  const MdManga m = manga_row("aaa", "Alpha");
  const MdChapter left = chap("c2", "2", 3);
  const std::string dir = library_chapter_dir(root, m, left);
  REQUIRE(detail::mkdir_p(dir).has_value());
  std::ofstream(dir + "/.complete").close();

  MgDeps deps;
  deps.source = &md;
  deps.sources = {&md};
  deps.store = &store;
  deps.pages_client = &client.value();
  deps.pages_root = root;

  MgApp& app = fresh_app();
  app.deps = &deps;
  app.results = {m};
  app.chapters = {chap("c1", "1", 3), left};
  app.chapters_for = "aaa";
  app.dl_manga = m;
  app.dl_source = &md;
  app.dl_queue = {left};
  app.dl_active = true;
  app.dl_total = 2;
  app.dl_done = 0;
  app.pages_fetching = true;

  MgPagesDone d;
  d.gen = app.pages_gen.bump();
  d.chapter_id = "c1";
  d.dir = dir;
  d.pages = 3;
  tick(app, MgEvent{d});
  CHECK(app.open_viewers.empty());   // a queued fetch never opens a reader.
  CHECK(!app.dl_active);     // c2 was already on disk: the queue drained.
  CHECK(app.dl_queue.empty());
  CHECK(!app.pages_fetching);
}

TEST_CASE("the update sweep diffs the cache and reports once") {
  MangaStore store = memory_store();
  FakeSource md("md", "MangaDex");
  MgDeps deps;
  deps.source = &md;
  deps.sources = {&md};
  deps.store = &store;
  REQUIRE(store.upsert_manga("md", manga_row("aaa", "Alpha"), 10).has_value());
  REQUIRE(store.set_following("md:aaa", true, 10).has_value());
  REQUIRE(store.put_chapters("md:aaa", {chap("c1", "1", 5), chap("c2", "2", 5)}, 10)
              .has_value());

  MgApp& app = fresh_app();
  app.deps = &deps;
  app.sweep_running = true;
  app.sweep_total = 1;
  const Generation gen = app.sweep_gen.bump();

  MgSweepChapters s;
  s.gen = gen;
  s.manga_id = "md:aaa";
  s.chapters = {chap("c1", "1", 5), chap("c2", "2", 5), chap("c3", "3", 5)};
  tick(app, MgEvent{s});
  CHECK(app.sweep_new == 1);
  CHECK(app.sweep_series == 1);
  CHECK(store.cached_chapters("md:aaa").value().size() == 3);

  tick(app, MgEvent{MgSweepFinished{gen, 1, 0}});
  CHECK(!app.sweep_running);
  CHECK(app.library.size() == 1);
  CHECK(app.library[0].unread() == 3);

  // A stale generation's answers are dropped whole.
  s.gen = gen - 1;
  s.chapters.push_back(chap("c4", "4", 5));
  tick(app, MgEvent{s});
  CHECK(store.cached_chapters("md:aaa").value().size() == 3);
}

TEST_CASE("U with nothing followed is a toast, not a sweep") {
  MangaStore store = memory_store();
  MgApp& app = fresh_app();
  MgDeps deps;
  deps.store = &store;
  app.deps = &deps;
  tick(app, MgEvent{key_char(U'U')});
  CHECK(!app.sweep_running);
  CHECK(!app.toasts.empty());
}

// --- AniList sync -----------------------------------------------------

namespace {

// The newest toast's copy — these cases are all about WHY a push was refused,
// and "a toast happened" does not say that.
std::string last_toast(const MgApp& app) {
  const std::vector<tui::Toast> v = app.toasts.visible();
  return v.empty() ? std::string{} : v.back().text;
}

MdManga tracked(const char* id, const char* title, std::int64_t al_id) {
  MdManga m = manga_row(id, title);
  m.al_id = al_id;
  return m;
}

MgSyncDone synced(const char* manga_id, MgSyncOutcome outcome, std::uint32_t progress) {
  MgSyncDone s;
  s.manga_id = manga_id;
  s.outcome = outcome;
  s.progress = progress;
  return s;
}

}  // namespace

TEST_CASE("S refuses a title with no anilist id, whatever else is wired") {
  FakeSource md("md", "MangaDex");
  MgApp& app = fresh_app();
  MgDeps deps;
  deps.source = &md;
  deps.sources = {&md};
  app.deps = &deps;
  app.results = {manga_row("aaa", "Alpha")};  // every source but MangaDex.

  tick(app, MgEvent{key_char(U'S')});
  CHECK_FALSE(app.syncing);
  CHECK(last_toast(app).find("no anilist id") != std::string::npos);
}

TEST_CASE("S says which gate stopped it: the switch, then the account") {
  FakeSource md("md", "MangaDex");
  auto client = http::Client::create();
  REQUIRE(client.has_value());

  MgApp& app = fresh_app();
  MgDeps deps;
  deps.source = &md;
  deps.sources = {&md};
  deps.sync_client = &client.value();
  deps.config.anilist_sync = false;
  app.deps = &deps;
  app.results = {tracked("aaa", "Alpha", 105778)};

  tick(app, MgEvent{key_char(U'S')});
  CHECK_FALSE(app.syncing);
  CHECK(last_toast(app).find("off in config") != std::string::npos);

  // Switch on, still signed out: the next gate, not a silent no-op.
  deps.config.anilist_sync = true;
  tick(app, MgEvent{key_char(U'S')});
  CHECK_FALSE(app.syncing);
  CHECK(last_toast(app).find("not connected") != std::string::npos);
}

TEST_CASE("S while a push is in flight waits rather than queueing a second") {
  FakeSource md("md", "MangaDex");
  auto client = http::Client::create();
  REQUIRE(client.has_value());

  MgApp& app = fresh_app();
  MgDeps deps;
  deps.source = &md;
  deps.sources = {&md};
  deps.sync_client = &client.value();
  app.deps = &deps;
  app.results = {tracked("aaa", "Alpha", 105778)};
  app.auth.anilist.access_token = "tok";
  app.auth.anilist.user_id = 7;
  app.syncing = true;  // one at a time: a chapter read is a single-row edit.

  tick(app, MgEvent{key_char(U'S')});
  CHECK(last_toast(app).find("already syncing") != std::string::npos);
}

TEST_CASE("a completed push moves the local high-water mark; a failed one does not") {
  MangaStore store = memory_store();
  REQUIRE(store.upsert_manga("md", tracked("aaa", "Alpha", 105778), 10).has_value());
  REQUIRE(store.set_following("md:aaa", true, 10).has_value());
  FakeSource md("md", "MangaDex");

  MgApp& app = fresh_app();
  MgDeps deps;
  deps.source = &md;
  deps.sources = {&md};
  deps.store = &store;
  app.deps = &deps;
  app.syncing = true;
  app.syncing_manga = "md:aaa";

  tick(app, MgEvent{synced("md:aaa", MgSyncOutcome::Pushed, 12)});
  CHECK_FALSE(app.syncing);
  CHECK(app.syncing_manga.empty());
  CHECK(store.al_synced("md:aaa").value() == 12);
  // The library row was reloaded, so the detail pane shows the new number.
  REQUIRE(app.library.size() == 1);
  CHECK(app.library[0].al_synced_chapter == 12);

  // A failure leaves the mark where it was: the chapter stays re-pushable.
  app.syncing = true;
  MgSyncDone failed = synced("md:aaa", MgSyncOutcome::PushFailed, 0);
  failed.cause = ProviderError::http(401);
  tick(app, MgEvent{failed});
  CHECK_FALSE(app.syncing);
  CHECK(store.al_synced("md:aaa").value() == 12);
  CHECK(last_toast(app).find("anilist") != std::string::npos);

  // "Already up to date" is an answer, not an error, and moves nothing.
  app.syncing = true;
  tick(app, MgEvent{synced("md:aaa", MgSyncOutcome::UpToDate, 0)});
  CHECK(store.al_synced("md:aaa").value() == 12);
}

TEST_CASE("the viewer exit pins the chapter the tracker would be told about") {
  MangaStore store = memory_store();
  REQUIRE(store.upsert_manga("md", tracked("aaa", "Alpha", 105778), 10).has_value());
  FakeSource md("md", "MangaDex");
  MgApp& app = fresh_app();
  MgDeps deps;
  deps.source = &md;
  deps.sources = {&md};
  deps.store = &store;
  app.deps = &deps;
  app.results = {tracked("aaa", "Alpha", 105778)};
  tick(app, MgEvent{feed(app.chapters_gen.bump(), "aaa", "md:aaa",
                         {chap("c1", "1", 10), chap("c2", "2", 10)})});

  // What open_viewer pins at spawn; the selection may move while reading.
  app.open_viewers["c2"] = {"md:aaa", "2", 105778, false};
  tick(app, MgEvent{exited("c2", 10, 10)});

  // Marked read locally with no account wired — the push simply never fires,
  // and nothing about the local read state depends on it.
  CHECK(app.read_chapters.count("c2") == 1);
  CHECK_FALSE(app.syncing);
  CHECK(store.al_synced("md:aaa").value() == 0);
  auto row = store.progress_row("md:aaa", "c2");
  REQUIRE(row.has_value());
  REQUIRE(row->has_value());
  CHECK((*row)->read);
  CHECK((*row)->chapter == "2");  // the pinned number, not the held list's.
}

TEST_CASE("the connect overlay captures the keyboard until Esc") {
  MgApp& app = fresh_app();
  app.results = {manga_row("aaa", "Alpha"), manga_row("bbb", "Beta")};
  bool canceled = false;
  MgApp::MgConnectSession session;
  session.url = "https://anilist.co/api/v2/oauth/authorize";
  session.cancel = [&canceled]() { canceled = true; };
  app.connect = std::move(session);

  // Motion, quit and every other key are ignored behind the modal.
  tick(app, MgEvent{key_char(U'j')});
  CHECK(app.cursor == 0);
  tick(app, MgEvent{key_char(U'q')});
  CHECK_FALSE(app.quit);

  tick(app, MgEvent{key_special(KeyEvent::Special::Escape)});
  CHECK_FALSE(app.connect.has_value());
  CHECK(canceled);
}

TEST_CASE("A with no auth file wired is a toast, not a half-open modal") {
  MgApp& app = fresh_app();
  MgDeps deps;  // bare frame: no client, no auth.json.
  app.deps = &deps;
  tick(app, MgEvent{key_char(U'A')});
  CHECK_FALSE(app.connect.has_value());
  CHECK(last_toast(app).find("connect unavailable") != std::string::npos);
}
