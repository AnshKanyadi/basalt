#include "call_site.h"

#include <vector>

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

const std::vector<CallSite>& AllCallSites() {
  static const std::vector<CallSite>* const kAll = new std::vector<CallSite>{
      // RIFT-CALL-SITE-LIST-BEGIN
    CallSite::kEnvNewWritableFile,
    CallSite::kEnvNewSequentialFile,
    CallSite::kEnvNewRandomAccessFile,
    CallSite::kEnvNewDirectory,
    CallSite::kEnvGetChildren,
    CallSite::kEnvGetFileSize,
    CallSite::kEnvFileExists,
    CallSite::kEnvDeleteFile,
    CallSite::kEnvRenameFile,
    CallSite::kEnvCreateDir,
    CallSite::kEnvLockFile,
    CallSite::kEnvUnlockFile,
    CallSite::kWritableFileAppend,
    CallSite::kWritableFileFlush,
    CallSite::kWritableFileSync,
    CallSite::kWritableFileClose,
    CallSite::kSequentialFileRead,
    CallSite::kSequentialFileClose,
    CallSite::kRandomAccessFileRead,
    CallSite::kRandomAccessFileClose,
    CallSite::kDirectorySync,
    CallSite::kDirectoryClose,
      // RIFT-CALL-SITE-LIST-END
  };
  return *kAll;
}

}  // namespace rift
