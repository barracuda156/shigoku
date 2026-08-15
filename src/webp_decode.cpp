#include "webp_decode.hpp"

#ifdef HAVE_WEBP
#include <webp/decode.h>
#endif

namespace shigoku {

bool looks_like_webp(const std::uint8_t* data, std::size_t len) {
  if (data == nullptr || len < 12) return false;
  return data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' &&
         data[8] == 'W' && data[9] == 'E' && data[10] == 'B' && data[11] == 'P';
}

#ifdef HAVE_WEBP

bool webp_info(const std::uint8_t* data, std::size_t len, int* w, int* h) {
  return WebPGetInfo(data, len, w, h) != 0;
}

std::uint8_t* decode_webp(const std::uint8_t* data, std::size_t len, int* out_w,
                          int* out_h) {
  return WebPDecodeRGBA(data, len, out_w, out_h);
}

void free_webp_pixels(std::uint8_t* pixels) { WebPFree(pixels); }

#endif  // HAVE_WEBP

}  // namespace shigoku
