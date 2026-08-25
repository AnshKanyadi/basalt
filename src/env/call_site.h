// CallSite: one enumerator per public Env entry point. Exactly one.
//
// This enum is one of the three artifacts the 1:1:1 assertion binds together
// (DESIGN-B1 section 3.2): one public non-virtual wrapper, one private Do*
// pure virtual, one CallSite enumerator. Any of the three drifting is a lane
// failure with all three printed -- see scripts/cpp-scan.sh.
//
// What the enumerator IS, concretely: the identity of a fault injection point
// and of a kill point. "Every failure the B1 and B4 rigs need must be
// expressible as the behaviour of an Env call, and every Env call must be a
// kill point" (section 3.1). A CallSite that exists and is never reached is an
// injector nobody can fire, which is why section 3.2's census asserts every
// enumerator is observed at least once -- that half lands at B1.3.
#ifndef RIFT_ENV_CALL_SITE_H_
#define RIFT_ENV_CALL_SITE_H_

#include <cstdint>

namespace rift {

// CLOSED. -Werror=switch, no `default:` arm over it, anywhere.
enum class CallSite : uint8_t {
  // ---- Env
  kEnvNewWritableFile,
  kEnvNewSequentialFile,
  kEnvNewRandomAccessFile,
  kEnvNewDirectory,
  kEnvGetChildren,
  kEnvGetFileSize,
  kEnvFileExists,
  kEnvDeleteFile,
  kEnvRenameFile,
  kEnvCreateDir,
  kEnvLockFile,
  kEnvUnlockFile,
  // ---- WritableFile
  kWritableFileAppend,
  kWritableFileFlush,
  kWritableFileSync,
  kWritableFileClose,
  // ---- SequentialFile
  kSequentialFileRead,
  kSequentialFileClose,
  // ---- RandomAccessFile
  kRandomAccessFileRead,
  kRandomAccessFileClose,
  // ---- Directory
  kDirectorySync,
  kDirectoryClose,
};

// Total by construction: the switch inside has no `default:` arm, so an
// enumerator added without a name is a build failure. A kill point reported as
// "kill 47 failed" is not a bug report; "kill 47 = Sync(000001.log)" is
// (section 9.5), and that starts here.
const char* CallSiteName(CallSite site);

}  // namespace rift

#endif  // RIFT_ENV_CALL_SITE_H_
