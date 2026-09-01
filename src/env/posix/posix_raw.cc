#include "basalt/posix_raw.h"

#include <dirent.h>
#include <unistd.h>

#include <cerrno>
#include <string>
#include <vector>

namespace basalt {
namespace posix {

ssize_t RawWrite(int fd, const void* buf, std::size_t n) {
  return ::write(fd, buf, n);
}

Status WriteFully(int fd, const char* data, std::size_t n, RawWriteFn raw) {
  std::size_t done = 0;
  int zeros = 0;
  while (done < n) {  // BASALT_POSIX_RETRY: resumes on a short count, retries on EINTR
    const ssize_t w = raw(fd, data + done, n - done);
    if (w < 0) {
      if (errno == EINTR) continue;  // not a failure; the call was interrupted
      return Status::IoError("write: errno " + std::to_string(errno));
    }
    if (w == 0) {
      if (++zeros >= kMaxConsecutiveZeroWrites) {
        return Status::IoError("write returned 0 " +
                               std::to_string(zeros) + " times without an error");
      }
      continue;
    }
    zeros = 0;
    done += static_cast<std::size_t>(w);
  }
  return Status::Ok();
}

const char* RawReadDir(void* dir) {
  errno = 0;
  struct dirent* e = ::readdir(static_cast<DIR*>(dir));
  return e == nullptr ? nullptr : e->d_name;
}

Status ReadAllNames(void* dir, std::vector<std::string>* out, RawReadDirFn raw) {
  out->clear();
  while (true) {  // BASALT_POSIX_RETRY: one iteration per directory entry
    const char* name = raw(dir);
    if (name == nullptr) {
      if (errno == 0) return Status::Ok();  // end of directory
      out->clear();
      return Status::IoError("readdir: errno " + std::to_string(errno));
    }
    const std::string s(name);
    if (s == "." || s == "..") continue;
    out->push_back(s);
  }
}

}  // namespace posix
}  // namespace basalt
