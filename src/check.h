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
  // A RUN THAT ABORTS SAYS SO, IN ITS OWN OUTPUT, WHERE THE COUNTS ARE READ.
  //
  // A test binary that aborts reports FEWER FAILURES THAN EXIST, and fewer
  // failures reported is indistinguishable from fewer failures existing. That
  // nearly produced the opposite ruling on B3's aliasing condition: the first
  // measurement showed no DropCheck test failing under either reader mutant --
  // which would have meant the shared parser was unacceptable -- and the zero
  // was an artifact of an earlier RIFT_CHECK killing the process before those
  // tests ran.
  //
  // The same shape as HARNESS-013's stalled log: a signal read without its
  // provenance. The cheap mechanical answer is to put the provenance IN the
  // signal, so a count grepped out of this output carries the fact that it is
  // partial.
  std::fprintf(stderr,
               "*** RIFT PARTIAL RUN: aborted here, so any count above this "
               "line is a LOWER BOUND and any absence is unproven ***\n");
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

// THE SAME CHECK, WITH THE CONSEQUENCE INSTEAD OF THE CONDITION.
//
// `RIFT_CHECK` prints the expression, which tells a reader WHAT failed and
// nothing about what it means. For an invariant whose violation is a WRONG
// ANSWER somewhere else -- a read that silently misses a tombstone, a
// compaction that under-selects -- the expression is the least useful half of
// the message. Whoever hits it is holding a bug, not a style violation.
//
// Use it where the failure's meaning is not obvious from the condition. Where
// it is, `RIFT_CHECK` is shorter and says the same thing.
#define RIFT_CHECK_MSG(cond, what)                              \
  do {                                                          \
    if (!(cond)) ::rift::Die(__FILE__, __LINE__, "CHECK failed: " #cond "\n  " what); \
  } while (0)

#endif  // RIFT_CHECK_H_
