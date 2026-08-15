// impersonate.hpp — dlopen seam to libcurl-impersonate (P25).
//
// anidb.app scores the TLS ClientHello (JA3/JA4); a stock OpenSSL handshake
// 403s where a real browser's passes. The fix is to drive that one provider's
// fetches through libcurl-impersonate (BoringSSL, Chrome/Firefox ClientHello)
// while every other request keeps riding the app-default OpenSSL libcurl.
//
// The two libcurls export byte-identical `curl_easy_*` names (only
// `curl_easy_impersonate` is unique to the impersonate build), so they CANNOT
// be link-time-linked into one image. Instead we dlopen the impersonate dylib
// with RTLD_LOCAL — the loader keeps its symbols private to our handle, so its
// `curl_easy_perform` is a distinct address from the stock one and the two
// never collide. shigoku consumes the STOCK, unmodified dylib exactly as every
// other runtime consumer (curl-cffi is the only linker; we are not it).
//
// This is the ONLY place that touches the impersonate lib. If it ever has to
// become a shell-out instead, only this TU + the fetch() branch change; no
// provider and no other call site moves (project_p25_clienthello_decision).

#pragma once

#include <curl/curl.h>  // CURL, CURLoption, CURLcode, CURLINFO — ABI-shared types.

namespace shigoku::http {

// The slice of the impersonate libcurl API the fetch path drives. Function
// pointers resolved once via dlsym; identical prototypes to <curl/curl.h> so
// call sites read the same as the stock path. `impersonate` is the one symbol
// the stock build lacks — its absence is how we detect a too-old/wrong dylib.
//
// curl_easy_setopt / curl_easy_getinfo are VARIADIC in libcurl. They MUST be
// typed and called as variadic here: on the SysV x86-64 ABI a variadic callee
// reads %al for its vector-register count, which a fixed-arity call site never
// sets — calling through a non-variadic pointer is UB and segfaults in
// practice (proven on real hardware). The `long`/`ptr` naming is just which trailing
// argument type the call site passes; the pointer type stays variadic.
struct ImpersonateApi {
  CURLcode (*global_init)(long) = nullptr;  // once before the first easy_init.
  CURL* (*easy_init)() = nullptr;
  CURLcode (*setopt)(CURL*, CURLoption, ...) = nullptr;
  CURLcode (*perform)(CURL*) = nullptr;
  CURLcode (*getinfo)(CURL*, CURLINFO, ...) = nullptr;
  void (*cleanup)(CURL*) = nullptr;
  struct curl_slist* (*slist_append)(struct curl_slist*, const char*) = nullptr;
  void (*slist_free_all)(struct curl_slist*) = nullptr;
  // The impersonate entry point. Signature per curl-impersonate:
  //   int curl_easy_impersonate(CURL*, const char* target, int default_headers)
  CURLcode (*impersonate)(CURL*, const char*, int) = nullptr;
};

// Resolve the impersonate dylib once (dlopen RTLD_NOW|RTLD_LOCAL + dlsym). Tries
// the MacPorts install name first, then a small fallback list. Returns nullptr
// if the dylib is absent, unreadable, or missing `curl_easy_impersonate` (a
// stock libcurl symlinked into place) — callers map that to a provider-level
// error so the fallback walk hops, never a hard crash. The returned pointer is
// process-lived (never freed; the dylib stays mapped for the process).
[[nodiscard]] const ImpersonateApi* load_impersonate_api();

}  // namespace shigoku::http
