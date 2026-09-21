// docsrc.cpp — the resize flow. Pure: it calls only the DocSource interface
// and the pager's remap, so it is testable against a scripted fake and
// compiles into viewer_tests with no graphics or document library in sight.

#include "docsrc.hpp"

namespace shigoku::view {

ViewState relayout_and_remap(DocSource& src, ViewState s, int w_px, int h_px) {
  if (!src.reflowable()) return s;  // fixed layout: the pages are the file's.
  const Bookmark b = src.mark(s.page);  // BEFORE the relayout invalidates it.
  src.relayout(w_px, h_px);
  // Sequenced, not two arguments of one call: both queries must observe the
  // NEW layout, and argument evaluation order would not say so.
  const int page = src.lookup(b);
  const int count = src.page_count();
  return remap_after_relayout(s, count, page);
}

}  // namespace shigoku::view
