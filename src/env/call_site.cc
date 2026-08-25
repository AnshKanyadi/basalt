#include "call_site.h"

#include "check.h"

namespace rift {

const char* CallSiteName(CallSite site) {
  // NO `default:` ARM. An enumerator added here without a name is a build
  // failure, which is the cheapest possible moment to notice it.
  switch (site) {
    case CallSite::kEnvNewWritableFile:        return "kEnvNewWritableFile";
    case CallSite::kEnvNewSequentialFile:      return "kEnvNewSequentialFile";
    case CallSite::kEnvNewRandomAccessFile:    return "kEnvNewRandomAccessFile";
    case CallSite::kEnvNewDirectory:           return "kEnvNewDirectory";
    case CallSite::kEnvGetChildren:            return "kEnvGetChildren";
    case CallSite::kEnvGetFileSize:            return "kEnvGetFileSize";
    case CallSite::kEnvFileExists:             return "kEnvFileExists";
    case CallSite::kEnvDeleteFile:             return "kEnvDeleteFile";
    case CallSite::kEnvRenameFile:             return "kEnvRenameFile";
    case CallSite::kEnvCreateDir:              return "kEnvCreateDir";
    case CallSite::kEnvLockFile:               return "kEnvLockFile";
    case CallSite::kEnvUnlockFile:             return "kEnvUnlockFile";
    case CallSite::kWritableFileAppend:        return "kWritableFileAppend";
    case CallSite::kWritableFileFlush:         return "kWritableFileFlush";
    case CallSite::kWritableFileSync:          return "kWritableFileSync";
    case CallSite::kWritableFileClose:         return "kWritableFileClose";
    case CallSite::kSequentialFileRead:        return "kSequentialFileRead";
    case CallSite::kSequentialFileClose:       return "kSequentialFileClose";
    case CallSite::kRandomAccessFileRead:      return "kRandomAccessFileRead";
    case CallSite::kRandomAccessFileClose:     return "kRandomAccessFileClose";
    case CallSite::kDirectorySync:             return "kDirectorySync";
    case CallSite::kDirectoryClose:            return "kDirectoryClose";
  }
  RIFT_UNREACHABLE("CallSite holds a value no enumerator names");
}

}  // namespace rift
