// stb_selftest.cpp — standalone big-endian sanity check for the cover decode
// pipeline (HW-2 diagnostic). Runs the exact stages Covers::load runs — stb
// JPEG/PNG decode to RGBA8, center-crop, sRGB resize — against a local image
// file, printing per-stage verdicts, a decode checksum, and channel means, and
// writing the resized result as out.ppm for a visual check.
//
// The JPEG decode is integer math, so the DECODE checksum must match the
// x86-64 reference for the same input file bit-for-bit; the resize goes
// through floats, so compare its output visually (out.ppm) not by checksum.
//
// Build (from the repo root; plain g++, no CMake, no other deps):
//   g++ -O1 -Ithird_party -o stb_selftest tools/stb_selftest.cpp
// Run:
//   ./stb_selftest /tmp/cover.jpg
//
// Compare the "decode fnv1a" line against the reference printed by the same
// tool on the Linux dev box for the same file (sha1-verified identical input).

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb/stb_image.h>
#include <stb/stb_image_resize2.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long fnv1a(const unsigned char* p, size_t n) {
  unsigned long long h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <image-file>\n", argv[0]);
    return 2;
  }

  FILE* f = fopen(argv[1], "rb");
  if (!f) {
    fprintf(stderr, "FAIL open: %s\n", argv[1]);
    return 1;
  }
  fseek(f, 0, SEEK_END);
  long fn = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char* body = (unsigned char*)malloc((size_t)fn);
  if (!body || fread(body, 1, (size_t)fn, f) != (size_t)fn) {
    fprintf(stderr, "FAIL read\n");
    return 1;
  }
  fclose(f);
  printf("input: %ld bytes, fnv1a=%016llx\n", fn, fnv1a(body, (size_t)fn));

  // Stage 1: header probe (decode_cover's stbi_info gate).
  int w = 0, h = 0, comp = 0;
  if (!stbi_info_from_memory(body, (int)fn, &w, &h, &comp)) {
    printf("FAIL stbi_info: %s\n", stbi_failure_reason());
    return 1;
  }
  printf("PASS stbi_info: %dx%d comp=%d\n", w, h, comp);

  // Stage 2: full decode to RGBA8 (decode_cover's stbi_load, forced 4ch).
  int ow = 0, oh = 0, oc = 0;
  unsigned char* px = stbi_load_from_memory(body, (int)fn, &ow, &oh, &oc, 4);
  if (!px) {
    printf("FAIL stbi_load: %s\n", stbi_failure_reason());
    return 1;
  }
  size_t n = (size_t)ow * (size_t)oh * 4u;
  unsigned long long sum = 0;
  for (size_t i = 0; i < n; ++i) sum += px[i];
  printf("PASS stbi_load: %dx%d  decode fnv1a=%016llx  mean=%.2f\n", ow, oh,
         fnv1a(px, n), (double)sum / (double)n);
  printf("  first pixels (RGBA): %u,%u,%u,%u  %u,%u,%u,%u\n", px[0], px[1],
         px[2], px[3], px[4], px[5], px[6], px[7]);

  // Stage 3: center-crop to the target aspect + sRGB resize, the same call
  // crop_and_resize makes. Box mirrors a typical detail-cover cell box.
  const int tw = 182, th = 270;
  unsigned long long cw64 = (unsigned long long)oh * tw / th;
  int cw, ch;
  if ((unsigned long long)ow * th > (unsigned long long)tw * oh) {
    cw = (int)(cw64 < 1 ? 1 : cw64);
    ch = oh;
  } else {
    cw = ow;
    unsigned long long ch64 = (unsigned long long)ow * th / tw;
    ch = (int)(ch64 < 1 ? 1 : ch64);
  }
  int x0 = (ow - cw) / 2, y0 = (oh - ch) / 2;
  unsigned char* crop = (unsigned char*)malloc((size_t)cw * ch * 4u);
  for (int y = 0; y < ch; ++y)
    memcpy(crop + (size_t)y * cw * 4u,
           px + ((size_t)(y0 + y) * ow + x0) * 4u, (size_t)cw * 4u);

  unsigned char* out = (unsigned char*)malloc((size_t)tw * th * 4u);
  unsigned char* r = stbir_resize_uint8_srgb(crop, cw, ch, 0, out, tw, th, 0,
                                             STBIR_RGBA);
  if (!r) {
    printf("FAIL stbir_resize\n");
    return 1;
  }
  size_t rn = (size_t)tw * th * 4u;
  sum = 0;
  for (size_t i = 0; i < rn; ++i) sum += out[i];
  printf("PASS stbir_resize: %dx%d  resize fnv1a=%016llx  mean=%.2f\n", tw, th,
         fnv1a(out, rn), (double)sum / (double)rn);

  // Stage 4: write out.ppm (RGB, alpha dropped) — open it to check the colors
  // look right (a channel-order or endian fault shows as wrong hues/garbage).
  FILE* o = fopen("out.ppm", "wb");
  if (o) {
    fprintf(o, "P6\n%d %d\n255\n", tw, th);
    for (size_t i = 0; i < rn; i += 4) fwrite(out + i, 1, 3, o);
    fclose(o);
    printf("wrote out.ppm (%dx%d) — open it and check the poster looks right\n",
           tw, th);
  }
  printf("ALL STAGES PASS\n");
  return 0;
}
