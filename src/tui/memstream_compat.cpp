// memstream_compat.cpp — the two halves of the string-backed FILE* seam. See
// memstream_compat.hpp for which platform takes which.

#include "memstream_compat.hpp"

#include <cstdlib>
#include <utility>

namespace shigoku::tui {

#if SHIGOKU_MEMSTREAM_FUNOPEN

namespace {

// funopen write callback: append to the std::string cookie. Returns the count
// consumed, which is always the whole request — a std::string append only
// fails by throwing, and a throw across C stdio would be undefined, so the
// allocation failure path is std::terminate via noexcept rather than an
// exception unwinding through fwrite.
int sink_write(void* cookie, const char* data, int len) noexcept {
  if (len <= 0) return 0;
  static_cast<std::string*>(cookie)->append(data, static_cast<std::size_t>(len));
  return len;
}

}  // namespace

StringFile::StringFile() {
  fp_ = ::funopen(&sink_, /*readfn=*/nullptr, &sink_write, /*seekfn=*/nullptr,
                  /*closefn=*/nullptr);
}

StringFile::~StringFile() {
  if (fp_ != nullptr) std::fclose(fp_);
}

std::string StringFile::take() {
  if (fp_ != nullptr) {
    std::fclose(fp_);  // flushes the last partial buffer into sink_.
    fp_ = nullptr;
  }
  std::string out = std::move(sink_);
  sink_.clear();  // a moved-from string is valid but unspecified; pin it empty.
  return out;
}

#else  // POSIX.1-2008 open_memstream

StringFile::StringFile() { fp_ = ::open_memstream(&buf_, &len_); }

StringFile::~StringFile() {
  if (fp_ != nullptr) std::fclose(fp_);
  std::free(buf_);
}

std::string StringFile::take() {
  if (fp_ != nullptr) {
    std::fclose(fp_);  // buf_/len_ are only final once the stream is closed.
    fp_ = nullptr;
  }
  std::string out;
  if (buf_ != nullptr) {
    out.assign(buf_, len_);
    std::free(buf_);
    buf_ = nullptr;
  }
  len_ = 0;
  return out;
}

#endif

}  // namespace shigoku::tui
