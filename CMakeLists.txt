cmake_minimum_required(VERSION 3.20)
project(shigoku VERSION 0.1.0 LANGUAGES CXX)

# --- Language: C++23 (std::expected, if consteval, etc.). No GNU extensions. ---
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# --- Locked warning set (PORT_CPP.md §5, A2). The exhaustiveness fence lives here:
#     a new enum arm without a matching switch case is a HARD build error. ---
#
# SHIGOKU_WERROR (default ON) turns *all* warnings into errors — our normal dev
# contract. It is a separate knob from the two -Werror=switch* below because
# those are LOAD-BEARING (the A2 fence + its compile-fail tests depend on them)
# and must stay on regardless. A first build on an untested toolchain (older GCC
# or clang on the PPC target, debt #1) may surface a benign warning we have never
# seen; -DSHIGOKU_WERROR=OFF lets that build finish instead of hard-failing,
# without weakening the exhaustiveness guarantee.
option(SHIGOKU_WERROR "Treat all warnings as errors (fence -Werror=switch* stay on regardless)" ON)
add_library(shigoku_flags INTERFACE)
target_compile_options(shigoku_flags INTERFACE
  -Wall -Wextra
  -Werror=switch -Werror=switch-enum
  $<$<BOOL:${SHIGOKU_WERROR}>:-Werror>
)

# libstdc++ dual-ABI (§3). _GLIBCXX_USE_CXX11_ABI is a GCC/libstdc++-ONLY macro
# (clang/libc++ has no such thing). This is a TESTING knob, NOT an end-user
# option: it only matters when linking our code against a prebuilt libstdc++
# built with the pre-C++11 string/list ABI (=0), which we exercise on this dev
# box to keep the port ABI-agnostic. By DEFAULT we emit NOTHING and let the
# toolchain use its own default (the modern =1 for GCC). We must never emit it
# unasked: it is meaningless-to-harmful under clang and an unwanted end-user
# knob. Only when SHIGOKU_GLIBCXX_USE_CXX11_ABI is set to a non-empty value do
# we force _GLIBCXX_USE_CXX11_ABI to match, once here on shigoku_flags so EVERY
# TU — our code, the vendored SYSTEM headers, the tests — shares it (a mixed
# build silently mismatches std::string across a libstdc++ boundary). The code
# itself is ABI-agnostic (public std API only, no SSO/COW assumptions); this
# switch exists purely to align with a GCC target's prebuilt libstdc++.
set(SHIGOKU_GLIBCXX_USE_CXX11_ABI "" CACHE STRING
  "libstdc++ _GLIBCXX_USE_CXX11_ABI override (empty = toolchain default; 0 = legacy ABI, 1 = modern). Testing-only; do not set for normal builds.")
set_property(CACHE SHIGOKU_GLIBCXX_USE_CXX11_ABI PROPERTY STRINGS "" "0" "1")
if(NOT SHIGOKU_GLIBCXX_USE_CXX11_ABI STREQUAL "")
  target_compile_definitions(shigoku_flags INTERFACE
    _GLIBCXX_USE_CXX11_ABI=${SHIGOKU_GLIBCXX_USE_CXX11_ABI})
endif()

# --- Vendored single-headers (third_party/: json, stb, doctest, tl; sha256
#     stays vendored-only, no system equivalent worth swapping to). Each of
#     the other four can point at a system install instead, for packaging —
#     default stays the checked-in copy. All header-only, so "swap" just
#     means putting the system include root ahead of third_party/ on the
#     search path: the same <nlohmann/json.hpp>-style includes resolve to
#     whichever root comes first. Compiled as SYSTEM includes either way so
#     their own warnings never trip our -Werror. ---
option(SHIGOKU_SYSTEM_NLOHMANN_JSON "Use a system-installed nlohmann/json instead of the vendored header" OFF)
option(SHIGOKU_SYSTEM_STB "Use a system-installed stb instead of the vendored headers" OFF)
option(SHIGOKU_SYSTEM_DOCTEST "Use a system-installed doctest instead of the vendored header" OFF)
option(SHIGOKU_SYSTEM_TL_EXPECTED "Use a system-installed tl::expected instead of the vendored header" OFF)

add_library(shigoku_thirdparty INTERFACE)

if(SHIGOKU_SYSTEM_NLOHMANN_JSON)
  find_path(NLOHMANN_JSON_INCLUDE_DIR NAMES nlohmann/json.hpp)
  if(NLOHMANN_JSON_INCLUDE_DIR)
    target_include_directories(shigoku_thirdparty SYSTEM INTERFACE ${NLOHMANN_JSON_INCLUDE_DIR})
  else()
    message(WARNING "SHIGOKU_SYSTEM_NLOHMANN_JSON=ON but nlohmann/json.hpp was not found; falling back to the vendored copy.")
  endif()
endif()

if(SHIGOKU_SYSTEM_STB)
  find_path(STB_INCLUDE_DIR NAMES stb/stb_image.h)
  if(STB_INCLUDE_DIR)
    target_include_directories(shigoku_thirdparty SYSTEM INTERFACE ${STB_INCLUDE_DIR})
  else()
    message(WARNING "SHIGOKU_SYSTEM_STB=ON but stb/stb_image.h was not found; falling back to the vendored copy.")
  endif()
endif()

if(SHIGOKU_SYSTEM_DOCTEST)
  find_path(DOCTEST_INCLUDE_DIR NAMES doctest/doctest.h)
  if(DOCTEST_INCLUDE_DIR)
    target_include_directories(shigoku_thirdparty SYSTEM INTERFACE ${DOCTEST_INCLUDE_DIR})
  else()
    message(WARNING "SHIGOKU_SYSTEM_DOCTEST=ON but doctest/doctest.h was not found; falling back to the vendored copy.")
  endif()
endif()

if(SHIGOKU_SYSTEM_TL_EXPECTED)
  find_path(TL_EXPECTED_INCLUDE_DIR NAMES tl/expected.hpp)
  if(TL_EXPECTED_INCLUDE_DIR)
    target_include_directories(shigoku_thirdparty SYSTEM INTERFACE ${TL_EXPECTED_INCLUDE_DIR})
  else()
    message(WARNING "SHIGOKU_SYSTEM_TL_EXPECTED=ON but tl/expected.hpp was not found; falling back to the vendored copy.")
  endif()
endif()

# Vendored root always goes on last, so anything not found/opted-in above
# still resolves here (and sha256, which has no system option, always does).
target_include_directories(shigoku_thirdparty SYSTEM INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

# --- Dependency detection: prefer pkg-config, fall back to find_library. ---
find_package(PkgConfig QUIET)

# libcurl (A5) — built with nghttp2 (HTTP/2 is load-bearing, §3). Required.
if(PkgConfig_FOUND)
  pkg_check_modules(CURL IMPORTED_TARGET libcurl)
endif()
if(CURL_FOUND)
  add_library(shigoku::curl ALIAS PkgConfig::CURL)
  set(SHIGOKU_HAVE_CURL ON)
else()
  find_path(CURL_INCLUDE_DIR curl/curl.h)
  find_library(CURL_LIBRARY NAMES curl libcurl)
  if(CURL_INCLUDE_DIR AND CURL_LIBRARY)
    add_library(shigoku_curl INTERFACE)
    target_include_directories(shigoku_curl INTERFACE ${CURL_INCLUDE_DIR})
    target_link_libraries(shigoku_curl INTERFACE ${CURL_LIBRARY})
    add_library(shigoku::curl ALIAS shigoku_curl)
    set(SHIGOKU_HAVE_CURL ON)
  else()
    set(SHIGOKU_HAVE_CURL OFF)
    message(WARNING "libcurl not found: HTTP layer (P2), AniList (P3), senshi (P4), covers (P8) will not build.")
  endif()
endif()

# sqlite3 (A7). Required for the store; optional at scaffold time.
if(PkgConfig_FOUND)
  pkg_check_modules(SQLITE3 IMPORTED_TARGET sqlite3)
endif()
if(SQLITE3_FOUND)
  add_library(shigoku::sqlite3 ALIAS PkgConfig::SQLITE3)
  set(SHIGOKU_HAVE_SQLITE3 ON)
else()
  find_path(SQLITE3_INCLUDE_DIR sqlite3.h)
  find_library(SQLITE3_LIBRARY NAMES sqlite3)
  if(SQLITE3_INCLUDE_DIR AND SQLITE3_LIBRARY)
    add_library(shigoku_sqlite3 INTERFACE)
    target_include_directories(shigoku_sqlite3 INTERFACE ${SQLITE3_INCLUDE_DIR})
    target_link_libraries(shigoku_sqlite3 INTERFACE ${SQLITE3_LIBRARY})
    add_library(shigoku::sqlite3 ALIAS shigoku_sqlite3)
    set(SHIGOKU_HAVE_SQLITE3 ON)
  else()
    set(SHIGOKU_HAVE_SQLITE3 OFF)
    message(WARNING "sqlite3 not found: store (P9) will not build.")
  endif()
endif()

# libwebp (MANGA_PLAN MG-6c): WeebCentral/Dynasty/nhentai serve WebP pages
# and covers that stb_image cannot decode. Auto-off like WITH_VIEWER — never
# a hard configure error, since this is a manga-only decode path and not
# load-bearing for the anime side. SHIGOKU_HAVE_WEBP gates the
# shigoku_webp_decode target below.
option(WITH_WEBP "Decode WebP manga pages/covers via libwebp (auto-off if not found)" ON)
if(WITH_WEBP AND PkgConfig_FOUND)
  pkg_check_modules(WEBP IMPORTED_TARGET libwebp)
endif()
if(WITH_WEBP AND WEBP_FOUND)
  add_library(shigoku::webp ALIAS PkgConfig::WEBP)
  set(SHIGOKU_HAVE_WEBP ON)
elseif(WITH_WEBP)
  find_path(WEBP_INCLUDE_DIR webp/decode.h)
  find_library(WEBP_LIBRARY NAMES webp)
  if(WEBP_INCLUDE_DIR AND WEBP_LIBRARY)
    add_library(shigoku_webp INTERFACE)
    target_include_directories(shigoku_webp INTERFACE ${WEBP_INCLUDE_DIR})
    target_link_libraries(shigoku_webp INTERFACE ${WEBP_LIBRARY})
    add_library(shigoku::webp ALIAS shigoku_webp)
    set(SHIGOKU_HAVE_WEBP ON)
  else()
    set(SHIGOKU_HAVE_WEBP OFF)
    message(STATUS "shigoku-webp: libwebp not found — WebP manga pages/covers "
                   "will decode-fail cleanly (MG-6c). `source scripts/env.sh` "
                   "or install libwebp-dev to enable.")
  endif()
else()
  set(SHIGOKU_HAVE_WEBP OFF)
endif()

# --- Threads (A1: std::thread + condvar) and libatomic (PPC32 64-bit atomics, §3). ---
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# libatomic is only needed where 64-bit atomics aren't native (PPC32). Link it if
# the linker knows it; harmless no-op on x86_64/aarch64.
include(CheckLinkerFlag OPTIONAL RESULT_VARIABLE HAVE_CHECK_LINKER_FLAG)
add_library(shigoku_atomic INTERFACE)
if(HAVE_CHECK_LINKER_FLAG)
  check_linker_flag(CXX "-latomic" SHIGOKU_HAS_LIBATOMIC)
  if(SHIGOKU_HAS_LIBATOMIC)
    target_link_libraries(shigoku_atomic INTERFACE atomic)
  endif()
endif()

# ---------------------------------------------------------------------------
# shigoku_webp_decode (MANGA_PLAN MG-6c): the RIFF….WEBP magic sniff + thin
# libwebp wrapper, a leaf module like shigoku_paths/shigoku_semver. HAVE_WEBP
# is PUBLIC so every linker (shigoku_tui's covers.cpp, shigoku_view_bin's
# main.cpp) sees the same #ifdef — WITH_WEBP=OFF or libwebp-not-found means
# WebP bytes decode-fail cleanly rather than the two TUs disagreeing about
# whether decode_webp() exists.
# ---------------------------------------------------------------------------
add_library(shigoku_webp_decode STATIC src/webp_decode.cpp)
target_include_directories(shigoku_webp_decode PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(shigoku_webp_decode PUBLIC shigoku_flags shigoku_thirdparty)
if(SHIGOKU_HAVE_WEBP)
  target_link_libraries(shigoku_webp_decode PUBLIC shigoku::webp)
  target_compile_definitions(shigoku_webp_decode PUBLIC HAVE_WEBP)
endif()

# ---------------------------------------------------------------------------
# shigoku_domain (P1): the type vocabulary + pure helpers. Header-heavy; the
# .cpp files carry the non-inline helper bodies. No I/O, no deps beyond libc++.
# ---------------------------------------------------------------------------
add_library(shigoku_domain STATIC
  src/domain.cpp
  src/provider.cpp
)
target_include_directories(shigoku_domain PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(shigoku_domain PUBLIC shigoku_flags shigoku_thirdparty)

# ---------------------------------------------------------------------------
# shigoku_paths (P7 bootstrap): XDG dir resolution (paths.rs, 06 §1). A leaf
# module (imports nothing from the crate) — kept separate from shigoku_domain
# since it does real filesystem I/O (mkdir/chmod), unlike domain's pure fns.
# ---------------------------------------------------------------------------
add_library(shigoku_paths STATIC src/paths.cpp)
target_include_directories(shigoku_paths PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(shigoku_paths PUBLIC shigoku_flags shigoku_thirdparty)

# ---------------------------------------------------------------------------
# shigoku_config (P18): the config.json load/save (config.rs, 06 §2). A leaf
# module like shigoku_paths — real filesystem I/O, JSON via the vendored
# nlohmann header (no TOML lib is vendored; sabigoku's own doc left the
# format open). No domain dependency: keys stay strings, degrading to safe
# defaults at each call site the way config.rs's comment specifies.
# ---------------------------------------------------------------------------
add_library(shigoku_config STATIC src/config.cpp)
target_include_directories(shigoku_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(shigoku_config PUBLIC shigoku_flags shigoku_thirdparty)

# ---------------------------------------------------------------------------
# shigoku_auth (P19): the AniList auth token store (auth.rs, 06 §3). A leaf
# module like shigoku_config — real filesystem I/O, JSON via nlohmann — but
# deliberately its own target: the bearer must never ride a Settings
# round-trip, so nothing in this link ever pulls in shigoku_config's save
# path. The only new primitive over config.cpp is the 0600-atomic write
# (O_EXCL|O_CREAT, not chmod-after — a plain create would leak the ambient
# umask for a window before the chmod lands).
# ---------------------------------------------------------------------------
add_library(shigoku_auth STATIC src/auth.cpp)
target_include_directories(shigoku_auth PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(shigoku_auth PUBLIC shigoku_flags shigoku_thirdparty)

# ---------------------------------------------------------------------------
# shigoku_semver (P28): version comparison for the update check (semver.rs,
# 06 §6.1). A pure leaf module — no I/O, no dependencies beyond flags/
# thirdparty (the compile-flags/warnings interface every target picks up).
# ---------------------------------------------------------------------------
add_library(shigoku_semver STATIC src/semver.cpp)
target_include_directories(shigoku_semver PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(shigoku_semver PUBLIC shigoku_flags shigoku_thirdparty)

# ---------------------------------------------------------------------------
# shigoku_http (P2, A5): the one libcurl wrapper + the SSRF fetch guard. The
# guard is pure (byte-wise host classification, endian-neutral); the client
# needs libcurl (nghttp2, §3). Depends on domain for strip_controls parity.
# ---------------------------------------------------------------------------
if(SHIGOKU_HAVE_CURL)
  # impersonate.cpp does NOT link libcurl-impersonate — it dlopen's the STOCK
  # dylib at runtime (RTLD_LOCAL) so the two libcurls' identical curl_easy_*
  # symbols never collide. Only ${CMAKE_DL_LIBS} (dlopen/dlsym: -ldl on Linux,
  # empty on macOS) is needed here. See project_p25_clienthello_decision.
  add_library(shigoku_http STATIC src/http.cpp src/impersonate.cpp)
  target_include_directories(shigoku_http PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(shigoku_http
    PUBLIC shigoku_domain shigoku_flags shigoku_thirdparty
    PRIVATE shigoku::curl ${CMAKE_DL_LIBS})
endif()

# ---------------------------------------------------------------------------
# shigoku_updatecheck (P28): the boot-time GitHub release check (updatecheck.rs,
# 06 §6.1) — cache read/write/parse + the semver-gated `check()`. No self-update
# machinery (update.rs's install-method detection/install.sh spawn): the legacy
# target's binary is package-owned (MacPorts), and there is no standalone-
# install path to self-update into (user ruling, 2026-08-03) — shigoku only
# ever reports what it finds. Needs shigoku_http for the GET, gated the same.
# ---------------------------------------------------------------------------
if(SHIGOKU_HAVE_CURL)
  add_library(shigoku_updatecheck STATIC src/updatecheck.cpp)
  target_include_directories(shigoku_updatecheck PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(shigoku_updatecheck
    PUBLIC shigoku_http shigoku_semver shigoku_flags shigoku_thirdparty)
endif()

# ---------------------------------------------------------------------------
# shigoku_download (P35 slices 1+2): the headless episode-download core —
# curl-to-file through the A5 wrapper's streaming fetch (.part temp + atomic
# rename + Range resume) plus the HLS arm: external `ffmpeg -c copy` spawned
# like mpv (argv vetting, posix_spawnp, cancel-poll wait), master→variant via
# shigoku_hls, decloak links through the P12 proxy. shigoku-only (§9;
# sabigoku has no downloads). download_link owns the SSRF guard; the
# lower-level cores are mechanism (see download.hpp). Surfaces = slice 3.
# ---------------------------------------------------------------------------
if(SHIGOKU_HAVE_CURL)
  add_library(shigoku_download STATIC src/download.cpp)
  target_include_directories(shigoku_download PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(shigoku_download
    PUBLIC shigoku_http shigoku_domain shigoku_flags shigoku_thirdparty
    PRIVATE shigoku_hls shigoku_proxy Threads::Threads)
endif()

# ---------------------------------------------------------------------------
# shigoku_anilist (P3, +P20 sync/GraphQL list ops): public GraphQL search plus
# the AniList list pull/push (06 §5). JSON via the vendored nlohmann header;
# depends on http for the POST and, since P20, on store for the RemoteEntry
# wire-shape it builds (store.hpp is sqlite-only/curl-free, so this is the one
# new link edge rather than a third shared header — RemoteEntry lives where
# reconcile_pull, its only non-anilist consumer, already needs ListStatus).
# Gated on BOTH curl and sqlite3 now; search/discover/viewer don't need
# sqlite3 themselves, but the module as a whole does.
# ---------------------------------------------------------------------------
if(SHIGOKU_HAVE_CURL AND SHIGOKU_HAVE_SQLITE3)
  add_library(shigoku_anilist STATIC src/anilist.cpp)
  target_include_directories(shigoku_anilist PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(shigoku_anilist PUBLIC shigoku_http shigoku_store shigoku_domain shigoku_flags shigoku_thirdparty)
endif()

# ---------------------------------------------------------------------------
# shigoku_login (P19): the OAuth login core (login.rs, 06 §4.2) — token
# extract/floor-check/verify/build/save. I/O-free relative to the UI: the
# Verifier seam (anilist::viewer, injected) lets tests fake the network.
# Needs anilist for the real verifier + Viewer type, auth for the token
# store. Gated on both curl and sqlite3 because shigoku_anilist is (P20: it
# now links shigoku_store for RemoteEntry) — the core itself does no direct
# network or database I/O beyond calling the injected verifier.
# ---------------------------------------------------------------------------
if(SHIGOKU_HAVE_CURL AND SHIGOKU_HAVE_SQLITE3)
  add_library(shigoku_login STATIC src/login.cpp)
  target_include_directories(shigoku_login PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(shigoku_login PUBLIC shigoku_anilist shigoku_auth shigoku_domain shigoku_flags shigoku_thirdparty)
endif()

# ---------------------------------------------------------------------------
# shigoku_mal_login (P31 §9.1 slice 2): MAL's OAuth login core — Authorization
# Code + PKCE (mal_login.rs has no sabigoku reference; MAL never existed
# there). No shigoku_anilist dependency (unlike shigoku_login): MAL's REST
# client is its own module (slice 3), not GraphQL. Needs shigoku_http for the
# token-exchange POST, shigoku_auth for the .mal block, shigoku_nonce for the
# PKCE verifier. Gated the same as shigoku_login for consistency even though
# it does not itself need sqlite3 — kept in lockstep so the two providers'
# build gating never silently diverges.
# ---------------------------------------------------------------------------
if(SHIGOKU_HAVE_CURL AND SHIGOKU_HAVE_SQLITE3)
  add_library(shigoku_mal_login STATIC src/mal_login.cpp)
  target_include_directories(shigoku_mal_login PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(shigoku_mal_login
    PUBLIC shigoku_http shigoku_auth shigoku_domain shigoku_flags shigoku_thirdparty
    PRIVATE shigoku_nonce)
endif()

# ---------------------------------------------------------------------------
# shigoku_mal (P31 §9.1 slice 3): MAL API v2 REST client — get/put list entry
# + the status-map branch table (mal.hpp). No sabigoku reference (same reason
# as shigoku_mal_login). Deliberately no link edge to shigoku_mal_login or
# shigoku_anilist: this is the transport+mapping layer slice 4's mirror push
# will call through (mirrors anilist.hpp predating sync.cpp, P3 before P20);
# it needs only shigoku_http for the GET/PUT and shigoku_domain for
# ListStatus, not store.hpp (no RemoteEntry/sqlite dependency — that arrives
# with slice 4's dirty-row walk). Gated the same as the other MAL modules for
# consistency even though it needs neither auth.hpp nor sqlite3 directly.
# ---------------------------------------------------------------------------
if(SHIGOKU_HAVE_CURL AND SHIGOKU_HAVE_SQLITE3)
  add_library(shigoku_mal STATIC src/mal.cpp)
  target_include_directories(shigoku_mal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(shigoku_mal
    PUBLIC shigoku_http shigoku_domain shigoku_flags shigoku_thirdparty)
endif()

# ---------------------------------------------------------------------------
# shigoku_sync (P20): AniList sync orchestration (sync.rs, 06 §5) — dirty-
# tracking push, pull reconcile funnel, 429 backoff ladder. I/O-free relative
# to the UI: the AniListSync ABC (HttpAniListSync, the real one; a scripted
# fake in tests) and Sleeper seam let tests assert the 2s/60s schedule without
# a socket or a real wait. Needs anilist for the transport calls + RemoteEntry,
# auth for the token, store for the dirty work-list / reconcile / mark_synced.
# Gated the same as shigoku_login (transitively shigoku_anilist, shigoku_store).
# ---------------------------------------------------------------------------
if(SHIGOKU_HAVE_CURL AND SHIGOKU_HAVE_SQLITE3)
  add_library(shigoku_sync STATIC src/sync.cpp)
  target_include_directories(shigoku_sync PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(shigoku_sync PUBLIC shigoku_anilist shigoku_auth shigoku_store shigoku_domain shigoku_flags shigoku_thirdparty)
endif()

# ---------------------------------------------------------------------------
# shigoku_mal_mirror (P31 §9.1 slice 4): MAL mirror push orchestration
# (mal_mirror.hpp) — dirty-row walk over mal_id-keyed rows, NULL-mal_id skip
# (the store query), MAL-deleted-entry handling, own snapshot
# (mal_synced_status/progress, store v3). Deliberately its OWN module, not
# folded into shigoku_sync: "not a generalization of the 06 §5 sync law...
# the mirror is separate, simpler machinery behind its own client trait"
# (PORT_PARITY.md P31). Needs shigoku_mal for the transport calls,
# shigoku_auth for MalAuth, shigoku_store for the dirty work-list / mark_mal_synced.
# Gated the same as the other MAL modules.
# ---------------------------------------------------------------------------
if(SHIGOKU_HAVE_CURL AND SHIGOKU_HAVE_SQLITE3)
  add_library(shigoku_mal_mirror STATIC src/mal_mirror.cpp)
  target_include_directories(shigoku_mal_mirror PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(shigoku_mal_mirror
    PUBLIC shigoku_mal shigoku_auth shigoku_store shigoku_domain shigoku_flags shigoku_thirdparty)
endif()

# ---------------------------------------------------------------------------
# shigoku_loopback (P19): the OAuth callback listener (loopback.rs, 06 §4.4).
# Raw BSD sockets (same shape as shigoku_proxy: bind 127.0.0.1, accept loop,
# self-dial to wake a blocked accept), std-only beyond shigoku_login (the
# request handler) and shigoku_nonce (the CSRF token). No curl — this module
# never makes outbound HTTP itself, only serves a local listener — but it is
# gated the same as shigoku_login (transitively shigoku_anilist, shigoku_store) is.
# ---------------------------------------------------------------------------
if(SHIGOKU_HAVE_CURL AND SHIGOKU_HAVE_SQLITE3)
  add_library(shigoku_loopback STATIC src/loopback.cpp)
  target_include_directories(shigoku_loopback PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(shigoku_loopback
    PUBLIC shigoku_login shigoku_domain shigoku_flags shigoku_thirdparty
    PRIVATE shigoku_nonce Threads::Threads)
endif()

# ---------------------------------------------------------------------------
# shigoku_mal_loopback (P31 §9.1 slice 2): the MAL OAuth callback listener.
# Deliberately its own translation unit, not a second route grafted onto
# shigoku_loopback — see mal_loopback.hpp's header comment for why the two
# grant types don't share a handler. Links shigoku_login only for its
# provider-agnostic query-param parser (login::detail::param); it does NOT
# use anything AniList-specific from that module.
# ---------------------------------------------------------------------------
if(SHIGOKU_HAVE_CURL AND SHIGOKU_HAVE_SQLITE3)
  add_library(shigoku_mal_loopback STATIC src/mal_loopback.cpp)
  target_include_directories(shigoku_mal_loopback PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(shigoku_mal_loopback
    PUBLIC shigoku_mal_login shigoku_domain shigoku_flags shigoku_thirdparty
    PRIVATE shigoku_login shigoku_nonce Threads::Threads)
endif()

# ---------------------------------------------------------------------------
# shigoku_cli (P27): CLI dispatch (cli.rs, 06 §7) — cli.cpp is the pure parse
# over argv plus the sync/connect/search/pick renderers (pure data -> text);
# play_cli.cpp is the testable search->pick->play flow (main.rs play_flow),
# split out so the exit table (06 §7.4) unit-tests with a fake provider and a
# scripted picker (no mpv spawn). No process exit / arg parsing; main.cpp owns
# those. cli.cpp needs only header TYPES (login::ConnectResult, sync::
# SyncSummary, SearchHit, the *Error kinds); play_cli.cpp calls player/aniskip/
# store for real, hence those link edges. Gated the same as shigoku_login/sync.
# ---------------------------------------------------------------------------
if(SHIGOKU_HAVE_CURL AND SHIGOKU_HAVE_SQLITE3)
  add_library(shigoku_cli STATIC src/cli.cpp src/play_cli.cpp)
  target_include_directories(shigoku_cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  # P35 slice 3: play_cli.cpp's download_flow calls the download core.
  target_link_libraries(shigoku_cli
    PUBLIC shigoku_login shigoku_sync shigoku_player shigoku_aniskip shigoku_store
           shigoku_config shigoku_domain shigoku_flags shigoku_thirdparty
    PRIVATE shigoku_download)
endif()

# ---------------------------------------------------------------------------
# shigoku_senshi (P4 + P23): senshi.live StreamProvider ABC implementation.
# cap_variant (HLS quality capping) rides shigoku_hls (parse_master_playlist +
# select_variant).
# ---------------------------------------------------------------------------
if(SHIGOKU_HAVE_CURL)
  add_library(shigoku_senshi STATIC src/senshi.cpp)
  target_include_directories(shigoku_senshi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(shigoku_senshi PUBLIC shigoku_http shigoku_hls shigoku_domain shigoku_flags shigoku_thirdparty)
endif()

# ---------------------------------------------------------------------------
# shigoku_anibd (P12a): epeng.animeapps.top StreamProvider — AniList-keyed,
# no search, catalog + apilink hop + player-page scrape. No crypto/proxy. The
# one new primitive is the URL origin/join helper via libcurl's CURLU API, so
# it links curl directly (not just transitively via http).
# ---------------------------------------------------------------------------
if(SHIGOKU_HAVE_CURL)
  add_library(shigoku_anibd STATIC src/anibd.cpp)
  target_include_directories(shigoku_anibd PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(shigoku_anibd
    PUBLIC shigoku_http shigoku_domain shigoku_flags shigoku_thirdparty
    PRIVATE shigoku::curl)
endif()

# ---------------------------------------------------------------------------
# shigoku_megaplay (P13): megaplay.buzz StreamProvider — MAL-keyed (senshi-
# shaped, no resolve changes), no search, no listing (mints a grid from a
# count hint). Two GETs (embed -> getSources), no crypto. Its links carry
# decloak_segments=true, so playback routes through the P12 proxy (wired in
# player.cpp). Needs only http + domain (JSON via the header-only thirdparty).
# ---------------------------------------------------------------------------
if(SHIGOKU_HAVE_CURL)
  add_library(shigoku_megaplay STATIC src/megaplay.cpp)
  target_include_directories(shigoku_megaplay PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(shigoku_megaplay
    PUBLIC shigoku_http shigoku_domain shigoku_flags shigoku_thirdparty)
endif()

# ---------------------------------------------------------------------------
# shigoku_anidbapp (P25): anidb.app StreamProvider — no canonical key (tier-C
# search binds it, detail-page ids give tier-A confidence), 5-stage HTML/JSON
# hop chain, no crypto. Its edge fingerprints the TLS ClientHello, so every
# fetch sets Request::fingerprint = Chrome (the P25 impersonate seam in http).
# Links hls for cap_variant (this provider caps its own quality).
# ---------------------------------------------------------------------------
if(SHIGOKU_HAVE_CURL)
  add_library(shigoku_anidbapp STATIC src/anidbapp.cpp)
  target_include_directories(shigoku_anidbapp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(shigoku_anidbapp
    PUBLIC shigoku_http shigoku_hls shigoku_domain shigoku_flags shigoku_thirdparty)
endif()

# ---------------------------------------------------------------------------
# shigoku_anilibria: AniLibria (AniLiberty API v1) StreamProvider — no
# canonical key (the API carries no MAL/AniList ids; tier-C search binds it),
# pure JSON chain with DIRECT per-quality HLS urls in the release object. No
# crypto, no cloaking, no fingerprinting; needs only http.
# ---------------------------------------------------------------------------
if(SHIGOKU_HAVE_CURL)
  add_library(shigoku_anilibria STATIC src/anilibria.cpp)
  target_include_directories(shigoku_anilibria PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(shigoku_anilibria
    PUBLIC shigoku_http shigoku_domain shigoku_flags shigoku_thirdparty)
endif()

# ---------------------------------------------------------------------------
# shigoku_mangadex (MANGA_PLAN MG-1): the manga app's MangaDex source — pure
# parsers + URL builders in detail:: (offline golden tests over committed
# fixtures) + a small blocking transport. Greenfield (no sabigoku
# reference); shape follows the anime provider pattern. JSON via the
# header-only thirdparty; needs only http.
# ---------------------------------------------------------------------------
if(SHIGOKU_HAVE_CURL)
  add_library(shigoku_mangadex STATIC src/manga/mangadex.cpp)
  target_include_directories(shigoku_mangadex PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(shigoku_mangadex
    PUBLIC shigoku_http shigoku_flags shigoku_thirdparty)
endif()

# ---------------------------------------------------------------------------
# shigoku_weebcentral (MANGA_PLAN MG-6b): the WeebCentral source — string-scan
# parsers over the site's HTMX fragments (offline golden tests over committed
# fixtures) + a small blocking transport with CF-challenge detection. Links
# shigoku_mangadex for the shared detail helpers (url_encode, filename_safe,
# chapter_less).
# ---------------------------------------------------------------------------
if(SHIGOKU_HAVE_CURL)
  add_library(shigoku_weebcentral STATIC src/manga/weebcentral.cpp)
  target_include_directories(shigoku_weebcentral PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(shigoku_weebcentral
    PUBLIC shigoku_mangadex shigoku_http shigoku_flags shigoku_thirdparty)
endif()

# ---------------------------------------------------------------------------
# shigoku_dynasty (MANGA_PLAN MG-6d): the Dynasty Reader source — two JSON
# parsers over the site's Rails .json endpoints plus one string-scan HTML
# parser (search has no JSON twin) + a small blocking transport. Links
# shigoku_weebcentral for wc::html_unescape (and through it shigoku_mangadex
# for url_encode / filename_safe / chapter_less): generic scrape helpers stay
# where they were born, the law md/wc already run on.
# ---------------------------------------------------------------------------
if(SHIGOKU_HAVE_CURL)
  add_library(shigoku_dynasty STATIC src/manga/dynasty.cpp)
  target_include_directories(shigoku_dynasty PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(shigoku_dynasty
    PUBLIC shigoku_weebcentral shigoku_mangadex shigoku_http shigoku_flags
           shigoku_thirdparty)
endif()

# ---------------------------------------------------------------------------
# shigoku_nhentai (MANGA_PLAN MG-6e): the nhentai source — three JSON parsers
# over the v2 FastAPI (search/cdn/gallery) + a blocking transport with the
# adaptive TLS-fingerprint posture (plain first, sticky Chrome escalation on
# 403 through the P25 impersonate seam). nsfw() true: the `nsfw_sources`
# config gate (default off) keeps it invisible until the user opts in. Links
# shigoku_mangadex for detail::url_encode / filename_safe (no HTML anywhere,
# so no weebcentral dependency).
# ---------------------------------------------------------------------------
if(SHIGOKU_HAVE_CURL)
  add_library(shigoku_nhentai STATIC src/manga/nhentai.cpp)
  target_include_directories(shigoku_nhentai PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(shigoku_nhentai
    PUBLIC shigoku_mangadex shigoku_http shigoku_flags shigoku_thirdparty)
endif()

# ---------------------------------------------------------------------------
# shigoku_manga_flow (MANGA_PLAN MG-3): the read flow's mechanism half —
# chapter fetch-to-cache (pages.cpp: fetch_to_sink + .part/.complete + node
# rotation), the ViewerBackend seam (viewer.cpp: argv table + spawn/wait),
# and config-lite (mconfig.cpp). No TUI dependency: testable headless.
# ---------------------------------------------------------------------------
if(SHIGOKU_HAVE_CURL)
  add_library(shigoku_manga_flow STATIC
    src/manga/pages.cpp src/manga/viewer.cpp src/manga/mconfig.cpp)
  target_include_directories(shigoku_manga_flow PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(shigoku_manga_flow
    PUBLIC shigoku_mangadex shigoku_http shigoku_flags shigoku_thirdparty)
endif()

# ---------------------------------------------------------------------------
# shigoku_mstore (MANGA_PLAN MG-4): manga.db — the library, persistent read
# progress, and the chapter cache the offline-first read flow leans on. Own
# schema, own ladder, own error type; it shares nothing with the anime store
# but the discipline (atomic ladder, no silent-NULL binds). A leaf on
# model.hpp + sqlite3 — no curl, no tui, no http.
# ---------------------------------------------------------------------------
if(SHIGOKU_HAVE_SQLITE3)
  add_library(shigoku_mstore STATIC src/manga/mstore.cpp)
  target_include_directories(shigoku_mstore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(shigoku_mstore
    PUBLIC shigoku_flags shigoku_thirdparty
    PRIVATE shigoku::sqlite3)
endif()

# ---------------------------------------------------------------------------
# shigoku_manga_sync (MANGA_PLAN MG-5): the AniList progress push — the pure
# floor/guard rules plus the two list calls behind them. Not a reuse of
# shigoku_sync: that module drains the ANIME store's dirty set, this one moves
# one entry on a mark-read (msync.hpp's header comment has the full why).
# Gated on curl AND sqlite3 because shigoku_anilist is (it links shigoku_store
# for RemoteEntry/ListEntry).
# ---------------------------------------------------------------------------
if(SHIGOKU_HAVE_CURL AND SHIGOKU_HAVE_SQLITE3)
  add_library(shigoku_manga_sync STATIC src/manga/msync.cpp)
  target_include_directories(shigoku_manga_sync PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(shigoku_manga_sync
    PUBLIC shigoku_anilist shigoku_http shigoku_domain shigoku_flags shigoku_thirdparty)
endif()

# ---------------------------------------------------------------------------
# shigoku_nonce (P12): 128-bit hex nonce from /dev/urandom (nonce.rs). A leaf
# (std-only). Used for the de-cloak proxy's per-playback token (ROD-447).
# ---------------------------------------------------------------------------
add_library(shigoku_nonce STATIC src/nonce.cpp)
target_include_directories(shigoku_nonce PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(shigoku_nonce PUBLIC shigoku_flags shigoku_thirdparty)

# ---------------------------------------------------------------------------
# shigoku_hls (P12 seed + P23): join_url, parse_master_playlist,
# select_variant (providers/hls.rs). String-based join (NOT CURLU — it must
# not normalize ./ and ../), so it needs no curl; depends on domain for
# is_absolute_url/Quality/StreamLink.
# ---------------------------------------------------------------------------
add_library(shigoku_hls STATIC src/hls.cpp)
target_include_directories(shigoku_hls PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(shigoku_hls PUBLIC shigoku_domain shigoku_flags shigoku_thirdparty)

# ---------------------------------------------------------------------------
# shigoku_proxy (P12, ROD-443): localhost HLS de-cloaking reverse proxy. Binds
# 127.0.0.1:0, rewrites playlists to tokened loopback refs, byte-strips segment
# decoy prefixes to the first TS sync. fetch_upstream follows redirects by hand
# so every hop re-passes the P2 SSRF guard (hence http); it links curl directly
# for the 32 MiB / identity / manual-redirect fetch the provider client lacks.
# ---------------------------------------------------------------------------
if(SHIGOKU_HAVE_CURL)
  add_library(shigoku_proxy STATIC src/proxy.cpp)
  target_include_directories(shigoku_proxy PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(shigoku_proxy
    PUBLIC shigoku_domain shigoku_flags shigoku_thirdparty
    PRIVATE shigoku_http shigoku_hls shigoku_nonce Threads::Threads shigoku::curl)
endif()

# ---------------------------------------------------------------------------
# shigoku_player (P5, A6): mpv posix_spawnp + push-based JSON IPC + retry law.
# Pure argv builder + resume rule; the play() path spawns mpv and guards the
# stream url via the P2 fetch guard (hence the http dep). Engages the P12
# de-cloak proxy for a flagged StreamLink (else a pass-through). JSON via the
# vendored nlohmann header (IPC line parsing).
# ---------------------------------------------------------------------------
if(SHIGOKU_HAVE_CURL)
  add_library(shigoku_player STATIC src/player.cpp)
  target_include_directories(shigoku_player PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(shigoku_player
    PUBLIC shigoku_domain shigoku_flags shigoku_thirdparty
    PRIVATE shigoku_http shigoku_proxy Threads::Threads shigoku_atomic)
endif()

# ---------------------------------------------------------------------------
# shigoku_aniskip (P22, 03 §9): AniSkip OP/ED fetch -> skip.lua + mpv
# --script-opts, keyed on MAL id + numeric episode. Best-effort throughout: any
# failure collapses to plain play, never a hard error. Falls back to a Jikan
# title lookup when enrichment carries no MAL id (shigoku addition, no 1:1
# Rust source — sabigoku ships without a jikan module, ROD-439). Needs
# shigoku_player only for the SkipScript{path,opts} return type.
# ---------------------------------------------------------------------------
if(SHIGOKU_HAVE_CURL)
  add_library(shigoku_aniskip STATIC src/aniskip.cpp)
  target_include_directories(shigoku_aniskip PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(shigoku_aniskip
    PUBLIC shigoku_domain shigoku_flags shigoku_thirdparty
    PRIVATE shigoku_http shigoku_player)
endif()

# ---------------------------------------------------------------------------
# shigoku_store (P9, A7): SQLite persistence on the 02 §3.3 key shapes. Resume
# rule + progress ratchet + watched marks; migration v1 = show + episode_
# progress. Links domain (for the thresholds/ListStatus/label sort) + sqlite3
# ONLY — never http/provider/tui — so the store stays a leaf the render core
# provably cannot reach (the shigoku_tui_render link graph excludes it). Gated
# on sqlite like the network modules are on curl.
# ---------------------------------------------------------------------------
if(SHIGOKU_HAVE_SQLITE3)
  add_library(shigoku_store STATIC src/store.cpp)
  target_include_directories(shigoku_store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(shigoku_store
    PUBLIC shigoku_domain shigoku_flags shigoku_thirdparty
    PRIVATE shigoku::sqlite3)
endif()

# ---------------------------------------------------------------------------
# shigoku_resolve (P15): the pure headless resolve engine + Tier-B/C scorer.
# resolve.cpp is the resolve.rs port — classify_open / open_history /
# route_preferred / the Walk state machine + K-2 exhaust — pure over a
# ResolveWorld read-ABC (no store, no threads, no network). resolver.cpp is the
# resolver.rs scorer (title_score/best_id_match/best_provider_match), also pure.
# Depends ONLY on shigoku_domain (provider.hpp lives there), so the whole engine
# unit-tests offline against a FakeWorld (§8) and the transport (app.cpp) owns
# every store write + staleness guard.
# ---------------------------------------------------------------------------
add_library(shigoku_resolve STATIC
  src/resolve.cpp
  src/resolver.cpp
)
target_include_directories(shigoku_resolve PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(shigoku_resolve PUBLIC shigoku_domain shigoku_flags shigoku_thirdparty)

# ---------------------------------------------------------------------------
# shigoku_tui_render (P6/P7, A2/A3 purity core): cells/term/input/toast/theme/
# queue/workers + views.cpp (the pure browse/detail/grid render). Depends
# ONLY on domain + threads — NEVER http/provider/sqlite — so `draw`/`views`
# provably have no network/store reachable from the render path (enforced by
# THIS target's link graph; app.cpp, which spawns real workers, is deliberately
# a separate target below so that guarantee stays checkable).
# ---------------------------------------------------------------------------
add_library(shigoku_tui_render STATIC
  src/tui/cells.cpp
  src/tui/term.cpp
  src/tui/input.cpp
  src/tui/toast.cpp
  src/tui/views.cpp
  # P8 cover render-side: the pure Kitty + OSC 1337 emitters, cover geometry,
  # and the startup terminal probe. All terminal/pure — NO curl — so they stay
  # in the purity core (covers.cpp, which fetches, is in the curl-gated
  # shigoku_tui).
  src/tui/kitty.cpp
  src/tui/iterm.cpp
  src/tui/cover_geom.cpp
  src/tui/cover_probe.cpp
  # P17: Discover card-grid pure state + geometry + glyphs (no store/curl — the
  # store-touching on_feed lives in discover_feed.cpp in shigoku_tui below).
  src/tui/discover.cpp
  # P17b: the reusable url-keyed multi-cover pool (coordinator + render
  # store) — pure, no curl/store, so it lives in the render core too.
  src/tui/cover_pool.cpp
  # P18: the Settings view state (row model, cycling, edit mode) — pure, no
  # curl/store, mutates only a Config (shigoku_config) passed by reference.
  src/tui/settings.cpp
  # P19: the AniList connect modal render (DESIGN 5.5a) — pure, no curl; the
  # live OAuth flow (loopback worker, complete_login) is curl-gated in
  # shigoku_tui below, same split as covers.cpp/kitty.cpp.
  src/tui/connect.cpp
  # P38: the Discover filter overlay (state + render) — pure, no curl; the
  # GenreCollection fetch worker spawn lives in app.cpp (shigoku_tui below),
  # same split as connect.cpp/settings.cpp.
  src/tui/discover_filters.cpp
)
target_include_directories(shigoku_tui_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(shigoku_tui_render
  PUBLIC shigoku_domain shigoku_config shigoku_auth shigoku_flags shigoku_thirdparty
  PRIVATE Threads::Threads shigoku_atomic)

# ---------------------------------------------------------------------------
# shigoku_tui (P6/P7, A1): app.cpp — tick()/on_key()/run(), the event loop and
# the P7 worker spawns (search/episodes/play). Needs anilist+senshi+player+http
# (curl) to actually fetch, so this target — unlike shigoku_tui_render above —
# is gated on SHIGOKU_HAVE_CURL like every other network-touching module.
# ---------------------------------------------------------------------------
if(SHIGOKU_HAVE_CURL AND SHIGOKU_HAVE_SQLITE3)
  # app.cpp (event loop + cover wiring) + covers.cpp (the fetch/decode/resize
  # pipeline — curl-dependent, so it lives here, not in the render core).
  # Also links the P9 store (checkpoint/resume/watched marks from the event
  # loop). app.hpp only forward-declares Store (a pointer in AppDeps), so the
  # render core stays off the sqlite link graph; app.cpp is the only TU that
  # includes store.hpp.
  add_library(shigoku_tui STATIC
    src/tui/app.cpp
    src/tui/covers.cpp
    src/tui/discover_feed.cpp      # P17: DiscoverState::on_feed (upserts catalog_cache).
    src/tui/resolve_transport.cpp  # P15: the resolve walk transport (episodes.rs).
    src/tui/prewarm.cpp)           # P15: the eager sibling-provider warm walk.
  target_include_directories(shigoku_tui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # app.cpp is the tree's second-biggest TU — same 1GB-box OOM cliff (and
    # same knob) as the app_tests target below.
    target_compile_options(shigoku_tui PRIVATE
      --param=ggc-min-expand=10 --param=ggc-min-heapsize=32768)
  endif()
  target_link_libraries(shigoku_tui
    PUBLIC shigoku_tui_render shigoku_domain shigoku_flags shigoku_thirdparty
    PRIVATE shigoku_anilist shigoku_player shigoku_aniskip shigoku_http shigoku_store
            shigoku_resolve shigoku_login shigoku_loopback shigoku_mal_login shigoku_mal_loopback
            shigoku_sync shigoku_mal_mirror shigoku_updatecheck shigoku_download
            shigoku_webp_decode Threads::Threads shigoku_atomic)

  # --- shigoku-manga (MANGA_PLAN MG-1): the manga companion app -------------
  # Second executable, same tree, shared static libs (ANALYSIS §4c). mapp.cpp
  # links the pure render core + the MangaDex source; covers.cpp comes from
  # shigoku_tui via selective object pull (mapp references only Covers/
  # make_http_fetch — the anime app.cpp object never links in). Rename knob:
  # SHIGOKU_MANGA_BIN (MANGA_PLAN §0 "rename is one CMake var").
  set(SHIGOKU_MANGA_BIN "shigoku-manga" CACHE STRING
      "Output name of the manga companion binary")
  add_library(shigoku_manga_app STATIC src/manga/mapp.cpp)
  target_include_directories(shigoku_manga_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  # shigoku_mstore is PUBLIC: mapp.hpp carries MangaStore*/LibraryEntry in its
  # state, so every consumer compiles against manga.db's headers (they stay
  # sqlite-free — the C handle lives only in mstore.cpp).
  # shigoku_auth is PUBLIC for the same reason as shigoku_mstore: mapp.hpp
  # carries the Auth record in its state. The AniList transport (msync) and the
  # OAuth halves (login/loopback) are mapp.cpp's business alone.
  target_link_libraries(shigoku_manga_app
    PUBLIC shigoku_tui_render shigoku_mangadex shigoku_manga_flow shigoku_mstore
           shigoku_auth shigoku_flags shigoku_thirdparty
    PRIVATE shigoku_tui shigoku_http shigoku_manga_sync shigoku_login shigoku_loopback
            Threads::Threads shigoku_atomic)

  add_executable(shigoku_manga_bin src/manga/main.cpp)
  set_target_properties(shigoku_manga_bin PROPERTIES OUTPUT_NAME ${SHIGOKU_MANGA_BIN})
  target_compile_definitions(shigoku_manga_bin PRIVATE SHIGOKU_VERSION="${PROJECT_VERSION}")
  target_link_libraries(shigoku_manga_bin PRIVATE
    shigoku_manga_app shigoku_tui shigoku_paths shigoku_http shigoku_mangadex
    shigoku_weebcentral shigoku_dynasty shigoku_nhentai
    shigoku_manga_flow shigoku_mstore shigoku_manga_sync shigoku_auth
    shigoku_login shigoku_loopback shigoku_flags shigoku_thirdparty
    shigoku::curl shigoku::sqlite3 Threads::Threads shigoku_atomic)

  # drive-tui (P6 DoD demo binary): bars, animating spinner, key echo, live
  # resize reflow, q/Ctrl-C restore.
  add_executable(drive_tui src/tui/drive_tui.cpp)
  target_link_libraries(drive_tui PRIVATE
    shigoku_tui shigoku_domain shigoku_flags shigoku_thirdparty
    Threads::Threads shigoku_atomic)

  # shigoku (P7/P9): the real app — paths -> http client -> senshi -> registry
  # -> store -> tui::run. The §0 acceptance script binary. main.cpp uses the
  # store directly (opens the DB), so link it explicitly (not just transitively).
  add_executable(shigoku src/main.cpp)
  # P27: main.cpp now dispatches the CLI surface, so it calls login/loopback/
  # sync/anilist/player/aniskip directly (the headless paths) in addition to
  # tui::run. SHIGOKU_VERSION feeds the `--version` line (project(VERSION …)).
  target_compile_definitions(shigoku PRIVATE SHIGOKU_VERSION="${PROJECT_VERSION}")
  target_link_libraries(shigoku PRIVATE
    shigoku_tui shigoku_megaplay shigoku_senshi shigoku_anibd shigoku_anidbapp
    shigoku_anilibria
    shigoku_http shigoku_store shigoku_cli shigoku_anilist shigoku_login
    shigoku_loopback shigoku_mal_login shigoku_mal_loopback
    shigoku_sync shigoku_mal_mirror shigoku_auth shigoku_player shigoku_aniskip
    shigoku_updatecheck shigoku_semver shigoku_download
    shigoku_domain shigoku_paths shigoku_config shigoku_flags shigoku_thirdparty
    shigoku::curl shigoku::sqlite3 Threads::Threads shigoku_atomic)
endif()

# ---------------------------------------------------------------------------
# shigoku-view (MANGA_PLAN MG-2): the SDL micro-viewer. Independent of curl AND
# sqlite (it decodes local files with vendored stb, plus libwebp when
# WITH_WEBP is on — MG-6c) — so it lives OUTSIDE the tui block's
# HAVE_CURL/HAVE_SQLITE3 guard. Two pieces:
#   - viewer_tests: the PURE pager core (pager.cpp), NO SDL — always built and
#     ctest-registered, the MG-2 DoD's offline proof.
#   - shigoku-view binary: gated on SDL via pkg-config, auto-off with a status
#     line (never a hard configure error, per the plan). SDL2 default;
#     -DVIEWER_SDL3=ON selects SDL3 (rev 2.3: SDL2=Cocoa on ppc darwin, SDL3
#     X11-only there — hence opt-in). Rename knob: SHIGOKU_VIEW_BIN.
# ---------------------------------------------------------------------------
add_executable(viewer_tests tests/viewer_tests.cpp src/view/pager.cpp)
target_include_directories(viewer_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(viewer_tests PRIVATE
  shigoku_flags shigoku_thirdparty Threads::Threads shigoku_atomic)
add_test(NAME viewer_tests COMMAND viewer_tests)

set(SHIGOKU_VIEW_BIN "shigoku-view" CACHE STRING "Output name of the manga viewer binary")
option(WITH_VIEWER "Build the shigoku-view binary (needs SDL2/SDL3 via pkg-config)" ON)
option(VIEWER_SDL3 "Build shigoku-view against SDL3 instead of SDL2" OFF)
if(WITH_VIEWER AND PkgConfig_FOUND)
  if(VIEWER_SDL3)
    pkg_check_modules(SDLX IMPORTED_TARGET sdl3)
  else()
    pkg_check_modules(SDLX IMPORTED_TARGET sdl2)
  endif()
  if(SDLX_FOUND)
    set(SHIGOKU_VIEW_SRC src/view/main.cpp src/view/sdl_compat.cpp src/view/pager.cpp)
    # macOS: SDL's own "Toggle Full Screen" menu item is wired to a 10.7-only
    # NSWindow selector, so it is greyed out on the systems this port cares
    # about. mac_menu.mm takes that item over and adds the zoom items; it is
    # AppKit-only (no SDL), hence its own ObjC++ TU.
    if(APPLE)
      enable_language(OBJCXX)
      list(APPEND SHIGOKU_VIEW_SRC src/view/mac_menu.mm)
    endif()
    add_executable(shigoku_view_bin ${SHIGOKU_VIEW_SRC})
    set_target_properties(shigoku_view_bin PROPERTIES OUTPUT_NAME ${SHIGOKU_VIEW_BIN})
    target_include_directories(shigoku_view_bin PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(shigoku_view_bin PRIVATE
      PkgConfig::SDLX shigoku_webp_decode shigoku_flags shigoku_thirdparty
      Threads::Threads shigoku_atomic)
    if(APPLE)
      target_link_libraries(shigoku_view_bin PRIVATE "-framework Cocoa")
      # The ObjC++ TU needs nothing past C++17 (enum class + a function
      # pointer), and pinning it there keeps the ObjC++ front end off whatever
      # the C++23 setting above would demand of it.
      set_target_properties(shigoku_view_bin PROPERTIES
        OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED ON OBJCXX_EXTENSIONS OFF)
    endif()
    if(VIEWER_SDL3)
      target_compile_definitions(shigoku_view_bin PRIVATE VIEWER_SDL3)
    endif()
  else()
    message(STATUS "shigoku-view: SDL not found via pkg-config — skipping the "
                   "binary (viewer_tests still built). `source scripts/env.sh` "
                   "or install SDL2 to enable it.")
  endif()
elseif(WITH_VIEWER)
  message(STATUS "shigoku-view: pkg-config unavailable — skipping the binary "
                 "(viewer_tests still built).")
endif()

# ---------------------------------------------------------------------------
# smoke: minimal binary proving raw mode + truecolor + terminal restore.
# This is the P0 deliverable; real modules land in later phases.
# ---------------------------------------------------------------------------
add_executable(smoke src/smoke.cpp)
target_link_libraries(smoke PRIVATE shigoku_flags shigoku_thirdparty Threads::Threads shigoku_atomic)

# ---------------------------------------------------------------------------
# Tests: doctest, wired into ctest. One binary at scaffold time; later phases
# add module test binaries via the shigoku_add_test() helper below.
# ---------------------------------------------------------------------------
enable_testing()

# shigoku_add_test(<name> <src>...) : a doctest binary registered with ctest,
# carrying our flag set + third-party includes and the domain library.
function(shigoku_add_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE
    shigoku_domain shigoku_flags shigoku_thirdparty Threads::Threads shigoku_atomic)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

shigoku_add_test(scaffold_tests tests/scaffold_tests.cpp)
shigoku_add_test(domain_tests tests/domain_tests.cpp)

# config_tests (P18): config.json load/save — total-load defaults, atomic
# save, oversized/corrupt/FIFO handling, concurrency clamp. Pure file I/O, no
# curl/sqlite; links only the leaf shigoku_config.
add_executable(config_tests tests/config_tests.cpp)
target_link_libraries(config_tests PRIVATE
  shigoku_config shigoku_flags shigoku_thirdparty Threads::Threads shigoku_atomic)
add_test(NAME config_tests COMMAND config_tests)

# auth_tests (P19): auth.json load/save — total-load defaults, atomic 0600
# save, oversized/corrupt/FIFO handling, control-byte token rejection,
# is_expired boundary. Pure file I/O, no curl/sqlite; links only the leaf
# shigoku_auth.
add_executable(auth_tests tests/auth_tests.cpp)
target_link_libraries(auth_tests PRIVATE
  shigoku_auth shigoku_flags shigoku_thirdparty Threads::Threads shigoku_atomic)
add_test(NAME auth_tests COMMAND auth_tests)

# settings_tests (P18): the Settings view state machine — row nav, preset
# cycling (incl. out-of-preset snap-to-zero + provider wheel wrap), toggles,
# edit mode (prefill/append/backspace/commit/cancel, control-char rejection).
# Pure state, no curl/sqlite; links the render core + config.
add_executable(settings_tests tests/settings_tests.cpp)
target_link_libraries(settings_tests PRIVATE
  shigoku_tui_render shigoku_config shigoku_domain shigoku_flags shigoku_thirdparty
  Threads::Threads shigoku_atomic)
add_test(NAME settings_tests COMMAND settings_tests)

# connect_tests (P19): the AniList connect modal's pure render — the row-plan
# fit check across a size sweep (no key hint ever silently clips), the
# contained-not-full-bleed float shape, and the cramped-fallback strip. Pure
# state, no curl/sqlite; links only the render core + domain.
add_executable(connect_tests tests/connect_tests.cpp)
target_link_libraries(connect_tests PRIVATE
  shigoku_tui_render shigoku_domain shigoku_flags shigoku_thirdparty
  Threads::Threads shigoku_atomic)
add_test(NAME connect_tests COMMAND connect_tests)

# resolver_tests / resolve_tests (P15): the pure Tier-B/C scorer and the pure
# resolve engine, ported 1:1 from resolver.rs / resolve.rs. No curl/sqlite/tui —
# they link only the leaf shigoku_resolve + domain, so they run everywhere.
add_executable(resolver_tests tests/resolver_tests.cpp)
target_link_libraries(resolver_tests PRIVATE
  shigoku_resolve shigoku_domain shigoku_flags shigoku_thirdparty
  Threads::Threads shigoku_atomic)
add_test(NAME resolver_tests COMMAND resolver_tests)

add_executable(resolve_tests tests/resolve_tests.cpp)
target_link_libraries(resolve_tests PRIVATE
  shigoku_resolve shigoku_domain shigoku_flags shigoku_thirdparty
  Threads::Threads shigoku_atomic)
add_test(NAME resolve_tests COMMAND resolve_tests)

# store_tests (P9): resume rule + watched ratio + recompute + record_finish +
# migration idempotence + the resume DoD, in-memory or a tmp file. Links the
# store lib (+ sqlite); no curl/tui needed (the store is a leaf on domain).
if(SHIGOKU_HAVE_SQLITE3)
  add_executable(store_tests tests/store_tests.cpp)
  target_link_libraries(store_tests PRIVATE
    shigoku_store shigoku_domain shigoku_flags shigoku_thirdparty
    shigoku::sqlite3 Threads::Threads shigoku_atomic)
  add_test(NAME store_tests COMMAND store_tests)
endif()

# http_tests (P2) additionally links the http lib + curl. Only when curl is
# present (the lib itself is gated on SHIGOKU_HAVE_CURL).
if(SHIGOKU_HAVE_CURL)
  add_executable(http_tests tests/http_tests.cpp)
  target_link_libraries(http_tests PRIVATE
    shigoku_http shigoku_domain shigoku_flags shigoku_thirdparty
    shigoku::curl Threads::Threads shigoku_atomic)
  add_test(NAME http_tests COMMAND http_tests)
endif()

# download_tests (P35 slice 1): the headless download core over a scripted
# loopback server — .part/atomic-rename law, Range resume (206 append / 200
# restart / 416 drop), kill-mid-transfer and cancel leave only the .part, no
# 4 MiB cap on the streaming path. Needs curl for the http client.
if(SHIGOKU_HAVE_CURL)
  add_executable(download_tests tests/download_tests.cpp)
  target_link_libraries(download_tests PRIVATE
    shigoku_download shigoku_http shigoku_domain shigoku_flags shigoku_thirdparty
    shigoku::curl Threads::Threads shigoku_atomic)
  add_test(NAME download_tests COMMAND download_tests)
  set_tests_properties(download_tests PROPERTIES TIMEOUT 60)
endif()

# semver_tests (P28): pure version-parse/compare table (semver.rs tests).
# No curl/sqlite3 — links shigoku_semver only.
add_executable(semver_tests tests/semver_tests.cpp)
target_link_libraries(semver_tests PRIVATE
  shigoku_semver shigoku_flags shigoku_thirdparty Threads::Threads shigoku_atomic)
add_test(NAME semver_tests COMMAND semver_tests)

# updatecheck_tests (P28): cache read/write/parse + is_fresh (updatecheck.rs
# tests) plus a live-fixture pass over fetch_latest/check/latest_fresh through
# the SHIGOKU_UPDATE_URL seam and a loopback OneShotServer (no real GitHub
# hit). Needs curl for the http client.
if(SHIGOKU_HAVE_CURL)
  add_executable(updatecheck_tests tests/updatecheck_tests.cpp)
  target_link_libraries(updatecheck_tests PRIVATE
    shigoku_updatecheck shigoku_http shigoku_semver shigoku_domain shigoku_flags
    shigoku_thirdparty shigoku::curl Threads::Threads shigoku_atomic)
  add_test(NAME updatecheck_tests COMMAND updatecheck_tests)
endif()

# anilist_tests (P3, +P20 sync): golden fixture parsing + live-shaped
# HTTP-error/empty tests over a loopback fixture server. SHIGOKU_TEST_FIXTURES_DIR
# points the binary at tests/fixtures regardless of ctest's working directory.
# Gated on both curl and sqlite3 now: shigoku_anilist links shigoku_store (P20).
if(SHIGOKU_HAVE_CURL AND SHIGOKU_HAVE_SQLITE3)
  add_executable(anilist_tests tests/anilist_tests.cpp)
  target_compile_definitions(anilist_tests PRIVATE
    SHIGOKU_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  target_link_libraries(anilist_tests PRIVATE
    shigoku_anilist shigoku_http shigoku_store shigoku_domain shigoku_flags shigoku_thirdparty
    shigoku::curl Threads::Threads shigoku_atomic)
  add_test(NAME anilist_tests COMMAND anilist_tests)
endif()

# login_tests (P19): complete_login's extract/floor-check/verify/save state
# machine + authorize_url/normalize_paste/param boundary-anchoring, against a
# fake Verifier (no network). Links login + auth only; gated the same as
# shigoku_login (transitively shigoku_anilist, shigoku_store) is.
if(SHIGOKU_HAVE_CURL AND SHIGOKU_HAVE_SQLITE3)
  add_executable(login_tests tests/login_tests.cpp)
  target_link_libraries(login_tests PRIVATE
    shigoku_login shigoku_auth shigoku_anilist shigoku_store shigoku_domain shigoku_flags
    shigoku_thirdparty Threads::Threads shigoku_atomic)
  add_test(NAME login_tests COMMAND login_tests)
endif()

# mal_login_tests (P31 §9.1 slice 2): complete_mal_login/refresh_mal_token's
# state machine + PKCE minting/authorize_url/form_encode/parse_token_response,
# against a fake Exchanger (no network). No sabigoku golden reference (see
# mal_login.hpp). Gated the same as shigoku_mal_login.
if(SHIGOKU_HAVE_CURL AND SHIGOKU_HAVE_SQLITE3)
  add_executable(mal_login_tests tests/mal_login_tests.cpp)
  target_link_libraries(mal_login_tests PRIVATE
    shigoku_mal_login shigoku_auth shigoku_domain shigoku_flags
    shigoku_thirdparty Threads::Threads shigoku_atomic)
  add_test(NAME mal_login_tests COMMAND mal_login_tests)
endif()

# mal_tests (P31 §9.1 slice 3): the status-map branch table (both directions)
# + get/put request+response goldens, offline (asserts on the built Request,
# no socket). No sabigoku golden reference (see mal.hpp). Gated the same as
# shigoku_mal.
if(SHIGOKU_HAVE_CURL AND SHIGOKU_HAVE_SQLITE3)
  add_executable(mal_tests tests/mal_tests.cpp)
  target_link_libraries(mal_tests PRIVATE
    shigoku_mal shigoku_domain shigoku_flags shigoku_thirdparty)
  add_test(NAME mal_tests COMMAND mal_tests)
endif()

# sync_tests (P20): run_sync/flush_push/push_dirty against a FakeAni/RaceAni
# (no network) + a RecordingSleeper (no real waits) — the 2s/60s schedule,
# 401/429 branching, contended/ROD-498 races. Gated the same as shigoku_login
# (transitively shigoku_anilist, shigoku_store) is.
if(SHIGOKU_HAVE_CURL AND SHIGOKU_HAVE_SQLITE3)
  add_executable(sync_tests tests/sync_tests.cpp)
  target_link_libraries(sync_tests PRIVATE
    shigoku_sync shigoku_anilist shigoku_auth shigoku_store shigoku_domain shigoku_flags
    shigoku_thirdparty Threads::Threads shigoku_atomic)
  add_test(NAME sync_tests COMMAND sync_tests)
endif()

# mal_mirror_tests (P31 §9.1 slice 4): push_mirror against a FakeMalMirror
# (no network) — NULL-mal_id skip (store-level), dirty-set push + snapshot
# advance, 401/429 branching, MAL-deleted-entry (nullopt) handling, and the
# ROD-498-equivalent guard (MAL's live value moved since our last accepted
# push). Gated the same as shigoku_sync.
if(SHIGOKU_HAVE_CURL AND SHIGOKU_HAVE_SQLITE3)
  add_executable(mal_mirror_tests tests/mal_mirror_tests.cpp)
  target_link_libraries(mal_mirror_tests PRIVATE
    shigoku_mal_mirror shigoku_mal shigoku_auth shigoku_store shigoku_domain shigoku_flags
    shigoku_thirdparty Threads::Threads shigoku_atomic)
  add_test(NAME mal_mirror_tests COMMAND mal_mirror_tests)
endif()

# cli_tests (P27): the pure CLI surface — parse dispatch, sync/connect/search/
# pick renderers, classify_pick, paste_line, fetch/player error copy (cli.rs
# tests) + the play_flow exit table with a fake provider + scripted picker
# (main.rs play_flow mod; no mpv spawn — the exit cases return before launch).
# Links shigoku_cli (→ player/aniskip/store/sync/login). Gated the same.
if(SHIGOKU_HAVE_CURL AND SHIGOKU_HAVE_SQLITE3)
  add_executable(cli_tests tests/cli_tests.cpp)
  # shigoku_download directly too (P35 slice 4): the play-prefers-local test
  # plants fixtures via download::ensure_parent_dirs.
  target_link_libraries(cli_tests PRIVATE
    shigoku_cli shigoku_login shigoku_sync shigoku_player shigoku_aniskip shigoku_anilist
    shigoku_auth shigoku_store shigoku_config shigoku_domain shigoku_flags shigoku_thirdparty
    shigoku_download shigoku::curl shigoku::sqlite3 Threads::Threads shigoku_atomic)
  add_test(NAME cli_tests COMMAND cli_tests)
endif()

# loopback_tests (P19): the OAuth callback listener end-to-end over real
# loopback sockets (ephemeral port) — relay->callback completes login,
# bad-state warns and keeps waiting, page byte-shape (scrub-first, relay
# carries no scrub), cancel wakes a blocked serve(). Links loopback + login +
# auth + nonce; gated transitively the same as shigoku_loopback.
if(SHIGOKU_HAVE_CURL AND SHIGOKU_HAVE_SQLITE3)
  add_executable(loopback_tests tests/loopback_tests.cpp)
  target_link_libraries(loopback_tests PRIVATE
    shigoku_loopback shigoku_login shigoku_auth shigoku_anilist shigoku_store shigoku_nonce
    shigoku_domain shigoku_flags shigoku_thirdparty Threads::Threads shigoku_atomic)
  add_test(NAME loopback_tests COMMAND loopback_tests)
endif()

# mal_loopback_tests (P31 §9.1 slice 2): the MAL OAuth callback listener
# end-to-end over real loopback sockets (ephemeral port) — code+state
# completes login, a non-callback path is ignored (no relay hop for this
# grant type), bad-state warns and keeps waiting without reaching the
# exchanger, missing code, cancel wakes a blocked serve(), page shape. No
# sabigoku golden reference. Links mal_loopback + login (query-param parser
# only) + mal_login + auth + nonce; gated transitively the same as
# shigoku_mal_loopback.
if(SHIGOKU_HAVE_CURL AND SHIGOKU_HAVE_SQLITE3)
  add_executable(mal_loopback_tests tests/mal_loopback_tests.cpp)
  target_link_libraries(mal_loopback_tests PRIVATE
    shigoku_mal_loopback shigoku_mal_login shigoku_login shigoku_auth shigoku_anilist
    shigoku_store shigoku_nonce shigoku_domain shigoku_flags shigoku_thirdparty Threads::Threads
    shigoku_atomic)
  add_test(NAME mal_loopback_tests COMMAND mal_loopback_tests)
endif()

# anilist_live_smoke (P3 DoD): real network hit, gated on SHIGOKU_LIVE=1 at
# runtime (not registered with ctest — scripts/live_smoke.sh drives it).
if(SHIGOKU_HAVE_CURL AND SHIGOKU_HAVE_SQLITE3)
  add_executable(anilist_live_smoke tests/anilist_live_smoke.cpp)
  target_link_libraries(anilist_live_smoke PRIVATE
    shigoku_anilist shigoku_http shigoku_store shigoku_domain shigoku_flags shigoku_thirdparty
    shigoku::curl Threads::Threads shigoku_atomic)
endif()

# hls_tests (P12 seed + P23): providers/hls.rs `mod tests` ported 1:1 —
# parse_master_playlist, select_variant's cap policy, join_url. Pure/offline,
# no curl needed.
add_executable(hls_tests tests/hls_tests.cpp)
target_link_libraries(hls_tests PRIVATE
  shigoku_hls shigoku_domain shigoku_flags shigoku_thirdparty Threads::Threads shigoku_atomic)
add_test(NAME hls_tests COMMAND hls_tests)

# senshi_tests (P4): the ~20-test golden contract ported 1:1 from senshi.rs,
# offline (pure helpers + fixture parsing) plus a few transport cases over a
# loopback fixture server.
if(SHIGOKU_HAVE_CURL)
  add_executable(senshi_tests tests/senshi_tests.cpp)
  target_compile_definitions(senshi_tests PRIVATE
    SHIGOKU_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  target_link_libraries(senshi_tests PRIVATE
    shigoku_senshi shigoku_http shigoku_domain shigoku_flags shigoku_thirdparty
    shigoku::curl Threads::Threads shigoku_atomic)
  add_test(NAME senshi_tests COMMAND senshi_tests)
endif()

# manga_mangadex_tests (MANGA_PLAN MG-1): URL builders exact-string, the
# three parsers over tests/fixtures/mangadex/, chapter_less / dedupe /
# next_offset pinned rules. Fully offline (no fixture server; the transport
# loop's math is tested pure via detail::next_offset).
if(SHIGOKU_HAVE_CURL)
  add_executable(manga_mangadex_tests tests/manga_mangadex_tests.cpp)
  target_compile_definitions(manga_mangadex_tests PRIVATE
    SHIGOKU_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  target_link_libraries(manga_mangadex_tests PRIVATE
    shigoku_mangadex shigoku_http shigoku_flags shigoku_thirdparty
    shigoku::curl Threads::Threads shigoku_atomic)
  add_test(NAME manga_mangadex_tests COMMAND manga_mangadex_tests)
endif()

# manga_flow_tests (MANGA_PLAN MG-3): pages fetch-to-cache over a loopback
# fixture server (.part/.complete atomicity, re-entry skip, node rotation),
# the viewer argv table + report parse, config-lite, and the mapp read-flow
# state machine driven through tick() with hand-made events.
if(SHIGOKU_HAVE_CURL AND SHIGOKU_HAVE_SQLITE3)
  add_executable(manga_flow_tests tests/manga_flow_tests.cpp)
  target_link_libraries(manga_flow_tests PRIVATE
    shigoku_manga_app shigoku_manga_flow shigoku_mstore shigoku_tui shigoku_http
    shigoku_mangadex shigoku_flags shigoku_thirdparty
    shigoku::curl shigoku::sqlite3 Threads::Threads shigoku_atomic)
  add_test(NAME manga_flow_tests COMMAND manga_flow_tests)
endif()

# manga_mangadex_live_smoke (MG-1 DoD): real api.mangadex.org hit, gated on
# SHIGOKU_LIVE=1 at runtime (not registered with ctest).
if(SHIGOKU_HAVE_CURL)
  add_executable(manga_mangadex_live_smoke tests/manga_mangadex_live_smoke.cpp)
  target_link_libraries(manga_mangadex_live_smoke PRIVATE
    shigoku_mangadex shigoku_http shigoku_flags shigoku_thirdparty
    shigoku::curl Threads::Threads shigoku_atomic)
endif()

# manga_weebcentral_tests (MANGA_PLAN MG-6b): URL builders exact-string, the
# three fragment parsers over tests/fixtures/weebcentral/, the ULID vet /
# entity unescape / challenge sniff. Fully offline.
if(SHIGOKU_HAVE_CURL)
  add_executable(manga_weebcentral_tests tests/manga_weebcentral_tests.cpp)
  target_compile_definitions(manga_weebcentral_tests PRIVATE
    SHIGOKU_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  target_link_libraries(manga_weebcentral_tests PRIVATE
    shigoku_weebcentral shigoku_http shigoku_flags shigoku_thirdparty
    shigoku::curl Threads::Threads shigoku_atomic)
  add_test(NAME manga_weebcentral_tests COMMAND manga_weebcentral_tests)
endif()

# manga_weebcentral_live_smoke (MG-6b DoD): real weebcentral.com hit incl. a
# page-CDN byte fetch, gated on SHIGOKU_LIVE=1 at runtime (not ctest).
if(SHIGOKU_HAVE_CURL)
  add_executable(manga_weebcentral_live_smoke tests/manga_weebcentral_live_smoke.cpp)
  target_link_libraries(manga_weebcentral_live_smoke PRIVATE
    shigoku_weebcentral shigoku_http shigoku_flags shigoku_thirdparty
    shigoku::curl Threads::Threads shigoku_atomic)
endif()

# manga_dynasty_tests (MANGA_PLAN MG-6d): URL builders exact-string, the two
# JSON parsers + the search HTML parser over tests/fixtures/dynasty/, the slug
# vet and the permalink→chapter-number rules. Fully offline.
if(SHIGOKU_HAVE_CURL)
  add_executable(manga_dynasty_tests tests/manga_dynasty_tests.cpp)
  target_compile_definitions(manga_dynasty_tests PRIVATE
    SHIGOKU_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  target_link_libraries(manga_dynasty_tests PRIVATE
    shigoku_dynasty shigoku_http shigoku_flags shigoku_thirdparty
    shigoku::curl Threads::Threads shigoku_atomic)
  add_test(NAME manga_dynasty_tests COMMAND manga_dynasty_tests)
endif()

# manga_dynasty_live_smoke (MG-6d DoD): real dynasty-scans.com hit incl. a
# WebP page byte fetch, gated on SHIGOKU_LIVE=1 at runtime (not ctest).
if(SHIGOKU_HAVE_CURL)
  add_executable(manga_dynasty_live_smoke tests/manga_dynasty_live_smoke.cpp)
  target_link_libraries(manga_dynasty_live_smoke PRIVATE
    shigoku_dynasty shigoku_http shigoku_flags shigoku_thirdparty
    shigoku::curl Threads::Threads shigoku_atomic)
endif()

# manga_nhentai_tests (MANGA_PLAN MG-6e): URL builders exact-string, the three
# JSON parsers over tests/fixtures/nhentai/, the id/path vets, the
# gallery≡oneshot adapter, and the nsfw_sources gate. Fully offline.
if(SHIGOKU_HAVE_CURL)
  add_executable(manga_nhentai_tests tests/manga_nhentai_tests.cpp)
  target_compile_definitions(manga_nhentai_tests PRIVATE
    SHIGOKU_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  target_link_libraries(manga_nhentai_tests PRIVATE
    shigoku_nhentai shigoku_http shigoku_flags shigoku_thirdparty
    shigoku::curl Threads::Threads shigoku_atomic)
  add_test(NAME manga_nhentai_tests COMMAND manga_nhentai_tests)
endif()

# mstore_tests (MANGA_PLAN MG-4): the manga.db ladder (fresh create,
# idempotent re-open, foreign-file refusal), the library query's ordering +
# counters, progress round-trips incl. the exact-undo pair, the chapter cache,
# and the pure helpers (split_scoped, next_unread_index, new_chapter_ids).
# Links the store lib (+ sqlite) only — no curl, no tui.
if(SHIGOKU_HAVE_SQLITE3)
  add_executable(mstore_tests tests/mstore_tests.cpp)
  target_link_libraries(mstore_tests PRIVATE
    shigoku_mstore shigoku_flags shigoku_thirdparty
    shigoku::sqlite3 Threads::Threads shigoku_atomic)
  add_test(NAME mstore_tests COMMAND mstore_tests)
endif()

# manga_sync_tests (MANGA_PLAN MG-5): the AniList push rules — the floor of a
# display chapter number, the never-decrease guard, and push_chapter driven
# against a scripted MangaListClient (no socket: a failed read must be shown
# to write nothing). Gated like shigoku_manga_sync is.
if(SHIGOKU_HAVE_CURL AND SHIGOKU_HAVE_SQLITE3)
  add_executable(manga_sync_tests tests/manga_sync_tests.cpp)
  target_link_libraries(manga_sync_tests PRIVATE
    shigoku_manga_sync shigoku_anilist shigoku_store shigoku_domain shigoku_flags
    shigoku_thirdparty Threads::Threads shigoku_atomic)
  add_test(NAME manga_sync_tests COMMAND manga_sync_tests)
endif()

# manga_nhentai_live_smoke (MG-6e DoD): real nhentai.net hit incl. the cdn
# pool + a page byte fetch, gated on SHIGOKU_LIVE=1 at runtime (not ctest).
if(SHIGOKU_HAVE_CURL)
  add_executable(manga_nhentai_live_smoke tests/manga_nhentai_live_smoke.cpp)
  target_link_libraries(manga_nhentai_live_smoke PRIVATE
    shigoku_nhentai shigoku_http shigoku_flags shigoku_thirdparty
    shigoku::curl Threads::Threads shigoku_atomic)
endif()

# senshi_live_smoke (P4 DoD): real network hit, gated on SHIGOKU_LIVE=1 at
# runtime (not registered with ctest — scripts/live_smoke.sh drives it).
if(SHIGOKU_HAVE_CURL)
  add_executable(senshi_live_smoke tests/senshi_live_smoke.cpp)
  target_link_libraries(senshi_live_smoke PRIVATE
    shigoku_senshi shigoku_http shigoku_domain shigoku_flags shigoku_thirdparty
    shigoku::curl Threads::Threads shigoku_atomic)
endif()

# anibd_tests (P12a): every anibd #[test] ported 1:1 — pure parsers/guards
# offline plus transport cases (SSRF skip, percent-encoded data param, 403 ->
# taxonomy) over a loopback fixture server. No fixture files (JSON is inline).
if(SHIGOKU_HAVE_CURL)
  add_executable(anibd_tests tests/anibd_tests.cpp)
  target_link_libraries(anibd_tests PRIVATE
    shigoku_anibd shigoku_http shigoku_domain shigoku_flags shigoku_thirdparty
    shigoku::curl Threads::Threads shigoku_atomic)
  add_test(NAME anibd_tests COMMAND anibd_tests)
endif()

# anibd_live_smoke (P12a DoD): real network resolve against the verified-alive
# upstream, gated on SHIGOKU_LIVE=1 at runtime (not a ctest — driven by hand).
if(SHIGOKU_HAVE_CURL)
  add_executable(anibd_live_smoke tests/anibd_live_smoke.cpp)
  target_link_libraries(anibd_live_smoke PRIVATE
    shigoku_anibd shigoku_http shigoku_domain shigoku_flags shigoku_thirdparty
    shigoku::curl Threads::Threads shigoku_atomic)
endif()

# megaplay_tests (P13): every megaplay #[test] ported 1:1 — pure parsers/guards
# offline (data-id scrape, getSources map, softsub pick, cue-cap) plus transport
# cases (episodes probe, two-hop resolve, private-host sub drop) over a loopback
# fixture server. No fixture files (JSON is inline).
if(SHIGOKU_HAVE_CURL)
  add_executable(megaplay_tests tests/megaplay_tests.cpp)
  target_link_libraries(megaplay_tests PRIVATE
    shigoku_megaplay shigoku_http shigoku_domain shigoku_flags shigoku_thirdparty
    shigoku::curl Threads::Threads shigoku_atomic)
  add_test(NAME megaplay_tests COMMAND megaplay_tests)
endif()

# anidbapp_tests (P25): every anidbapp #[test] ported 1:1 — pure parsers/guards
# offline (card/slug/entity/year parsing, external ids, challenge gate, HLS
# extraction, episode sort/dedup/offset) plus transport cases (Slice 3) over a
# loopback fixture server. No fixture files (HTML/JSON inline).
if(SHIGOKU_HAVE_CURL)
  add_executable(anidbapp_tests tests/anidbapp_tests.cpp)
  target_link_libraries(anidbapp_tests PRIVATE
    shigoku_anidbapp shigoku_resolve shigoku_hls shigoku_http shigoku_domain
    shigoku_flags shigoku_thirdparty shigoku::curl Threads::Threads shigoku_atomic)
  add_test(NAME anidbapp_tests COMMAND anidbapp_tests)
endif()

# anidbapp_live_smoke (P25 DoD): real resolve against anidb.app through the REAL
# Chrome-shaped TLS handshake (needs libcurl-impersonate at runtime), gated on
# SHIGOKU_LIVE=1 (not a ctest — driven by hand). A stock ClientHello 403s here.
if(SHIGOKU_HAVE_CURL)
  add_executable(anidbapp_live_smoke tests/anidbapp_live_smoke.cpp)
  target_link_libraries(anidbapp_live_smoke PRIVATE
    shigoku_anidbapp shigoku_hls shigoku_http shigoku_domain shigoku_flags
    shigoku_thirdparty shigoku::curl Threads::Threads shigoku_atomic)
endif()

# anilibria_tests: pure parsers/guards offline (search/release JSON, ordinal
# labels, quality ladder, cover joins) plus transport cases over a loopback
# fixture server. No fixture files (JSON inline).
if(SHIGOKU_HAVE_CURL)
  add_executable(anilibria_tests tests/anilibria_tests.cpp)
  target_link_libraries(anilibria_tests PRIVATE
    shigoku_anilibria shigoku_http shigoku_domain shigoku_flags
    shigoku_thirdparty shigoku::curl Threads::Threads shigoku_atomic)
  add_test(NAME anilibria_tests COMMAND anilibria_tests)
endif()

# anilibria_live_smoke: real search/episodes/resolve against the live
# AniLiberty API, gated on SHIGOKU_LIVE=1 (not a ctest — driven by hand).
if(SHIGOKU_HAVE_CURL)
  add_executable(anilibria_live_smoke tests/anilibria_live_smoke.cpp)
  target_link_libraries(anilibria_live_smoke PRIVATE
    shigoku_anilibria shigoku_http shigoku_domain shigoku_flags
    shigoku_thirdparty shigoku::curl Threads::Threads shigoku_atomic)
endif()

# megaplay_live_smoke (P13 DoD): real network resolve against the live upstream,
# gated on SHIGOKU_LIVE=1 at runtime (not a ctest — driven by hand).
if(SHIGOKU_HAVE_CURL)
  add_executable(megaplay_live_smoke tests/megaplay_live_smoke.cpp)
  target_link_libraries(megaplay_live_smoke PRIVATE
    shigoku_megaplay shigoku_http shigoku_domain shigoku_flags shigoku_thirdparty
    shigoku::curl Threads::Threads shigoku_atomic)
endif()

# player_tests (P5): argv goldens + retry law + IPC watcher over a socketpair.
# The lib pulls in http (guard) + curl transitively; link them explicitly too.
# P39 adds the stub-backend walks (stub_mplayer.py / stub_qmplay2.py), hence
# the fixtures dir.
if(SHIGOKU_HAVE_CURL)
  add_executable(player_tests tests/player_tests.cpp)
  target_link_libraries(player_tests PRIVATE
    shigoku_player shigoku_http shigoku_domain shigoku_flags shigoku_thirdparty
    shigoku::curl Threads::Threads shigoku_atomic)
  target_compile_definitions(player_tests PRIVATE
    SHIGOKU_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  add_test(NAME player_tests COMMAND player_tests)
endif()

# aniskip_tests (P22): aniskip.rs mod tests ported 1:1 — skip_mode parse,
# times_from_body/build_opts/episode_number pure helpers, ensure_script
# rewrite, prepare() early-outs. Needs shigoku_player for SkipScript.
if(SHIGOKU_HAVE_CURL)
  add_executable(aniskip_tests tests/aniskip_tests.cpp)
  target_link_libraries(aniskip_tests PRIVATE
    shigoku_aniskip shigoku_player shigoku_http shigoku_domain shigoku_flags
    shigoku_thirdparty shigoku::curl Threads::Threads shigoku_atomic)
  add_test(NAME aniskip_tests COMMAND aniskip_tests)
endif()

# proxy_tests (P12): the proxy.rs golden suite ported 1:1 — pure codec/scan/
# rewrite helpers offline, plus fetch_upstream guard/injection, the lifecycle
# stop-wakes-accept, and the token gate driven over a real loopback socket.
if(SHIGOKU_HAVE_CURL)
  add_executable(proxy_tests tests/proxy_tests.cpp)
  target_link_libraries(proxy_tests PRIVATE
    shigoku_proxy shigoku_http shigoku_hls shigoku_nonce shigoku_domain
    shigoku_flags shigoku_thirdparty shigoku::curl Threads::Threads shigoku_atomic)
  add_test(NAME proxy_tests COMMAND proxy_tests)
endif()

# cells_tests / input_tests / toast_tests / cover_pool_tests (P6/P17b): pure
# TUI-core tests (no curl). They link shigoku_tui_render + domain; the
# render/decode/queue paths are offline and build even without curl
# (shigoku_tui itself is curl-gated, P7). cover_pool_tests covers the P17b
# CoverPool coordinator + render store — pure, no store/curl needed either.
foreach(_p6test cells_tests input_tests toast_tests cover_pool_tests)
  add_executable(${_p6test} tests/${_p6test}.cpp)
  target_link_libraries(${_p6test} PRIVATE
    shigoku_tui_render shigoku_domain shigoku_flags shigoku_thirdparty
    Threads::Threads shigoku_atomic)
  add_test(NAME ${_p6test} COMMAND ${_p6test})
endforeach()

# app_tests (P7): pure-helper unit layer — pane_split, classify_tier, the
# ProviderError/PlayError -> toast mapping, idle_help's Detail arm. No worker/
# network/pty involved (pty_walk_tests.cpp is the end-to-end layer). Links
# shigoku_tui, so it needs sqlite too (the store rides that lib since P9).
if(SHIGOKU_HAVE_CURL AND SHIGOKU_HAVE_SQLITE3)
  add_executable(app_tests tests/app_tests.cpp)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # This TU is the biggest in the tree (one Catch2 file over the whole app
    # layer) and GCC's default GC pacing peaks past what a 1GB box has —
    # cc1plus gets OOM-killed. Collect aggressively: ~2x compile time for
    # roughly half the peak RSS. GCC-only knob; clang paces itself fine.
    target_compile_options(app_tests PRIVATE
      --param=ggc-min-expand=10 --param=ggc-min-heapsize=32768)
  endif()
  target_link_libraries(app_tests PRIVATE
    shigoku_tui shigoku_store shigoku_domain shigoku_flags shigoku_thirdparty
    shigoku::curl shigoku::sqlite3 Threads::Threads shigoku_atomic)
  add_test(NAME app_tests COMMAND app_tests)
endif()

# resolve_transport_tests (P15): the EpisodeSession walk transport driven
# end-to-end against a real in-memory Store + a two-fixture registry — store
# write ownership (bind/absence/route/pin), tier-B/C scoring, pin-flip +
# fallback, the staleness guard, and prewarm mint. Links shigoku_tui (transport)
# + store + resolve; curl+sqlite-gated like app_tests.
if(SHIGOKU_HAVE_CURL AND SHIGOKU_HAVE_SQLITE3)
  add_executable(resolve_transport_tests tests/resolve_transport_tests.cpp)
  target_link_libraries(resolve_transport_tests PRIVATE
    shigoku_tui shigoku_store shigoku_resolve shigoku_domain shigoku_flags
    shigoku_thirdparty shigoku::curl shigoku::sqlite3 Threads::Threads shigoku_atomic)
  add_test(NAME resolve_transport_tests COMMAND resolve_transport_tests)
  set_tests_properties(resolve_transport_tests PROPERTIES TIMEOUT 30)
endif()

# covers_tests (P8): kitty emitter goldens, cover geometry, decoded LRU, PNG
# decode + crop/resize, the CoverState decision table, and the CSI 14t parse.
# Links shigoku_tui (covers.cpp — decode/crop/LRU/CoverState) + the render core
# (kitty/cover_geom/cover_probe). curl-gated because covers.cpp pulls http;
# also sqlite-gated since P9 (shigoku_tui carries the store).
if(SHIGOKU_HAVE_CURL AND SHIGOKU_HAVE_SQLITE3)
  add_executable(covers_tests tests/covers_tests.cpp)
  target_compile_definitions(covers_tests PRIVATE
    SHIGOKU_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    SHIGOKU_HAVE_WEBP=$<BOOL:${SHIGOKU_HAVE_WEBP}>)
  target_link_libraries(covers_tests PRIVATE
    shigoku_tui shigoku_tui_render shigoku_store shigoku_domain shigoku_flags
    shigoku_thirdparty shigoku::curl shigoku::sqlite3 Threads::Threads shigoku_atomic)
  add_test(NAME covers_tests COMMAND covers_tests)
endif()

# discover_tests (P17a): the Discover card-grid STATE suite — feed filing by
# axis, stale/duplicate discard, MAX_FEED_ROWS exhaustion, catalog_cache upsert,
# wanted_fetch, sticky feed error, nav wrap/clamp, cursor/scroll survival, the
# glyph table + format labels. Links shigoku_tui (DiscoverState::on_feed →
# store) + the render core (the pure state/geometry/glyphs) + the store.
if(SHIGOKU_HAVE_CURL AND SHIGOKU_HAVE_SQLITE3)
  add_executable(discover_tests tests/discover_tests.cpp)
  target_link_libraries(discover_tests PRIVATE
    shigoku_tui shigoku_tui_render shigoku_store shigoku_domain shigoku_flags
    shigoku_thirdparty shigoku::curl shigoku::sqlite3 Threads::Threads shigoku_atomic)
  add_test(NAME discover_tests COMMAND discover_tests)
endif()

# pty_walk_tests (P7 DoD, §8 "the only integration test"): forkpty()s itself
# (--child re-invocation) to drive the real event loop against a
# FixtureProvider + stub mpv script, offline. Needs shigoku_tui (curl+sqlite-
# gated, links anilist/player/provider/store transitively) + -lutil for
# forkpty/openpty.
if(SHIGOKU_HAVE_CURL AND SHIGOKU_HAVE_SQLITE3)
  add_executable(pty_walk_tests tests/pty_walk_tests.cpp)
  target_compile_definitions(pty_walk_tests PRIVATE
    SHIGOKU_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  target_link_libraries(pty_walk_tests PRIVATE
    shigoku_tui shigoku_store shigoku_domain shigoku_flags shigoku_thirdparty
    shigoku::curl shigoku::sqlite3 Threads::Threads shigoku_atomic util)
  add_test(NAME pty_walk_tests COMMAND pty_walk_tests)
  set_tests_properties(pty_walk_tests PROPERTIES TIMEOUT 45)
endif()

# --- Exhaustiveness fence: compile-fail tests (A2, P1 DoD) -----------------
#
# fence_fail.cpp MUST NOT compile under our flag set. We register two ctest
# cases that invoke the compiler and mark them WILL_FAIL, so a green suite
# proves the fence is live: if either case starts compiling, the fence broke.
set(_fence_flags
  -std=c++23 -Wall -Wextra -Werror -Werror=switch -Werror=switch-enum
  -I${CMAKE_CURRENT_SOURCE_DIR}/third_party
  -fsyntax-only)

add_test(NAME fence_missing_switch_arm
  COMMAND ${CMAKE_CXX_COMPILER} ${_fence_flags}
          -DSHIGOKU_FENCE_CASE=1 ${CMAKE_CURRENT_SOURCE_DIR}/tests/fence_fail.cpp)
set_tests_properties(fence_missing_switch_arm PROPERTIES WILL_FAIL TRUE)

add_test(NAME fence_missing_visit_arm
  COMMAND ${CMAKE_CXX_COMPILER} ${_fence_flags}
          -DSHIGOKU_FENCE_CASE=2 ${CMAKE_CURRENT_SOURCE_DIR}/tests/fence_fail.cpp)
set_tests_properties(fence_missing_visit_arm PROPERTIES WILL_FAIL TRUE)

# The K-2 fence (P15): a switch over WalkOrigin missing the PinFlip arm must not
# compile — proving no future edit can silently fold a pin flip into the
# forced-preferred continuation path (the exact conflation the K-2 bug was).
add_test(NAME fence_missing_walk_origin_arm
  COMMAND ${CMAKE_CXX_COMPILER} ${_fence_flags}
          -DSHIGOKU_FENCE_CASE=3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/fence_fail.cpp)
set_tests_properties(fence_missing_walk_origin_arm PROPERTIES WILL_FAIL TRUE)
