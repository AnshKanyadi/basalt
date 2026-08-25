// The two assertions section 8.3 attaches to the Env choke point.
//
// B1-D9 says the WAL buffer is the ENGINE'S OWN MEMORY and that Apply appends
// to it and makes zero Env calls. That is one sentence and TWO assertions, and
// the difference matters:
//
//   1. THE ENV-CALL COUNTER DOES NOT MOVE ACROSS Apply. LevelDB's
//      WritableFile::Append flushes to the OS when its internal buffer fills,
//      so a write can perform I/O at an unpredictable moment -- and
//      "unpredictable moment" is not a way to satisfy "never blocks on I/O".
//      Counting is how that becomes checkable instead of asserted. BM9 blinds
//      it.
//
//   2. THE DB MUTEX IS NEVER HELD ACROSS AN ENV CALL. This is section 0.1
//      principle 1 in force: a ruling that constrains a design is responsible
//      for the failure modes it opens. The memtable lock (B1-D6c) is correct
//      and it opens exactly one new failure -- a Sync holding the mutex across
//      an fsync blocks every reader for the fsync's duration -- which would
//      have been invisible until B5's benchmarks looked inexplicably bad. This
//      guard closes it and BM16 proves the guard fires.
//
// Both are checked IN THE NON-VIRTUAL INTERCEPTION LAYER, which is the same
// choke point the fault controller uses, so they cannot be bypassed for the
// same reason it cannot.
//
// ALWAYS ON, not debug-only. The cost is two thread-local reads per Env call,
// which is unmeasurable against a syscall, and a guard compiled out of the
// configuration people actually ship is a guard that stops being true without
// anyone noticing.
#ifndef RIFT_ENV_ENV_GUARD_H_
#define RIFT_ENV_ENV_GUARD_H_

#include <cstdint>

namespace rift {

// Env calls made on THIS thread. Per-thread because the syncer and the node
// loop are different threads and a global counter would let one hide the
// other's I/O.
uint64_t EnvCallsOnThisThread();
void NoteEnvCall();

// RAII marker for "the DB mutex is held on this thread". Nesting is counted, so
// a re-entrant path does not clear the flag on the way out of its inner scope.
class MutexHeldMarker {
 public:
  MutexHeldMarker();
  ~MutexHeldMarker();
  MutexHeldMarker(const MutexHeldMarker&) = delete;
  MutexHeldMarker& operator=(const MutexHeldMarker&) = delete;
};
int MutexDepthOnThisThread();

// What happens when a guard assertion fails. Production aborts.
//
// The seam exists for the same reason RawWriteFn does: the failure path has to
// be exercised, and a path whose only outcome is abort() can be tested only
// with a death test -- which forks, behaves differently under three of our four
// sanitizer lanes, and would make the induced failure for BM16 the least
// reliable test in the suite. Nothing in the engine can install a handler; only
// a test can.
using GuardViolationFn = void (*)(const char* what);
void SetGuardViolationHandler(GuardViolationFn fn);
void ReportGuardViolation(const char* what);

}  // namespace rift

#endif  // RIFT_ENV_ENV_GUARD_H_
