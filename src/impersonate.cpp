// impersonate.cpp — dlopen/dlsym of libcurl-impersonate (P25). See the header.

#include "impersonate.hpp"

#include <dlfcn.h>

#include <mutex>

namespace shigoku::http {

namespace {

// Install names to try, MacPorts first (the user's target), then common Linux
// sonames for the dev box. RTLD_LOCAL keeps every resolved curl_easy_* symbol
// private to this handle so it never collides with the app-default libcurl.
constexpr const char* kDylibCandidates[] = {
    "libcurl-impersonate.dylib",             // macOS install name (dyld search path).
    "/opt/local/lib/libcurl-impersonate.dylib",  // MacPorts absolute (user target).
    "libcurl-impersonate.so",                // Linux soname (dev box / generic).
    "libcurl-impersonate-chrome.so",         // some distros suffix by flavor.
};

// The real curl_easy_setopt is variadic in the header, but its ABI takes a
// single trailing argument; casting the one exported symbol to each concrete
// arity is the supported way to call it through a pointer. void(*)() is the
// portable "any function pointer" we dlsym into, then cast.
using AnyFn = void (*)();

template <typename Fn>
Fn sym(void* handle, const char* name) {
  // reinterpret via AnyFn: object<->function cast is not guaranteed, but
  // function<->function through a common function-pointer type is fine, and
  // dlsym returns void* by POSIX — the double cast silences the pedantic path.
  return reinterpret_cast<Fn>(reinterpret_cast<AnyFn>(dlsym(handle, name)));
}

const ImpersonateApi* resolve_once() {
  static ImpersonateApi api;
  static const ImpersonateApi* result = nullptr;

  // RTLD_LOCAL keeps the impersonate lib's curl_* symbols from being EXPORTED
  // into the global namespace (so the app-default stock libcurl is never
  // shadowed). But on glibc that is not enough: the impersonate lib's OWN
  // internal references (its curl_easy_perform -> its helpers, its BoringSSL
  // init) would still bind to the globally-visible STOCK libcurl/OpenSSL
  // symbols loaded first, mixing two TLS stacks in one handle -> segfault
  // (proven on real hardware). RTLD_DEEPBIND makes the lib prefer its OWN
  // symbols for those internal references, which is exactly the isolation we
  // need. macOS has no RTLD_DEEPBIND, but its default two-level namespace
  // already binds each dylib's undefined symbols to their defining library, so
  // plain RTLD_LOCAL suffices there.
  int flags = RTLD_NOW | RTLD_LOCAL;
#ifdef RTLD_DEEPBIND
  flags |= RTLD_DEEPBIND;
#endif
  void* handle = nullptr;
  for (const char* name : kDylibCandidates) {
    handle = dlopen(name, flags);
    if (handle != nullptr) break;
  }
  if (handle == nullptr) return nullptr;  // dylib absent/unreadable.

  api.global_init = sym<CURLcode (*)(long)>(handle, "curl_global_init");
  api.easy_init = sym<CURL* (*)()>(handle, "curl_easy_init");
  // setopt/getinfo are variadic — bind to variadic pointers (see the header).
  api.setopt = sym<CURLcode (*)(CURL*, CURLoption, ...)>(handle, "curl_easy_setopt");
  api.perform = sym<CURLcode (*)(CURL*)>(handle, "curl_easy_perform");
  api.getinfo = sym<CURLcode (*)(CURL*, CURLINFO, ...)>(handle, "curl_easy_getinfo");
  api.cleanup = sym<void (*)(CURL*)>(handle, "curl_easy_cleanup");
  api.slist_append =
      sym<struct curl_slist* (*)(struct curl_slist*, const char*)>(
          handle, "curl_slist_append");
  api.slist_free_all =
      sym<void (*)(struct curl_slist*)>(handle, "curl_slist_free_all");
  api.impersonate =
      sym<CURLcode (*)(CURL*, const char*, int)>(handle, "curl_easy_impersonate");

  // A stock libcurl symlinked into the impersonate path resolves every symbol
  // EXCEPT curl_easy_impersonate. Require it (and the core calls) or refuse — a
  // half-resolved table would segfault on first use.
  const bool complete = api.global_init && api.easy_init && api.setopt &&
                        api.perform && api.getinfo && api.cleanup &&
                        api.slist_append && api.slist_free_all && api.impersonate;
  if (!complete) {
    // Leave the handle mapped (harmless) but report unavailable. Do NOT dlclose:
    // another thread may already hold a pointer we returned on a prior call.
    return nullptr;
  }

  // The impersonate lib is a self-contained libcurl (its own statically-linked
  // BoringSSL); it needs ITS OWN curl_global_init before the first easy handle,
  // independent of the stock libcurl's global init. Runs once (this whole fn is
  // call_once-guarded). CURL_GLOBAL_DEFAULT == 3.
  if (api.global_init(3L) != CURLE_OK) return nullptr;

  result = &api;
  return result;
}

}  // namespace

const ImpersonateApi* load_impersonate_api() {
  static std::once_flag once;
  static const ImpersonateApi* cached = nullptr;
  std::call_once(once, [] { cached = resolve_once(); });
  return cached;
}

}  // namespace shigoku::http
