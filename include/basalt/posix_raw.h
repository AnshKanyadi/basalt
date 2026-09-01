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
#ifndef BASALT_POSIX_RAW_H_
#define BASALT_POSIX_RAW_H_

#include <sys/types.h>

#include <cstddef>
#include <string>
#include <vector>

#include "basalt/status.h"

namespace basalt {
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

// ------------------------------------------------------- the readdir seam
//
// The second loop in PosixEnv, and it got a seam for the same reason the first
// one did: it is real logic in the component no B1 lane verifies, and
// readdir(3)'s error protocol is a classic. readdir returns NULL both at
// end-of-directory and on error, and the ONLY way to tell them apart is to
// clear errno before the call and read it after. Get that wrong and a
// directory listing truncated by an IO error is indistinguishable from a
// complete one -- which, at section 7.2's gapless-file-number check, becomes a
// missing WAL and a refused open with no explanation.
//
// The handle is a void* so <dirent.h> stays out of this header; posix_raw.cc
// casts it back. That is contained ugliness in the one file allowed to talk to
// the kernel, not a general pattern.

// Clears errno, then returns the next entry's name, or nullptr. nullptr with
// errno == 0 is end-of-directory; nullptr with errno != 0 is an error.
using RawReadDirFn = const char* (*)(void* dir);
const char* RawReadDir(void* dir);

// Every name in an open directory except "." and "..", in whatever order the
// filesystem gives -- deliberately unsorted, because recovery must sort by
// parsed file number and hiding that here would hide the bug.
//
// On error `out` is left EMPTY rather than partially filled: a truncated
// listing that looks complete is the failure mode this whole seam exists for.
Status ReadAllNames(void* dir, std::vector<std::string>* out, RawReadDirFn raw);

}  // namespace posix
}  // namespace basalt

#endif  // BASALT_POSIX_RAW_H_
