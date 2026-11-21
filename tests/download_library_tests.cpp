// download_library_tests.cpp — the download tree read back: scan_library and
// remove_local_episode over real directory trees under /tmp.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../src/download.hpp"
#include "../src/download_library.hpp"

using namespace shigoku;
using namespace shigoku::download;

namespace {

void wipe(const std::string& path) {
  struct stat st{};
  if (::lstat(path.c_str(), &st) != 0) return;
  if (!S_ISDIR(st.st_mode)) {
    ::unlink(path.c_str());
    return;
  }
  std::vector<std::string> names;
  if (DIR* d = ::opendir(path.c_str()); d != nullptr) {
    while (const struct dirent* e = ::readdir(d)) {
      const std::string_view name = e->d_name;
      if (name != "." && name != "..") names.emplace_back(name);
    }
    ::closedir(d);
  }
  for (const std::string& name : names) wipe(path + "/" + name);
  ::rmdir(path.c_str());
}

// An empty tree root in the OS tmpdir; whatever a prior run left there is gone.
std::string fresh_root(const char* name) {
  std::string root = "/tmp/shigoku-library-test-";
  root += std::to_string(static_cast<long>(::getpid()));
  root += "-";
  root += name;
  wipe(root);
  return root;
}

void make_dirs(const std::string& dir) {
  for (std::size_t i = 1; i <= dir.size(); ++i) {
    if (i != dir.size() && dir[i] != '/') continue;
    ::mkdir(dir.substr(0, i).c_str(), 0755);
  }
}

// mkdir -p the parent, then a file of exactly `bytes` bytes.
void plant(const std::string& path, std::size_t bytes) {
  make_dirs(path.substr(0, path.rfind('/')));
  FILE* f = std::fopen(path.c_str(), "wb");
  REQUIRE(f != nullptr);
  const std::string body(bytes, 'x');
  const std::size_t wrote = std::fwrite(body.data(), 1, body.size(), f);
  REQUIRE(std::fclose(f) == 0);
  REQUIRE(wrote == body.size());
}

bool exists(const std::string& path) {
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0;
}

LocalEpisode episode(std::string label, Translation translation, std::string path,
                     std::uint64_t bytes, bool partial = false) {
  LocalEpisode e;
  e.label = std::move(label);
  e.translation = translation;
  e.path = std::move(path);
  e.bytes = bytes;
  e.partial = partial;
  return e;
}

}  // namespace

TEST_CASE("scan_library: no root, a missing root and a file for a root are all empty") {
  CHECK(scan_library("").empty());
  const std::string root = fresh_root("missing");
  CHECK(scan_library(root).empty());
  CHECK(scan_library(root + "/deeper").empty());
  plant(root, 1);
  CHECK(scan_library(root).empty());
  wipe(root);
}

TEST_CASE("scan_library: show folders by name, each with its files sub first in episode order") {
  const std::string root = fresh_root("tree");

  const std::string show_a = root + "/1000";
  plant(show_a + "/sub/1.mp4", 3);
  plant(show_a + "/sub/2.mp4", 4);
  plant(show_a + "/sub/10.mp4", 5);
  plant(show_a + "/sub/12.5.mp4", 6);
  plant(show_a + "/sub/SP1.mkv", 7);
  plant(show_a + "/sub/OVA.mp4", 2);
  plant(show_a + "/sub/x.part", 1);
  plant(show_a + "/sub/.mp4", 1);
  make_dirs(show_a + "/sub/6.mp4");
  plant(show_a + "/dub/1.mp4", 8);
  plant(show_a + "/extras/3.mp4", 1);

  const std::string show_b = root + "/-42";
  plant(show_b + "/sub/3.mp4.part", 9);
  plant(show_b + "/sub/4.mp4", 2);
  plant(show_b + "/sub/4.mp4.part", 1);
  plant(show_b + "/sub/4.mkv.part", 1);
  plant(show_b + "/sub/5.mp4", 2);
  plant(show_b + "/sub/5.mkv", 3);

  plant(root + "/hianime-123/dub/1.mp4", 1);
  plant(root + "/.cache/sub/1.mp4", 1);
  plant(root + "/stray.mp4", 1);
  make_dirs(root + "/777/sub");

  const std::vector<LocalShow> shows = scan_library(root);
  REQUIRE(shows.size() == 3);

  // A .part alone is listed; one beside a finished file of its label is not,
  // and of two finished files for one label the smaller path is listed.
  const std::vector<LocalEpisode> expect_b = {
      episode("3", Translation::Sub, show_b + "/sub/3.mp4.part", 9, true),
      episode("4", Translation::Sub, show_b + "/sub/4.mp4", 2),
      episode("5", Translation::Sub, show_b + "/sub/5.mkv", 3),
  };
  CHECK(shows[0].folder == "-42");
  CHECK(shows[0].anilist_id == std::optional<std::int64_t>(-42));
  CHECK(shows[0].episodes == expect_b);

  // Episode order, not byte order; the two specials tie and go by label.
  const std::vector<LocalEpisode> expect_a = {
      episode("1", Translation::Sub, show_a + "/sub/1.mp4", 3),
      episode("2", Translation::Sub, show_a + "/sub/2.mp4", 4),
      episode("10", Translation::Sub, show_a + "/sub/10.mp4", 5),
      episode("12.5", Translation::Sub, show_a + "/sub/12.5.mp4", 6),
      episode("OVA", Translation::Sub, show_a + "/sub/OVA.mp4", 2),
      episode("SP1", Translation::Sub, show_a + "/sub/SP1.mkv", 7),
      episode("1", Translation::Dub, show_a + "/dub/1.mp4", 8),
  };
  CHECK(shows[1].folder == "1000");
  CHECK(shows[1].anilist_id == std::optional<std::int64_t>(1000));
  CHECK(shows[1].episodes == expect_a);

  const std::vector<LocalEpisode> expect_c = {
      episode("1", Translation::Dub, root + "/hianime-123/dub/1.mp4", 1),
  };
  CHECK(shows[2].folder == "hianime-123");
  CHECK_FALSE(shows[2].anilist_id.has_value());
  CHECK(shows[2].episodes == expect_c);

  // A trailing slash on the root changes nothing, and a listed path is the
  // one the play-prefers-local scan finds.
  CHECK(scan_library(root + "/") == shows);
  CHECK(find_local_episode(root, -42, Translation::Sub, "5") ==
        std::optional<std::string>(show_b + "/sub/5.mkv"));
  CHECK(find_local_episode(root, 1000, Translation::Sub, "12.5") ==
        std::optional<std::string>(show_a + "/sub/12.5.mp4"));

  wipe(root);
}

TEST_CASE("scan_library: a folder name is a catalogue id only when it is a whole int64") {
  const std::string root = fresh_root("ids");
  const char* const names[] = {
      "0", "-0", "007", "9223372036854775807", "-9223372036854775808",
      "9223372036854775808", "-9223372036854775809", "99999999999999999999",
      "-", "+5", "12a", "--5",
  };
  for (const char* name : names) plant(root + "/" + name + "/sub/1.mp4", 1);

  std::map<std::string, std::optional<std::int64_t>> ids;
  for (const LocalShow& show : scan_library(root)) ids.emplace(show.folder, show.anilist_id);
  REQUIRE(ids.size() == std::size(names));

  using Id = std::optional<std::int64_t>;
  CHECK(ids.at("0") == Id(0));
  CHECK(ids.at("-0") == Id(0));
  CHECK(ids.at("007") == Id(7));
  CHECK(ids.at("9223372036854775807") == Id(std::numeric_limits<std::int64_t>::max()));
  CHECK(ids.at("-9223372036854775808") == Id(std::numeric_limits<std::int64_t>::min()));
  CHECK_FALSE(ids.at("9223372036854775808").has_value());
  CHECK_FALSE(ids.at("-9223372036854775809").has_value());
  CHECK_FALSE(ids.at("99999999999999999999").has_value());
  CHECK_FALSE(ids.at("-").has_value());
  CHECK_FALSE(ids.at("+5").has_value());
  CHECK_FALSE(ids.at("12a").has_value());
  CHECK_FALSE(ids.at("--5").has_value());

  wipe(root);
}

TEST_CASE("remove_local_episode: the file goes, then the folders it leaves empty") {
  const std::string root = fresh_root("remove");
  plant(root + "/500/sub/1.mp4", 1);
  plant(root + "/500/sub/2.mp4.part", 1);
  plant(root + "/500/dub/1.mp4", 1);
  plant(root + "/600/sub/1.mp4", 1);

  std::vector<LocalShow> shows = scan_library(root);
  REQUIRE(shows.size() == 2);
  REQUIRE(shows[0].episodes.size() == 3);
  REQUIRE(shows[1].episodes.size() == 1);
  const LocalEpisode sub_1 = shows[0].episodes[0];
  const LocalEpisode sub_2 = shows[0].episodes[1];
  const LocalEpisode dub_1 = shows[0].episodes[2];
  const LocalEpisode lone = shows[1].episodes[0];
  CHECK(sub_2.partial);

  // One of two files: its folders stay.
  CHECK(remove_local_episode(sub_1));
  CHECK_FALSE(exists(sub_1.path));
  CHECK(exists(root + "/500/sub/2.mp4.part"));

  // The track's last file takes the track folder; the show keeps its dub.
  CHECK(remove_local_episode(sub_2));
  CHECK_FALSE(exists(root + "/500/sub"));
  CHECK(exists(root + "/500/dub/1.mp4"));

  // The show's last file takes the show folder, never the root.
  CHECK(remove_local_episode(dub_1));
  CHECK_FALSE(exists(root + "/500"));
  CHECK(exists(root));

  // Gone already is still gone.
  CHECK(remove_local_episode(dub_1));

  shows = scan_library(root);
  REQUIRE(shows.size() == 1);
  CHECK(shows[0].folder == "600");

  CHECK(remove_local_episode(lone));
  CHECK(remove_local_episode(lone));
  CHECK_FALSE(exists(root + "/600"));
  CHECK(exists(root));
  CHECK(scan_library(root).empty());

  wipe(root);
}

TEST_CASE("scan_library and remove_local_episode: a softsub goes with its episode") {
  const std::string root = fresh_root("subs");
  plant(root + "/500/sub/1.mp4", 4);
  plant(root + "/500/sub/1.ass", 2);
  plant(root + "/500/sub/1.vtt.part", 1);
  plant(root + "/500/sub/2.mp4", 4);
  plant(root + "/500/sub/2.vtt", 2);
  plant(root + "/500/sub/3.srt", 2);  // a subtitle alone: no episode.

  const std::vector<LocalShow> shows = scan_library(root);
  REQUIRE(shows.size() == 1);
  REQUIRE(shows[0].episodes.size() == 2);
  CHECK(shows[0].episodes[0].path == root + "/500/sub/1.mp4");
  CHECK_FALSE(shows[0].episodes[0].partial);
  CHECK(shows[0].episodes[1].path == root + "/500/sub/2.mp4");

  CHECK(remove_local_episode(shows[0].episodes[0]));
  CHECK_FALSE(exists(root + "/500/sub/1.mp4"));
  CHECK_FALSE(exists(root + "/500/sub/1.ass"));
  CHECK_FALSE(exists(root + "/500/sub/1.vtt.part"));
  CHECK(exists(root + "/500/sub/2.vtt"));
  CHECK(exists(root + "/500/sub/3.srt"));

  CHECK(remove_local_episode(shows[0].episodes[1]));
  CHECK_FALSE(exists(root + "/500/sub/2.vtt"));
  CHECK(exists(root + "/500/sub/3.srt"));
  CHECK(scan_library(root).empty());

  wipe(root);
}

TEST_CASE("remove_local_episode: a file that cannot be removed answers false") {
  const std::string root = fresh_root("remove-fail");
  plant(root + "/plain", 1);
  // A path through a regular file fails with ENOTDIR, not ENOENT.
  CHECK_FALSE(remove_local_episode(episode("1", Translation::Sub, root + "/plain/1.mp4", 1)));
  CHECK(exists(root + "/plain"));
  wipe(root);
}
