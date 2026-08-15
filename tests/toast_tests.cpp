// toast_tests.cpp — P6 toast queue tests (DESIGN §4.7 / 04 §8).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../src/tui/cells.hpp"
#include "../src/tui/toast.hpp"

using namespace shigoku::tui;

TEST_CASE("cap 3: oldest non-persistent evicted, newest kept") {
  ToastQueue q;
  q.push(ToastKind::Info, "one", 0);
  q.push(ToastKind::Info, "two", 0);
  q.push(ToastKind::Info, "three", 0);
  q.push(ToastKind::Info, "four", 0);  // over cap -> evict "one"
  CHECK(q.size() == 3);
  auto v = q.visible();
  REQUIRE(v.size() == 3);
  CHECK(v[0].text == "two");
  CHECK(v[1].text == "three");
  CHECK(v[2].text == "four");
}

TEST_CASE("non-persistent auto-dismiss after 2.5s (25 ticks)") {
  ToastQueue q;
  q.push(ToastKind::Info, "ephemeral", 0);
  q.tick(24);
  CHECK(q.size() == 1);  // not yet
  q.tick(25);
  CHECK(q.empty());  // age >= lifetime -> gone
}

TEST_CASE("persistent toast is a per-topic singleton, refreshed in place") {
  ToastQueue q;
  q.push_persistent(ToastKind::Error, "net", "network unreachable", 0);
  q.push_persistent(ToastKind::Error, "net", "still unreachable", 5);
  CHECK(q.size() == 1);  // same topic -> updated, not appended
  auto v = q.visible();
  REQUIRE(v.size() == 1);
  CHECK(v[0].text == "still unreachable");
  CHECK(v[0].persistent);

  // Does not auto-dismiss.
  q.tick(1000);
  CHECK(q.size() == 1);

  // Clears on recovery.
  q.clear_topic("net");
  CHECK(q.empty());
}

TEST_CASE("persistent survives cap pressure; transients yield first") {
  ToastQueue q;
  q.push_persistent(ToastKind::Warning, "sync", "db out of sync", 0);
  q.push(ToastKind::Info, "a", 0);
  q.push(ToastKind::Info, "b", 0);
  q.push(ToastKind::Info, "c", 0);  // over cap -> evict a transient, keep sync
  CHECK(q.size() == 3);
  bool has_persistent = false;
  for (const auto& t : q.visible())
    if (t.persistent && t.topic == "sync") has_persistent = true;
  CHECK(has_persistent);
}

TEST_CASE("copy budget is 36 columns, prefix is 4 (box 40)") {
  CHECK(kToastMaxBoxCols == 40);
  CHECK(kToastGlyphCols == 4);
  CHECK(kToastMaxCopyCols == 36);
  // Every glyph prefix is exactly 4 display columns.
  CHECK(str_width(toast_glyph(ToastKind::Info)) == 4);
  CHECK(str_width(toast_glyph(ToastKind::Success)) == 4);  // "[✓] "
  CHECK(str_width(toast_glyph(ToastKind::Error)) == 4);
  CHECK(str_width(toast_glyph(ToastKind::Warning)) == 4);
}

TEST_CASE("long copy truncates to 36 display columns, width-aware") {
  const std::string longtext(60, 'x');  // 60 columns of copy
  const std::string cut = truncate_to_cols(longtext, kToastMaxCopyCols);
  CHECK(str_width(cut) <= kToastMaxCopyCols);
  CHECK(cut.substr(cut.size() - 3) == "\xE2\x80\xA6");  // trailing …
}
