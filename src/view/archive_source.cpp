// archive_source.cpp — the libarchive half of the archive seam. One
// straight pass over the archive's headers: a non-regular or non-image entry
// is skipped without ever calling archive_read_data on it; an image entry is
// streamed to disk in 64 KiB chunks under its sanitized (and, on a
// collision, disambiguated) name via archive_names.hpp. No other TU in the
// viewer knows libarchive exists.

#include "archive_source.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "archive_names.hpp"
#include "pager.hpp"

namespace shigoku::view {

namespace fs = std::filesystem;

namespace {

// RAII around one archive_read* handle.
struct ReadHandle {
  archive* a = archive_read_new();
  ReadHandle() = default;
  ~ReadHandle() {
    if (a != nullptr) archive_read_free(a);
  }
  ReadHandle(const ReadHandle&) = delete;
  ReadHandle& operator=(const ReadHandle&) = delete;
};

void remove_dir(const std::string& dir) {
  std::error_code ec;
  fs::remove_all(dir, ec);  // best effort; nothing more to do if it fails.
}

// mkdtemp() of temp_directory_path()/"shigoku-view.XXXXXX". Empty string on
// failure (a missing or unwritable system temp dir).
std::string make_temp_dir() {
  std::error_code ec;
  const fs::path base = fs::temp_directory_path(ec) / "shigoku-view.XXXXXX";
  if (ec) return std::string();
  std::string tmpl = base.string();
  if (mkdtemp(tmpl.data()) == nullptr) return std::string();  // in place.
  return tmpl;
}

std::string why_from(archive* a, const char* fallback) {
  const char* msg = archive_error_string(a);
  return msg != nullptr ? std::string(msg) : std::string(fallback);
}

bool looks_encrypted(const std::string& why) {
  return why.find("assphrase") != std::string::npos ||
        why.find("ncrypt") != std::string::npos;
}

}  // namespace

ArchiveSource::~ArchiveSource() {
  if (!dir_.empty()) remove_dir(dir_);
}

ArchiveSource::ArchiveSource(ArchiveSource&& other) noexcept
    : dir_(std::move(other.dir_)), files_(std::move(other.files_)) {
  other.dir_.clear();
}

ArchiveSource& ArchiveSource::operator=(ArchiveSource&& other) noexcept {
  if (this != &other) {
    if (!dir_.empty()) remove_dir(dir_);
    dir_ = std::move(other.dir_);
    files_ = std::move(other.files_);
    other.dir_.clear();
  }
  return *this;
}

Result<ArchiveSource, std::string> ArchiveSource::extract(const std::string& path) {
  const std::string dir = make_temp_dir();
  if (dir.empty()) return err(path + ": cannot create a temporary directory");

  ReadHandle rd;
  if (rd.a == nullptr) {
    remove_dir(dir);
    return err(path + ": cannot allocate a libarchive reader");
  }
  archive_read_support_filter_all(rd.a);
  archive_read_support_format_all(rd.a);
  if (archive_read_open_filename(rd.a, path.c_str(), /*block_size=*/10240) != ARCHIVE_OK) {
    const std::string why = why_from(rd.a, "cannot open archive");
    remove_dir(dir);
    return err(path + ": " + why);
  }

  // From here on `src` owns `dir`: its destructor removes the temp directory
  // on every remaining return below, success or not.
  ArchiveSource src;
  src.dir_ = dir;
  if (archive_read_has_encrypted_entries(rd.a) > 0)
    return err(path + ": encrypted archive, not supported");

  std::unordered_map<std::string, int> counts;
  std::vector<char> chunk(64 * 1024);

  for (;;) {
    archive_entry* entry = nullptr;
    const int hr = archive_read_next_header(rd.a, &entry);
    if (hr == ARCHIVE_EOF) break;
    if (hr == ARCHIVE_FATAL || hr == ARCHIVE_FAILED) {
      const std::string why = why_from(rd.a, "cannot read the next entry");
      return err(path + ": " +
                 (looks_encrypted(why) ? "encrypted archive, not supported" : why));
    }
    if (archive_read_has_encrypted_entries(rd.a) > 0)
      return err(path + ": encrypted archive, not supported");

    const char* raw = archive_entry_pathname(entry);
    const bool regular = archive_entry_filetype(entry) == AE_IFREG;
    const std::optional<std::string> sanitized =
        raw != nullptr ? sanitize_entry_name(raw) : std::nullopt;
    if (!regular || !sanitized.has_value() || !is_image_ext(*sanitized)) {
      archive_read_data_skip(rd.a);
      continue;
    }

    int& count = counts[*sanitized];
    ++count;
    const std::string final_name = disambiguate(*sanitized, count);
    const fs::path out_path = fs::path(src.dir_) / final_name;

    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out)
      return err(path + ": " + final_name + ": cannot create the extracted file");
    for (;;) {
      const la_ssize_t n = archive_read_data(rd.a, chunk.data(), chunk.size());
      if (n < 0)
        return err(path + ": " + final_name + ": " + why_from(rd.a, "read error"));
      if (n == 0) break;
      out.write(chunk.data(), static_cast<std::streamsize>(n));
    }
    out.close();
    src.files_.push_back(out_path.string());
  }

  if (src.files_.empty()) return err(path + ": no pages found in archive");
  std::sort(src.files_.begin(), src.files_.end(), natural_less);
  return src;
}

}  // namespace shigoku::view
