// Assertions and the unreachable marker.
//
// The engine is built -fno-exceptions (DESIGN-B1 section 9.1), so there is no
// throw to reach for and none is wanted: no exception may cross into Go, ever,
// and the flag makes that structural rather than a review habit.
#ifndef RIFT_CHECK_H_
#define RIFT_CHECK_H_

#include <cstdio>
#include <cstdlib>

namespace rift {

[[noreturn]] inline void Die(const char* file, int line, const char* what) {
  std::fprintf(stderr, "rift: %s:%d: %s\n", file, line, what);
  std::abort();
}

}  // namespace rift

// RIFT_UNREACHABLE marks the end of an exhaustive switch over a closed enum.
//
// It is NOT a `default:` arm and must never be written as one. A `default:` arm
// silences -Werror=switch, which is the mechanism that turns "somebody added an
// enumerator and forgot to classify it" from a runtime surprise into a build
// failure (DESIGN-B1 sections 7.5 and 7.6). This marker sits AFTER the switch,
// so the exhaustiveness check still fires, and it only executes if the enum
// holds a value no enumerator names -- which means someone cast an integer in.
#define RIFT_UNREACHABLE(what) ::rift::Die(__FILE__, __LINE__, what)

#define RIFT_CHECK(cond)                                        \
  do {                                                          \
    if (!(cond)) ::rift::Die(__FILE__, __LINE__, "CHECK failed: " #cond); \
  } while (0)

#endif  // RIFT_CHECK_H_
