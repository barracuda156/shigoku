// archive_source.hpp — extract a comic archive (CBZ/CBR/CBT) once into a
// fresh temp directory of sanitized page images, then hand the sorted file
// list to the existing image pager + PageCache: the archive format never
// touches the render path, and this is the only piece of the viewer that
// knows libarchive exists.
//
// Compiled only when the build found libarchive (SHIGOKU_HAVE_LIBARCHIVE in
// CMake, HAVE_LIBARCHIVE in the code).

#pragma once

#include <string>
#include <vector>

#include "../result.hpp"

namespace shigoku::view {

class ArchiveSource {
 public:
  // Extract every image entry of `path` into a fresh temp directory
  // (mkdtemp under the system temp dir), sanitizing and deduping entry names
  // on the way (archive_names.hpp — no '/' survives sanitize_entry_name, so
  // no entry can ever be written outside that directory). Err covers: an
  // unreadable or unrecognised archive, an encrypted one ("encrypted
  // archive, not supported"), a read failure naming the entry, or "no pages
  // found in archive" when nothing image-shaped was inside — the temp
  // directory is removed again before any Err return.
  [[nodiscard]] static Result<ArchiveSource, std::string> extract(
      const std::string& path);

  // Removes the temp directory and everything extracted into it (best
  // effort; a SIGKILL between here and there leaves it — documented, not
  // guarded against).
  ~ArchiveSource();
  ArchiveSource(ArchiveSource&&) noexcept;
  ArchiveSource& operator=(ArchiveSource&&) noexcept;
  ArchiveSource(const ArchiveSource&) = delete;
  ArchiveSource& operator=(const ArchiveSource&) = delete;

  // The extracted pages' absolute paths, natural_less-sorted — exactly the
  // shape build_page_list() hands the image pager, so the Archive arm falls
  // straight into the existing decode/prefetch/scale path.
  [[nodiscard]] const std::vector<std::string>& files() const { return files_; }

 private:
  ArchiveSource() = default;

  std::string dir_;
  std::vector<std::string> files_;
};

}  // namespace shigoku::view
