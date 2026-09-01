// Carrying a durable image out of a process that is about to _exit.
//
// The in-process kill (KillMode::kInProcess) leaves TestEnv's durable image in
// memory where the rig can read it. A real _exit does not, and the whole point
// of the sampled real-_exit lane is that NOTHING survives it -- no destructor,
// no heap, no flush. So the image has to be on a real file before the process
// goes, and this is the only piece of the harness that writes one.
//
// It lives in rig/ and not in engine-cpp/src for a reason that is enforced
// rather than stylistic: src/ is what the A5 scope scan polices, and it bans
// raw open/write/fsync outside env/posix/. A mirror written from TestEnv would
// be a syscall in scanned scope. TestEnv therefore offers the image through a
// hook and the rig owns the file.
#ifndef BASALT_RIG_DURABLE_MIRROR_H_
#define BASALT_RIG_DURABLE_MIRROR_H_

#include <string>

#include "test_env.h"

namespace basalt {
namespace rig {

// Overwrites `path` with `image`. Best-effort by design: a real _exit is a
// PROCESS kill, not a power cut, so the page cache survives it and an fsync
// here would be modelling a failure this lane does not claim to cover.
bool WriteDurableMirror(const std::string& path, const testenv::DurableImage& image);

// Reads back what WriteDurableMirror wrote. Returns false if the file is
// absent, truncated, or does not parse -- all of which mean the child died
// before the image reached disk, which is a result and not an error.
bool ReadDurableMirror(const std::string& path, testenv::DurableImage* out);

// Installable directly as a TestEnvironment::PromotionHook; ctx is a
// std::string* naming the mirror path.
void MirrorHook(void* ctx, const testenv::DurableImage& image);

}  // namespace rig
}  // namespace basalt

#endif  // BASALT_RIG_DURABLE_MIRROR_H_
