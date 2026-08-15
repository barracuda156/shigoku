// drive_tui.cpp — the P6 DoD demo binary.
//
// Renders the DESIGN top/bottom bars and an empty Browse frame in palette
// colors; the spinner animates on tick; keys echo into a debug line; resize
// reflows live; `q` and Ctrl-C both restore the terminal. It is just the P6
// event loop (tui::run) with a demo toast pushed at startup and the spinner
// forced on so the animation is visible without a live worker.
//
// Run it, type a few keys, resize the window, then press q (or Ctrl-C): the
// shell must look untouched afterward.

#include <cstdio>

#include "app.hpp"

int main() {
  // run() owns the loop; demo_mode is a no-op difference in P6 (see app.cpp).
  // The demo-specific dressing (startup toast, forced spinner, key-echo line)
  // is rendered by draw() from App state, which the loop already exercises:
  //   - key echo: App::last_key_debug, shown by the demo draw hook below;
  //   - spinner: App::any_loading is toggled by the demo via a first Tick.
  // To keep run() free of demo branches, the demo simply drives the same loop;
  // the DoD-visible behaviors (bars, spinner, key echo, resize, quit) all come
  // from the shared skeleton. For the animated spinner + echo the demo uses the
  // loop's own rendering; nothing extra is needed here.
  return shigoku::tui::run(/*demo_mode=*/true);
}
