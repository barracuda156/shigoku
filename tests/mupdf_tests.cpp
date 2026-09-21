// mupdf_tests.cpp — MupdfSource against the committed document fixtures.
//
// Two kinds of assertion live here, and the difference matters. The PDF
// fixture is flat fills and no fonts, so its geometry and its pixels are
// exact and pinned. The EPUB goes through a text layout engine whose line
// breaking is free to change between mupdf releases, so everything about it
// is asserted as a RELATION — more pages in a smaller window, the same
// window twice gives the same count, a reading position lands in the same
// chapter — never as a golden number.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "../src/view/docsrc.hpp"
#include "../src/view/mupdf_source.hpp"
#include "../src/view/pager.hpp"

using namespace shigoku;
using namespace shigoku::view;

namespace {

const std::string kDocs = std::string(SHIGOKU_TEST_FIXTURES_DIR) + "/docs";
const std::string kPdf = kDocs + "/two_pages.pdf";
const std::string kEpub = kDocs + "/reflow.epub";

// The fixture PDF's two pages are 200x300 pt, which is 267x400 px at 96 dpi.
constexpr int kPdfW = 267;
constexpr int kPdfH = 400;

std::unique_ptr<MupdfSource> must_open(const std::string& path, int w, int h) {
  auto opened = MupdfSource::open(path, w, h);
  REQUIRE_MESSAGE(opened.has_value(), (opened ? std::string{} : opened.error()));
  return std::move(*opened);
}

RgbaImage must_render(MupdfSource& doc, int page, int w, int h) {
  auto img = doc.render(page, w, h);
  REQUIRE_MESSAGE(img.has_value(), (img ? std::string{} : img.error()));
  return std::move(*img);
}

// One channel of one pixel; `c` is 0=R 1=G 2=B 3=A.
int channel(const RgbaImage& img, int x, int y, int c) {
  const std::size_t off =
      ((static_cast<std::size_t>(y) * static_cast<std::size_t>(img.w)) +
       static_cast<std::size_t>(x)) * 4 + static_cast<std::size_t>(c);
  REQUIRE(off < img.rgba.size());
  return img.rgba[off];
}

void check_pixel(const RgbaImage& img, int x, int y, int r, int g, int b) {
  CHECK(channel(img, x, y, 0) == r);
  CHECK(channel(img, x, y, 1) == g);
  CHECK(channel(img, x, y, 2) == b);
  CHECK(channel(img, x, y, 3) == 255);  // alpha=0 rendering: always opaque.
}

// A scratch directory for the damaged files the error cases need; they are
// generated rather than committed so the tree carries no deliberately broken
// documents.
std::filesystem::path scratch_dir() {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "shigoku_mupdf_tests";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

void write_file(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

}  // namespace

// ===========================================================================
// A fixed layout: the file owns its pages
// ===========================================================================

TEST_CASE("a_fixed_layout_document_reports_the_pages_the_file_has") {
  auto doc = must_open(kPdf, 600, 800);
  CHECK_FALSE(doc->reflowable());
  CHECK(doc->page_count() == 2);
  CHECK(doc->page_size(0).w == kPdfW);
  CHECK(doc->page_size(0).h == kPdfH);
  CHECK(doc->page_size(1).w == kPdfW);
  CHECK(doc->page_size(1).h == kPdfH);
  // A page that does not exist has no size rather than a made-up one.
  CHECK(doc->page_size(-1).w == 0);
  CHECK(doc->page_size(2).w == 0);
}

TEST_CASE("a_page_renders_at_exactly_the_size_asked_for") {
  auto doc = must_open(kPdf, 600, 800);

  const RgbaImage one = must_render(*doc, 0, kPdfW, kPdfH);
  CHECK(one.w == kPdfW);
  CHECK(one.h == kPdfH);
  CHECK(one.rgba.size() ==
        static_cast<std::size_t>(one.w) * static_cast<std::size_t>(one.h) * 4);
  check_pixel(one, one.w / 2, one.h / 2, 255, 0, 0);  // the red fill.
  check_pixel(one, 2, 2, 255, 255, 255);              // the paper around it.
  check_pixel(one, one.w - 3, one.h - 3, 255, 255, 255);

  const RgbaImage two = must_render(*doc, 1, kPdfW, kPdfH);
  check_pixel(two, two.w / 2, two.h / 2, 0, 0, 255);  // the blue fill.

  // Vector art, so a zoom is a bigger rasterisation, not a scaled bitmap:
  // twice the request is twice the pixels, still sharp-edged.
  const RgbaImage big = must_render(*doc, 0, kPdfW * 2, kPdfH * 2);
  CHECK(big.w == kPdfW * 2);
  CHECK(big.h == kPdfH * 2);
  check_pixel(big, big.w / 2, big.h / 2, 255, 0, 0);
}

TEST_CASE("a_fixed_layout_document_ignores_a_relayout") {
  auto doc = must_open(kPdf, 600, 800);
  const Bookmark b = doc->mark(1);
  doc->relayout(300, 400);
  CHECK(doc->page_count() == 2);   // the window does not repaginate a PDF.
  CHECK(doc->lookup(b) == 1);      // and page 2 is still page 2.
}

TEST_CASE("a_render_that_cannot_be_asked_for_is_an_error_not_a_crash") {
  auto doc = must_open(kPdf, 600, 800);
  CHECK_FALSE(doc->render(2, kPdfW, kPdfH).has_value());
  CHECK_FALSE(doc->render(-1, kPdfW, kPdfH).has_value());
  CHECK_FALSE(doc->render(0, 0, kPdfH).has_value());
  CHECK_FALSE(doc->render(0, kPdfW, -1).has_value());
  // Past the surface ceiling the zoom ladder respects: refused, not attempted.
  CHECK_FALSE(doc->render(0, 40000, 40000).has_value());
  // The document is still usable afterwards.
  CHECK(must_render(*doc, 0, kPdfW, kPdfH).w == kPdfW);
}

// ===========================================================================
// A reflowable book: the window owns the pages
// ===========================================================================

TEST_CASE("a_reflowable_book_is_paginated_by_the_layout_size") {
  auto doc = must_open(kEpub, 900, 1200);
  CHECK(doc->reflowable());
  const int wide = doc->page_count();
  CHECK(wide > 1);

  // Deterministic: the same window always yields the same book.
  doc->relayout(900, 1200);
  CHECK(doc->page_count() == wide);

  // A smaller window fits less text per page, so there are strictly more of
  // them — the whole reason a reading position has to survive a resize.
  doc->relayout(600, 800);
  const int narrow = doc->page_count();
  CHECK(narrow > wide);

  // And it comes back: the layout is a function of the size, not a ratchet.
  doc->relayout(900, 1200);
  CHECK(doc->page_count() == wide);
}

TEST_CASE("a_reflowed_page_renders_ink_on_paper") {
  auto doc = must_open(kEpub, 600, 800);
  const RgbaImage img = must_render(*doc, 0, 600, 800);
  CHECK(img.w == 600);
  CHECK(img.h == 800);
  // No golden pixels here — mupdf's own fonts draw this text. All the test
  // can honestly say is that something was drawn on the white ground.
  bool ink = false;
  for (std::size_t i = 0; i < img.rgba.size(); i += 4) {
    if (img.rgba[i] != 255) {
      ink = true;
      break;
    }
  }
  CHECK(ink);
}

TEST_CASE("a_bookmark_carries_the_reading_position_across_a_relayout") {
  auto doc = must_open(kEpub, 600, 800);
  const int before = doc->page_count();
  REQUIRE(before > 2);

  std::vector<Bookmark> marks;
  marks.reserve(static_cast<std::size_t>(before));
  for (int p = 0; p < before; ++p) marks.push_back(doc->mark(p));

  doc->relayout(400, 534);
  const int after = doc->page_count();
  CHECK(after > before);

  std::vector<int> landed;
  landed.reserve(marks.size());
  for (const Bookmark& b : marks) landed.push_back(doc->lookup(b));

  for (std::size_t p = 0; p < landed.size(); ++p) {
    // Every page finds a home in the new book — including the last page of a
    // chapter, which mupdf itself cannot bookmark (no text flow starts
    // there); the chapter fraction answers for those. The assertion is on
    // the outcome, so a mupdf that closes that gap still passes.
    CHECK(landed[p] >= 0);
    CHECK(landed[p] < after);
    // The reader stays in the chapter they were reading.
    CHECK(doc->mark(landed[p]).chapter == marks[p].chapter);
    // Reading order is preserved: a later page never lands earlier.
    if (p > 0) CHECK(landed[p - 1] <= landed[p]);
  }
}

TEST_CASE("the_resize_flow_keeps_the_place_in_a_real_book") {
  auto doc = must_open(kEpub, 600, 800);
  ViewState s;
  s.page = doc->page_count() / 2;
  s.page_count = doc->page_count();
  s.zoom = 150;
  s.fit = Fit::Width;
  s.scroll_y = 400;

  const ViewState r = relayout_and_remap(*doc, s, 400, 534);

  CHECK(r.page_count == doc->page_count());
  CHECK(r.page_count > s.page_count);
  CHECK(r.page >= 0);
  CHECK(r.page < r.page_count);
  // Further into a longer book than the old index alone would have put it.
  CHECK(r.page > s.page);
  CHECK(r.scroll_y == 0);  // the offset addressed the old layout.
  CHECK(r.zoom == 150);    // the reader's settings are not the layout's.
  CHECK(r.fit == Fit::Width);
}

// ===========================================================================
// What a reader is told when a file will not open
// ===========================================================================

TEST_CASE("an_unopenable_file_is_an_error_naming_the_path") {
  const std::filesystem::path dir = scratch_dir();

  const std::string missing = (dir / "absent.pdf").string();
  auto gone = MupdfSource::open(missing, 600, 800);
  REQUIRE_FALSE(gone.has_value());
  CHECK(gone.error().find(missing) != std::string::npos);

  // A truncated PDF: valid enough to start reading, not enough to finish.
  const std::string whole = read_file(kPdf);
  REQUIRE(whole.size() > 100);
  const std::string cut = (dir / "cut.pdf").string();
  write_file(cut, whole.substr(0, 100));
  auto truncated = MupdfSource::open(cut, 600, 800);
  REQUIRE_FALSE(truncated.has_value());
  CHECK(truncated.error().find(cut) != std::string::npos);

  // Prose with a .pdf name. It has to be content mupdf cannot parse at all:
  // an image renamed .pdf would open as a perfectly good one-page document.
  const std::string prose = (dir / "prose.pdf").string();
  write_file(prose, "This is not a PDF. It is a sentence about one.\n");
  CHECK_FALSE(MupdfSource::open(prose, 600, 800).has_value());

  std::filesystem::remove_all(dir);
}
