// mupdf_selftest.cpp — proves the libmupdf this build linked actually works
// at run time: open a document, count its pages, render the first one at
// 96 dpi. The configure-time link probe only shows that a candidate link set
// links; this shows that the library it produced loads and does its job,
// which is the part that a wrong companion library or a missing loader path
// breaks. ctest runs it on the committed two-page PDF fixture; it takes any
// document mupdf can open.
//
// Everything mupdf happens inside one try block whose locals are all
// trivially destructible: fz_try/fz_catch are setjmp/longjmp, and a longjmp
// over a C++ destructor is undefined behaviour. The result is copied out
// afterwards.

#include <mupdf/fitz.h>

#include <cstdio>

namespace {

struct Probe {
  int reflowable = -1;
  int pages = -1;
  int w = 0;
  int h = 0;
  int n = 0;
};

// Returns 0 on success, 1 with a message on stderr otherwise.
int probe(fz_context* ctx, const char* path, Probe* out) {
  fz_document* doc = nullptr;
  fz_page* page = nullptr;
  fz_pixmap* pix = nullptr;
  int rc = 0;
  const char* why = nullptr;
  fz_var(doc);
  fz_var(page);
  fz_var(pix);
  fz_var(rc);
  fz_var(why);
  fz_try(ctx) {
    fz_register_document_handlers(ctx);
    doc = fz_open_document(ctx, path);
    if (fz_needs_password(ctx, doc)) {
      why = "document is password-protected";
    } else {
      out->reflowable = fz_is_document_reflowable(ctx, doc);
      if (out->reflowable) fz_layout_document(ctx, doc, 450.0f, 600.0f, 12.0f);
      out->pages = fz_count_pages(ctx, doc);
      if (out->pages < 1) {
        why = "document has no pages";
      } else {
        page = fz_load_page(ctx, doc, 0);
        pix = fz_new_pixmap_from_page(ctx, page, fz_scale(96.0f / 72.0f, 96.0f / 72.0f),
                                      fz_device_rgb(ctx), /*alpha=*/0);
        out->w = fz_pixmap_width(ctx, pix);
        out->h = fz_pixmap_height(ctx, pix);
        out->n = fz_pixmap_components(ctx, pix);
      }
    }
  }
  fz_always(ctx) {
    fz_drop_pixmap(ctx, pix);
    fz_drop_page(ctx, page);
    fz_drop_document(ctx, doc);
  }
  fz_catch(ctx) {
    std::fprintf(stderr, "mupdf_selftest: %s: %s\n", path, fz_caught_message(ctx));
    rc = 1;
  }
  if (rc == 0 && why != nullptr) {
    std::fprintf(stderr, "mupdf_selftest: %s: %s\n", path, why);
    rc = 1;
  }
  return rc;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fputs("usage: mupdf_selftest <document>\n", stderr);
    return 2;
  }
  fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
  if (ctx == nullptr) {
    std::fputs("mupdf_selftest: cannot create a mupdf context\n", stderr);
    return 1;
  }
  Probe p;
  const int rc = probe(ctx, argv[1], &p);
  fz_drop_context(ctx);
  if (rc != 0) return rc;
  std::printf("mupdf %s: %s reflowable=%d pages=%d page1=%dx%d components=%d\n",
              FZ_VERSION, argv[1], p.reflowable, p.pages, p.w, p.h, p.n);
  return 0;
}
