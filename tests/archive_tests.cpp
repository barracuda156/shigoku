// archive_tests.cpp — ArchiveSource against the committed probe.cbz/.cbt
// fixtures: the sanitize/dedupe rule under a real libarchive read, the
// zip-slip fence, the temp directory's lifetime, and the error taxonomy
// (truncated file, wrong content). The pure archive_names tables (the
// naming rule in isolation) live in viewer_tests; this binary only proves
// the real extraction agrees with them.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "../src/view/archive_source.hpp"

using namespace shigoku;
using namespace shigoku::view;

namespace {

namespace fs = std::filesystem;

const std::string kDocs = std::string(SHIGOKU_TEST_FIXTURES_DIR) + "/docs";
const std::string kCbz = kDocs + "/probe.cbz";
const std::string kCbt = kDocs + "/probe.cbt";

// Both fixtures share the same six entries (gen_docs.py's ARCHIVE_ENTRIES),
// so extraction must agree on the same five image pages either way — the
// point of the .cbt fixture is that this list does not depend on the
// container format.
const std::vector<std::string> kExpected = {
    "ch1_002.png", "ch1_002~2.png", "ch1_010.png", "empty.png", "evil.png"};

ArchiveSource must_extract(const std::string& path) {
  auto opened = ArchiveSource::extract(path);
  REQUIRE_MESSAGE(opened.has_value(), (opened ? std::string{} : opened.error()));
  return std::move(*opened);
}

std::vector<std::string> basenames(const std::vector<std::string>& paths) {
  std::vector<std::string> out;
  out.reserve(paths.size());
  for (const auto& p : paths) out.push_back(fs::path(p).filename().string());
  return out;
}

// A scratch directory for the damaged files the error cases need; generated
// rather than committed, so the tree carries no deliberately broken archive.
fs::path scratch_dir() {
  const fs::path dir = fs::temp_directory_path() / "shigoku_archive_tests";
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

void write_file(const fs::path& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("probe_cbz_extracts_exactly_the_expected_sorted_basenames") {
  ArchiveSource src = must_extract(kCbz);
  CHECK(basenames(src.files()) == kExpected);
}

TEST_CASE("probe_cbt_extracts_the_same_basenames_the_format_is_agnostic") {
  ArchiveSource src = must_extract(kCbt);
  CHECK(basenames(src.files()) == kExpected);
}

TEST_CASE("evil_png_lands_inside_the_temp_directory_not_above_it") {
  ArchiveSource src = must_extract(kCbz);
  REQUIRE_FALSE(src.files().empty());
  const fs::path dir = fs::path(src.files().front()).parent_path();
  for (const auto& f : src.files()) {
    CAPTURE(f);
    CHECK(fs::path(f).parent_path() == dir);
  }
  const std::vector<std::string> names = basenames(src.files());
  CHECK(std::find(names.begin(), names.end(), "evil.png") != names.end());  // ../evil.png, sanitized flat.
  CHECK(fs::exists(dir / "evil.png"));
}

TEST_CASE("the_dedupe_suffix_is_present_for_the_colliding_entry") {
  ArchiveSource src = must_extract(kCbz);
  const std::vector<std::string> names = basenames(src.files());
  CHECK(std::find(names.begin(), names.end(), "ch1_002~2.png") != names.end());
}

TEST_CASE("notes_txt_is_absent_it_is_not_an_image") {
  ArchiveSource src = must_extract(kCbz);
  for (const auto& f : src.files()) CHECK(fs::path(f).filename() != "notes.txt");
}

TEST_CASE("the_temp_directory_is_removed_once_the_source_is_destroyed") {
  fs::path dir;
  {
    ArchiveSource src = must_extract(kCbz);
    REQUIRE_FALSE(src.files().empty());
    dir = fs::path(src.files().front()).parent_path();
    CHECK(fs::exists(dir));
  }
  CHECK_FALSE(fs::exists(dir));
}

TEST_CASE("a_truncated_cbz_is_an_error") {
  const fs::path dir = scratch_dir();
  const std::string full = read_file(kCbz);
  REQUIRE(full.size() > 20);
  const fs::path broken = dir / "truncated.cbz";
  write_file(broken, full.substr(0, full.size() / 2));

  auto opened = ArchiveSource::extract(broken.string());
  CHECK_FALSE(opened.has_value());
}

TEST_CASE("a_cbz_that_is_actually_a_png_is_an_error") {
  const fs::path dir = scratch_dir();
  const std::string png = read_file(kDocs + "/../cover_2x3.png");
  REQUIRE_FALSE(png.empty());
  const fs::path fake = dir / "fake.cbz";
  write_file(fake, png);

  auto opened = ArchiveSource::extract(fake.string());
  CHECK_FALSE(opened.has_value());
}
