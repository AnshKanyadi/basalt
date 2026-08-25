#include "posix_raw.h"

#include <unistd.h>

#include <cerrno>
#include <string>

namespace rift {
namespace posix {

ssize_t RawWrite(int fd, const void* buf, std::size_t n) {
  return ::write(fd, buf, n);
}

Status WriteFully(int fd, const char* data, std::size_t n, RawWriteFn raw) {
  std::size_t done = 0;
  int zeros = 0;
  while (done < n) {
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

}  // namespace posix
}  // namespace rift
