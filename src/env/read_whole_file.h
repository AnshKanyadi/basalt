// Read a whole file through Env, in one place.
//
// It was file-local in wal/recovery.cc while there was exactly one caller. The
// manifest is the second, and a second caller is the moment a private helper
// becomes two helpers that agree about EINTR and short reads until one of them
// is edited. Section 7.5's one mechanism, two users.
#ifndef BASALT_ENV_READ_WHOLE_FILE_H_
#define BASALT_ENV_READ_WHOLE_FILE_H_

#include <string>

#include "basalt/env.h"
#include "basalt/status.h"

namespace basalt {

// Reads `path` to EOF. Closes the file on every path, including the failing
// ones, because a leaked descriptor under a fault schedule is a second failure
// with no relationship to the first.
Status ReadWholeFile(Env* env, const std::string& path, std::string* out);

}  // namespace basalt

#endif  // BASALT_ENV_READ_WHOLE_FILE_H_
