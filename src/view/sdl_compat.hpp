// sdl_compat.hpp — the ONLY interface between shigoku-view and SDL. The SDL
// headers are seen only by sdl_compat.cpp (pimpl), so main.cpp and pager.cpp
// stay SDL-free and the ifdef surface for the SDL2/SDL3 split lives in exactly
// one .cpp — main.cpp doesn't need to see SDL either.
//
// SDL flavor (rev 2.3): SDL2 is the default (Cocoa backend on ppc darwin);
// -DVIEWER_SDL3 selects SDL3 (X11-only on that box, so opt-in). The delta is
// a handful of renamed calls, all inside sdl_compat.cpp. SDL3 is NOT verified
// on this dev box (no SDL3 staged) — see NOTES.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "pager.hpp"  // Key, Rect.

namespace shigoku::view {

// A pumped window event, already normalized (SDL key syms mapped to pager::Key).
struct ViewEvent {
  enum class Type {
    None,    // timed out — no event.
    Key,     // `key` is set.
    Resize,  // window resized; `w`/`h` are the new drawable size.
    Quit,    // window close / SDL_QUIT.
  } type = Type::None;
  Key key = Key::Other;
  int w = 0;
  int h = 0;
};

// The SDL window+renderer+texture, RAII. One current texture (the scaled page);
// set_texture replaces it. All drawing is software-renderer friendly (works
// under SDL_VIDEODRIVER=dummy and on the PPC target's non-GL path).
class Backend {
 public:
  // Create a window `w`x`h` titled `title`. nullopt on any SDL failure (the
  // caller prints SDL_GetError-derived context via last_error()).
  [[nodiscard]] static std::optional<Backend> create(const std::string& title,
                                                     int w, int h);

  Backend(Backend&&) noexcept;
  Backend& operator=(Backend&&) noexcept;
  Backend(const Backend&) = delete;
  Backend& operator=(const Backend&) = delete;
  ~Backend();

  [[nodiscard]] int width() const;
  [[nodiscard]] int height() const;

  void set_title(const std::string& title);

  // Enter/leave fullscreen. Idempotent, and `on` is a target state rather than
  // a flip so a window the system took in or out of fullscreen behind our back
  // (the green button, a space switch) cannot leave the caller inverted.
  void set_fullscreen(bool on);

  // Whether the WINDOW is fullscreen right now — read from SDL, not from a
  // flag of our own, for the same reason.
  [[nodiscard]] bool is_fullscreen() const;

  // Block up to `timeout_ms` for the next event (<=0 polls once). Returns
  // Type::None on timeout. Key events are pre-mapped; motion/other are dropped.
  // macOS menu commands arrive here as Key events too — same path as a keypress.
  [[nodiscard]] ViewEvent wait_event(int timeout_ms);

  // Replace the current texture with an RGBA32 image (w*h*4, row-major). The
  // caller pre-scales to the size it wants blitted (upload 1:1).
  void set_texture(const std::uint8_t* rgba, int w, int h);

  // Replace the HUD overlay (RGBA32, alpha-blended over the page at the
  // top-right corner on every render) — or clear it with nullptr / w,h <= 0.
  // A separate texture so page rescale/scroll never touches it.
  void set_overlay(const std::uint8_t* rgba, int w, int h);

  // Clear to black, blit the current texture at `dst` (which may extend past
  // the window for scroll — the renderer clips), present.
  void render(const Rect& dst);

  // Last SDL error captured on a failed create()/op (for diagnostics).
  [[nodiscard]] static std::string last_error();

 private:
  Backend() = default;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace shigoku::view
