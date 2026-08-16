// memstream_compat.hpp — a FILE* whose writes accumulate into a std::string.
//
// Exists for one caller: the vendored SIXEL writer (sixel.cpp) hands its
// output to a FILE*, and we need a string to splice into the CellBuffer flush.
// Writing to a temp file and reading it back would put the cover path on the
// filesystem for no reason, so the stream is memory-backed.
//
// This is a platform seam, in the same family as term.cpp's poll()/select()
// split (§3):
//   - POSIX.1-2008 (Linux, modern *BSD): open_memstream(), which owns the
//     buffer and reports its length at fclose.
//   - macOS / *BSD: funopen(), a BSD callback stream. Old macOS — the 10.5/
//     10.6 PowerPC target — has NO open_memstream and NO fmemopen; funopen
//     has been there since well before 10.5 and is the portable choice across
//     every Darwin we care about, so all Apple builds take it.
// Both paths are plain C stdio underneath, so the writer sees an ordinary
// buffered FILE* either way.
//
// ENDIANNESS (§3): bytes in, bytes out; nothing is packed into a wider int.

#pragma once

#include <cstddef>
#include <cstdio>
#include <string>

// Darwin and the BSDs take funopen(); everything else open_memstream().
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
    defined(__OpenBSD__) || defined(__DragonFly__)
#define SHIGOKU_MEMSTREAM_FUNOPEN 1
#else
#define SHIGOKU_MEMSTREAM_FUNOPEN 0
#endif

namespace shigoku::tui {

// Opens the stream in the constructor; ok() reports whether that succeeded (a
// failed open leaves file() == nullptr rather than throwing — the cover path
// degrades to "no image this frame", never an exception across the flush).
// Neither copyable nor movable: the funopen path hands the callback a pointer
// to a member, so the object must not relocate.
class StringFile {
 public:
  StringFile();
  ~StringFile();
  StringFile(const StringFile&) = delete;
  StringFile& operator=(const StringFile&) = delete;
  StringFile(StringFile&&) = delete;
  StringFile& operator=(StringFile&&) = delete;

  [[nodiscard]] std::FILE* file() const { return fp_; }
  [[nodiscard]] bool ok() const { return fp_ != nullptr; }

  // Close the stream (flushing whatever stdio still buffers) and return
  // everything written to it. Idempotent: a second call returns the empty
  // string, and file() is nullptr from here on.
  [[nodiscard]] std::string take();

 private:
  std::FILE* fp_ = nullptr;
#if SHIGOKU_MEMSTREAM_FUNOPEN
  std::string sink_;  // the funopen cookie — do not let this object move.
#else
  char* buf_ = nullptr;    // owned by the stream until fclose, then by us.
  std::size_t len_ = 0;
#endif
};

}  // namespace shigoku::tui
