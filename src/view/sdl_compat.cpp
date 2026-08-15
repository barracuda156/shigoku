// sdl_compat.cpp — the SDL implementation behind sdl_compat.hpp. The only TU
// that includes SDL. SDL2 by default; -DVIEWER_SDL3 selects SDL3 (the renamed
// calls are isolated to the small #if blocks below). SDL3 is unverified on the
// dev box (no SDL3 staged) — the branches are written to the documented API
// deltas but compile-checked only where SDL3 is present.
//
// It also owns the macOS menu seam (mac_menu.hpp): the AppKit side calls back
// in here, and a menu command becomes an ordinary event before anything else
// sees it.

#include "sdl_compat.hpp"

#include <string>

#include "mac_menu.hpp"

#if defined(VIEWER_SDL3)
#include <SDL3/SDL.h>
#else
#include <SDL.h>
#endif

namespace shigoku::view {

namespace {
std::string g_last_error;  // best-effort diagnostic; single-threaded viewer.

// The event type registered for menu commands (0 = none registered, which is
// also what a headless run leaves it at). One viewer per process, so a file
// static is the whole story.
Uint32 g_menu_event = 0;

// SDL key sym -> pager::Key. Shift+F is fullscreen, f is fit (the pin);
// F11 and Cmd+Ctrl+F reach the same fullscreen toggle, and +/-/0 the zoom
// ladder with or without the Cmd the macOS menu advertises.
Key map_key(const SDL_Event& e) {
#if defined(VIEWER_SDL3)
  const SDL_Keycode sym = e.key.key;
  const SDL_Keymod mod = e.key.mod;
  const bool shift = (mod & SDL_KMOD_SHIFT) != 0;
  const bool cmd_ctrl = (mod & SDL_KMOD_GUI) != 0 && (mod & SDL_KMOD_CTRL) != 0;
#else
  const SDL_Keycode sym = e.key.keysym.sym;
  const Uint16 mod = e.key.keysym.mod;
  const bool shift = (mod & KMOD_SHIFT) != 0;
  const bool cmd_ctrl = (mod & KMOD_GUI) != 0 && (mod & KMOD_CTRL) != 0;
#endif
  switch (sym) {
    case SDLK_SPACE:     return Key::Space;
    case SDLK_PAGEDOWN:  return Key::PageDown;
    case SDLK_PAGEUP:    return Key::PageUp;
    case SDLK_BACKSPACE: return Key::Backspace;
    case SDLK_j:         return Key::J;
    case SDLK_k:         return Key::K;
    case SDLK_RIGHT:     return Key::ArrowRight;
    case SDLK_LEFT:      return Key::ArrowLeft;
    case SDLK_UP:        return Key::ArrowUp;
    case SDLK_DOWN:      return Key::ArrowDown;
    case SDLK_HOME:      return Key::Home;
    case SDLK_END:       return Key::End;
    case SDLK_f:         return (shift || cmd_ctrl) ? Key::Fullscreen : Key::F;
    case SDLK_F11:       return Key::Fullscreen;
    case SDLK_EQUALS:    return Key::ZoomIn;   // the unshifted '+' key.
    case SDLK_PLUS:      return Key::ZoomIn;
    case SDLK_KP_PLUS:   return Key::ZoomIn;
    case SDLK_MINUS:     return Key::ZoomOut;
    case SDLK_KP_MINUS:  return Key::ZoomOut;
    case SDLK_0:         return Key::ZoomReset;
    case SDLK_KP_0:      return Key::ZoomReset;
    case SDLK_p:         return Key::P;
    case SDLK_q:         return Key::Q;
    case SDLK_ESCAPE:    return Key::Escape;
    default:             return Key::Other;
  }
}

// A menu command is the same intent as the equivalent keypress, so it becomes
// one before it reaches the loop.
Key map_menu_command(int code) {
  switch (static_cast<MenuCommand>(code)) {
    case MenuCommand::Fullscreen: return Key::Fullscreen;
    case MenuCommand::ZoomIn:     return Key::ZoomIn;
    case MenuCommand::ZoomOut:    return Key::ZoomOut;
    case MenuCommand::ZoomReset:  return Key::ZoomReset;
    // Cmd+W: the same "close this window" intent the red button and q
    // already carry (state.quit=true, which writes the resume report).
    case MenuCommand::Close:      return Key::Q;
  }
  return Key::Other;
}

// Menu handler (main thread, inside AppKit's dispatch under SDL's pump): turn
// the command into an SDL event so the one event loop stays the only place
// that acts on input.
void on_menu_command(int code) {
  if (g_menu_event == 0) return;
  SDL_Event e;
  SDL_zero(e);
  e.type = g_menu_event;
  e.user.code = code;
  SDL_PushEvent(&e);
}
}  // namespace

struct Backend::Impl {
  SDL_Window* window = nullptr;
  SDL_Renderer* renderer = nullptr;
  SDL_Texture* texture = nullptr;
  int tex_w = 0;
  int tex_h = 0;
  SDL_Texture* overlay = nullptr;
  int ov_w = 0;
  int ov_h = 0;
};

Backend::Backend(Backend&&) noexcept = default;
Backend& Backend::operator=(Backend&&) noexcept = default;

Backend::~Backend() {
  if (!impl_) return;
  if (impl_->overlay) SDL_DestroyTexture(impl_->overlay);
  if (impl_->texture) SDL_DestroyTexture(impl_->texture);
  if (impl_->renderer) SDL_DestroyRenderer(impl_->renderer);
  if (impl_->window) SDL_DestroyWindow(impl_->window);
  // SDL_Init was done in create(); one viewer per process, so quit here.
  SDL_Quit();
}

std::optional<Backend> Backend::create(const std::string& title, int w, int h) {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    g_last_error = SDL_GetError();
    return std::nullopt;
  }
  auto impl = std::make_unique<Impl>();

#if defined(VIEWER_SDL3)
  impl->window = SDL_CreateWindow(title.c_str(), w, h, SDL_WINDOW_RESIZABLE);
  if (impl->window) impl->renderer = SDL_CreateRenderer(impl->window, nullptr);
#else
  impl->window = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED,
                                  SDL_WINDOWPOS_CENTERED, w, h,
                                  SDL_WINDOW_RESIZABLE);
  if (impl->window) {
    // -1 = first renderer that fits; 0 flags = let SDL pick (accelerated if
    // available, software otherwise — the dummy driver path uses software).
    impl->renderer = SDL_CreateRenderer(impl->window, -1, 0);
  }
#endif
  if (!impl->window || !impl->renderer) {
    g_last_error = SDL_GetError();
    SDL_Quit();
    return std::nullopt;
  }

  // Menu commands ride a registered user event; if registration fails (or the
  // platform has no menu) the seam simply never fires.
  const Uint32 ev = SDL_RegisterEvents(1);
  if (ev != static_cast<Uint32>(-1)) g_menu_event = ev;
  install_menu(&on_menu_command);

  Backend b;
  b.impl_ = std::move(impl);
  return b;
}

int Backend::width() const {
  int w = 0, h = 0;
#if defined(VIEWER_SDL3)
  SDL_GetCurrentRenderOutputSize(impl_->renderer, &w, &h);
#else
  SDL_GetRendererOutputSize(impl_->renderer, &w, &h);
#endif
  return w;
}

int Backend::height() const {
  int w = 0, h = 0;
#if defined(VIEWER_SDL3)
  SDL_GetCurrentRenderOutputSize(impl_->renderer, &w, &h);
#else
  SDL_GetRendererOutputSize(impl_->renderer, &w, &h);
#endif
  return h;
}

void Backend::set_title(const std::string& title) {
  SDL_SetWindowTitle(impl_->window, title.c_str());
}

void Backend::set_fullscreen(bool on) {
#if defined(VIEWER_SDL3)
  SDL_SetWindowFullscreen(impl_->window, on);
#else
  // FULLSCREEN_DESKTOP, never FULLSCREEN: no mode switch, so it works the same
  // on a system with fullscreen spaces and on one without (before 10.7 SDL
  // sizes the borderless window to the display and raises it over the menu
  // bar, which is the only fullscreen those systems have).
  SDL_SetWindowFullscreen(impl_->window, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
#endif
}

bool Backend::is_fullscreen() const {
  const Uint32 flags = SDL_GetWindowFlags(impl_->window);
#if defined(VIEWER_SDL3)
  return (flags & SDL_WINDOW_FULLSCREEN) != 0;
#else
  return (flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) != 0;
#endif
}

ViewEvent Backend::wait_event(int timeout_ms) {
  SDL_Event e;
#if defined(VIEWER_SDL3)
  const bool got = SDL_WaitEventTimeout(&e, timeout_ms > 0 ? timeout_ms : 0);
#else
  const int got = SDL_WaitEventTimeout(&e, timeout_ms > 0 ? timeout_ms : 0);
#endif
  if (!got) return ViewEvent{};

  ViewEvent out;
  if (g_menu_event != 0 && e.type == g_menu_event) {
    out.type = ViewEvent::Type::Key;
    out.key = map_menu_command(e.user.code);
    return out;
  }
#if defined(VIEWER_SDL3)
  switch (e.type) {
    case SDL_EVENT_QUIT:
      out.type = ViewEvent::Type::Quit;
      break;
    case SDL_EVENT_KEY_DOWN:
      out.type = ViewEvent::Type::Key;
      out.key = map_key(e);
      break;
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
      out.type = ViewEvent::Type::Resize;
      out.w = width();
      out.h = height();
      break;
    default:
      break;
  }
#else
  switch (e.type) {
    case SDL_QUIT:
      out.type = ViewEvent::Type::Quit;
      break;
    case SDL_KEYDOWN:
      out.type = ViewEvent::Type::Key;
      out.key = map_key(e);
      break;
    case SDL_WINDOWEVENT:
      if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
          e.window.event == SDL_WINDOWEVENT_RESIZED) {
        out.type = ViewEvent::Type::Resize;
        out.w = width();
        out.h = height();
      }
      break;
    default:
      break;
  }
#endif
  return out;
}

void Backend::set_texture(const std::uint8_t* rgba, int w, int h) {
  if (w <= 0 || h <= 0) return;
  if (impl_->texture && (impl_->tex_w != w || impl_->tex_h != h)) {
    SDL_DestroyTexture(impl_->texture);
    impl_->texture = nullptr;
  }
  if (!impl_->texture) {
    impl_->texture =
        SDL_CreateTexture(impl_->renderer, SDL_PIXELFORMAT_RGBA32,
                          SDL_TEXTUREACCESS_STATIC, w, h);
    impl_->tex_w = w;
    impl_->tex_h = h;
  }
  if (impl_->texture) {
    SDL_UpdateTexture(impl_->texture, nullptr, rgba, w * 4);
  }
}

void Backend::set_overlay(const std::uint8_t* rgba, int w, int h) {
  if (rgba == nullptr || w <= 0 || h <= 0) {
    if (impl_->overlay) SDL_DestroyTexture(impl_->overlay);
    impl_->overlay = nullptr;
    impl_->ov_w = 0;
    impl_->ov_h = 0;
    return;
  }
  if (impl_->overlay && (impl_->ov_w != w || impl_->ov_h != h)) {
    SDL_DestroyTexture(impl_->overlay);
    impl_->overlay = nullptr;
  }
  if (!impl_->overlay) {
    impl_->overlay =
        SDL_CreateTexture(impl_->renderer, SDL_PIXELFORMAT_RGBA32,
                          SDL_TEXTUREACCESS_STATIC, w, h);
    if (impl_->overlay) {
      SDL_SetTextureBlendMode(impl_->overlay, SDL_BLENDMODE_BLEND);
    }
    impl_->ov_w = w;
    impl_->ov_h = h;
  }
  if (impl_->overlay) {
    SDL_UpdateTexture(impl_->overlay, nullptr, rgba, w * 4);
  }
}

void Backend::render(const Rect& dst) {
  SDL_SetRenderDrawColor(impl_->renderer, 0, 0, 0, 255);
  SDL_RenderClear(impl_->renderer);
  if (impl_->texture) {
#if defined(VIEWER_SDL3)
    const SDL_FRect r{static_cast<float>(dst.x), static_cast<float>(dst.y),
                      static_cast<float>(dst.w), static_cast<float>(dst.h)};
    SDL_RenderTexture(impl_->renderer, impl_->texture, nullptr, &r);
#else
    const SDL_Rect r{dst.x, dst.y, dst.w, dst.h};
    SDL_RenderCopy(impl_->renderer, impl_->texture, nullptr, &r);
#endif
  }
  if (impl_->overlay) {
    constexpr int kMargin = 8;
    const int ox = width() - impl_->ov_w - kMargin;
    const int oy = kMargin;
#if defined(VIEWER_SDL3)
    const SDL_FRect r{static_cast<float>(ox), static_cast<float>(oy),
                      static_cast<float>(impl_->ov_w),
                      static_cast<float>(impl_->ov_h)};
    SDL_RenderTexture(impl_->renderer, impl_->overlay, nullptr, &r);
#else
    const SDL_Rect r{ox, oy, impl_->ov_w, impl_->ov_h};
    SDL_RenderCopy(impl_->renderer, impl_->overlay, nullptr, &r);
#endif
  }
  SDL_RenderPresent(impl_->renderer);
}

std::string Backend::last_error() { return g_last_error; }

}  // namespace shigoku::view
