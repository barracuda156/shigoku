// mac_menu.hpp — the macOS menu-bar seam for shigoku-view.
//
// SDL builds its own menu bar for an unbundled app, and the "Toggle Full
// Screen" item it puts in the Window menu is wired to NSWindow's
// -toggleFullScreen:, which only exists from 10.7 on. Before that AppKit finds
// nothing in the responder chain that handles the action and greys the item
// out, so the one obvious way to go fullscreen is dead on exactly the systems
// this port targets. install_menu() takes that item over (and adds the zoom
// items SDL never had), routing both to the viewer's own key table.
//
// The implementation (mac_menu.mm) is pure AppKit — it never includes SDL. The
// handler it calls is the SDL layer's, which turns a command into an event the
// normal loop picks up, so a menu click and a keypress travel the same path.
// Everywhere but macOS this is an inline no-op and no .mm is compiled.

#pragma once

namespace shigoku::view {

// What a menu item asks for. The values are carried through an SDL user event,
// hence plain ints on the wire.
enum class MenuCommand {
  Fullscreen = 1,
  ZoomIn,
  ZoomOut,
  ZoomReset,
  Close,  // Cmd+W — the one shortcut SDL's unbundled default menu has no item
          // for at all, so without this the key does nothing.
};

// Called on the main thread from the AppKit menu dispatch, with a MenuCommand
// as an int.
using MenuHandler = void (*)(int);

#if defined(__APPLE__)
// Repair/extend the menu bar. Safe to call once, after SDL has created its
// window (SDL builds its menus during video init). Silently does nothing if
// there is no NSApplication yet.
void install_menu(MenuHandler cb);
#else
inline void install_menu(MenuHandler) {}
#endif

}  // namespace shigoku::view
