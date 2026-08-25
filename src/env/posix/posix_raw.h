// The raw-write seam: one level below Env, and the only fault surface PosixEnv
// has.
//
// WHY IT EXISTS. DESIGN-B1 section 3.3 footnote (1): short writes are not
// expressible at the Env seam at all. WritableFile::Append is all-or-nothing by
// contract, so TestEnv -- which never calls write(2) -- has nothing to shorten.
// But the loop that must handle a short write(2) IS real code, it is in
// PosixEnv, and it is exactly the kind of loop that is written once and never
// exercised because a local filesystem almost never returns short.
//
// So it gets its own seam one level down: the write function is injectable, and
// a unit test drives it with 1..n-1 bytes per call, with EINTR, and with a
// zero-byte return that must not spin.
//
// THE COST, STATED (section 11 idealization 2). Short writes are covered by a
// UNIT test and are absent from the kill-point sweep, so a short write can
// never combine with a kill point, a quota exhaustion, or a torn sync in one
// run. The alternative -- making Append short-returning and pushing the loop
// into the engine -- would put the fault inside the sweep, and is rejected
// because it duplicates the loop at every call site and moves a syscall detail
// across the exact abstraction line Env exists to draw.
#ifndef RIFT_ENV_POSIX_POSIX_RAW_H_
#define RIFT_ENV_POSIX_POSIX_RAW_H_

#include <sys/types.h>

#include <cstddef>

#include "status.h"

namespace rift {
namespace posix {

// Same contract as write(2): returns bytes written, or -1 with errno set.
using RawWriteFn = ssize_t (*)(int fd, const void* buf, std::size_t n);

// The real one.
ssize_t RawWrite(int fd, const void* buf, std::size_t n);

// A write(2) that returned 0 without an error wrote nothing and reported no
// reason. Retrying is correct for a moment and a livelock forever, so the loop
// gives up after this many consecutive zero returns. The failure mode this
// prevents is the worst kind for a fault rig: a process that is not wrong, not
// finished, and not making progress, in a sweep whose only symptom would be a
// run that never ends.
constexpr int kMaxConsecutiveZeroWrites = 16;

// Writes all n bytes or returns an error. EINTR is retried; a short count is
// resumed from the offset actually written.
Status WriteFully(int fd, const char* data, std::size_t n, RawWriteFn raw);

}  // namespace posix
}  // namespace rift

#endif  // RIFT_ENV_POSIX_POSIX_RAW_H_
