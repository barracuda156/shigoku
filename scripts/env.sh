#!/usr/bin/env bash
# Source this before configuring/building on the dev machine where libcurl and
# sqlite3 dev files live under ~/.local (installed without root, from the Ubuntu
# .debs — see NOTES.md P0). No effect on the legacy MacPorts target, where the
# packages are on the default search paths.
#
#   source scripts/env.sh && cmake -S . -B build && cmake --build build

_local="${HOME}/.local"

# pkg-config finds our staged libcurl.pc / sqlite3.pc here.
export PKG_CONFIG_PATH="${_local}/lib/pkgconfig:${PKG_CONFIG_PATH}"

# The ~/.local pkg-config binary needs its own libpkgconf on the loader path.
_pkgconf_lib="${_local}/opt/pkgconf/usr/lib/x86_64-linux-gnu"
[ -d "${_pkgconf_lib}" ] && export LD_LIBRARY_PATH="${_pkgconf_lib}:${LD_LIBRARY_PATH}"

# SDL2 for the manga viewer (MANGA_PLAN MG-2), also staged from the Ubuntu
# .debs without root — but unlike curl/sqlite3 the RUNTIME libs aren't on the
# system either, so the staged libSDL2-2.0.so.0 (and its hard-NEEDED audio
# closure: libpulse/libsndfile/… under the same dir + the pulseaudio/ subdir)
# must be on the loader path, not just the dev symlink. sdl2.pc is in the
# pkgconfig dir above; this puts the runtime .so's where the loader finds them.
_sdl2_lib="${_local}/opt/sdl2/usr/lib/x86_64-linux-gnu"
[ -d "${_sdl2_lib}" ] && export LD_LIBRARY_PATH="${_sdl2_lib}:${_sdl2_lib}/pulseaudio:${LD_LIBRARY_PATH}"

# Let CMake's find_library see the staged dev symlinks (libcurl.so, libsqlite3.so).
export CMAKE_PREFIX_PATH="${_local}:${CMAKE_PREFIX_PATH}"

unset _local _pkgconf_lib _sdl2_lib
