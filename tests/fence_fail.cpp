// fence_fail.cpp — the exhaustiveness fence, proved by NON-compilation (A2).
//
// This file is NOT part of the normal build. CMake compiles it with
// SHIGOKU_FENCE_CASE defined to 1 or 2 and asserts the compile FAILS; a
// successful compile is the test failure (the fence would be a lie).
//
// Two cases, each isolating one half of A2:
//   Case 1: a switch over a closed enum missing an arm         -> -Werror=switch-enum
//   Case 2: a std::visit over Event missing an alternative     -> overloaded{} has no
//                                                                  matching operator()
//
// Run manually:
//   g++ -std=c++23 -Wall -Wextra -Werror -Werror=switch -Werror=switch-enum \
//       -DSHIGOKU_FENCE_CASE=1 -c tests/fence_fail.cpp   # MUST fail
// CMake wires this as ctest cases `fence_missing_switch_arm` and
// `fence_missing_visit_arm` (WILL_FAIL compile).

#include <string>
#include <variant>

#include "../src/domain.hpp"
#include "../src/event.hpp"
#include "../src/provider.hpp"

using namespace shigoku;

#ifndef SHIGOKU_FENCE_CASE
#  error "fence_fail.cpp must be compiled with -DSHIGOKU_FENCE_CASE=1, =2, or =3"
#endif

#if SHIGOKU_FENCE_CASE == 1
// Missing the Quality::Worst arm. -Werror=switch-enum turns the omission into a
// hard error: "enumeration value 'Worst' not handled in switch".
int fence(Quality q) {
  switch (q) {
    case Quality::Best:  return 0;
    case Quality::P1080: return 1080;
    case Quality::P720:  return 720;
    case Quality::P480:  return 480;
    // case Quality::Worst:  <-- deliberately omitted; this must not compile.
  }
  return -1;
}
#endif

#if SHIGOKU_FENCE_CASE == 2
// A visitor missing the Tick alternative. overloaded{} provides no catch-all
// (A2: by design), so std::visit cannot form a call for Tick and the build
// fails: "no matching function for call to object of type 'overloaded<...>'".
std::string fence(const Event& e) {
  return std::visit(overloaded{
      [](const KeyEvent&) { return std::string("key"); },
      // [](const Tick&) ... <-- omitted on purpose; std::visit must not compile.
  }, e);
}
#endif

#if SHIGOKU_FENCE_CASE == 3
// The K-2 fence, made a compile error (P15 DoD "K-2 fence proven"). A switch
// over WalkOrigin that omits PinFlip is exactly the conflation the K-2 bug was:
// treating a pin flip like a forced-preferred re-route (which continues to a
// full bindings-first walk) instead of stopping and keeping the pin. The
// -Werror=switch-enum fence turns the missing arm into a hard error, so no
// future edit can silently fold PinFlip into another origin's exhaust path.
int fence(WalkOrigin o) {
  switch (o) {
    case WalkOrigin::ForcedPreferred: return 0;  // K-2 continuation.
    case WalkOrigin::Fallback:        return 1;  // ordinary dead-end.
    // case WalkOrigin::PinFlip:  <-- omitted; must not compile (stop-and-keep).
  }
  return -1;
}
#endif
