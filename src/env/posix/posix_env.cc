#include "posix_env.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <string>
#include <utility>
#include <vector>

#include "check.h"

namespace rift {
namespace {

using posix::RawReadDirFn;
using posix::RawWriteFn;
using posix::ReadAllNames;
using posix::WriteFully;

// THE ONE PLACE errno BECOMES A Status. Concentrated here so the mapping is a
// decision rather than twenty of them.
//
// ENOENT maps to kIoError and NOT to kNotFound. kNotFound is the frozen
// interface's ErrNotFound -- a statement about a KEY, produced by engine/model
// too, and therefore the one code in section 7.6's table that carries no
// harness-side predicate. Overloading it with "no such path" would put a
// filesystem outcome into the one code that is defined as never being a place
// the two engines can differ. Callers that need to ask whether a path exists
// call FileExists, which answers with a bool.
Status FromErrno(const char* what, const std::string& path) {
  const int e = errno;
  const std::string ctx = std::string(what) + " " + path + ": errno " + std::to_string(e);
  if (e == ENOSPC || e == EDQUOT) return Status::DiskFull(ctx);
  return Status::IoError(ctx);
}

int CloseIgnoringEintr(int fd) {
  // close(2) must not be retried on EINTR: on Linux the descriptor is released
  // regardless, so a retry can close a descriptor another thread has since been
  // handed. Every other error is reported, because Close is a write call and
  // close(2) is where writeback that failed after the last Sync is reported --
  // the last moment anyone can learn the data is gone.
  return ::close(fd);
}

class PosixWritableFile final : public WritableFile {
 public:
  PosixWritableFile(FaultController* f, HandleId id, int fd, std::string path, RawWriteFn raw)
      : WritableFile(f, id), fd_(fd), path_(std::move(path)), raw_(raw) {}
  ~PosixWritableFile() override {
    if (fd_ >= 0) CloseIgnoringEintr(fd_);
  }

 private:
  // Append buffers and performs NO I/O; Flush is where bytes reach the file.
  //
  // There is deliberately no size-triggered flush inside Append. LevelDB's
  // WritableFile flushes when its internal buffer fills, which makes Append a
  // call that sometimes performs I/O at a moment nobody chose -- and section
  // 3.3's matrix says of Append "buffered; nothing has reached the device",
  // which is the behaviour TestEnv models. An Env whose two implementations
  // disagree about which call reaches the device would make B4's differential
  // comparison compare the wrong thing.
  //
  // The cost, stated: this file's memory use equals the largest span the caller
  // writes between Flushes. The engine bounds that with kWalBufferBytes, which
  // is why the cost is acceptable here and would not be in a general-purpose
  // file abstraction.
  Status DoAppend(Slice data) override {
    buf_.append(data.data(), data.size());
    return Status::Ok();
  }

  Status DoFlush() override {
    if (buf_.empty()) return Status::Ok();
    Status s = WriteFully(fd_, buf_.data(), buf_.size(), raw_);
    if (!s.ok()) return s;
    buf_.clear();
    return Status::Ok();
  }

  Status DoSync() override {
    Status s = DoFlush();
    if (!s.ok()) return s;
#if defined(__APPLE__)
    // fsync(2) on macOS returns before the drive has flushed its own write
    // cache. F_FULLFSYNC is the call that does not. Using fsync here would make
    // every durability claim on this platform a claim about the page cache.
    if (::fcntl(fd_, F_FULLFSYNC, 0) != 0) return FromErrno("fullfsync", path_);
#else
    if (::fsync(fd_) != 0) return FromErrno("fsync", path_);
#endif
    return Status::Ok();
  }

  Status DoClose() override {
    Status s = DoFlush();
    const int fd = fd_;
    fd_ = -1;
    if (CloseIgnoringEintr(fd) != 0) {
      Status close_err = FromErrno("close", path_);
      return s.ok() ? close_err : s;
    }
    return s;
  }

  int fd_;
  std::string path_;
  RawWriteFn raw_;
  std::string buf_;
};

class PosixSequentialFile final : public SequentialFile {
 public:
  PosixSequentialFile(FaultController* f, HandleId id, int fd, std::string path)
      : SequentialFile(f, id), fd_(fd), path_(std::move(path)) {}
  ~PosixSequentialFile() override {
    if (fd_ >= 0) CloseIgnoringEintr(fd_);
  }

 private:
  Status DoRead(std::size_t n, Slice* result, char* scratch) override {
    while (true) {  // RIFT_POSIX_RETRY: read(2) is retried on EINTR
      const ssize_t r = ::read(fd_, scratch, n);
      if (r < 0) {
        if (errno == EINTR) continue;
        return FromErrno("read", path_);
      }
      // A short read at EOF is normal and is not a fault.
      *result = Slice(scratch, static_cast<std::size_t>(r));
      return Status::Ok();
    }
  }

  Status DoClose() override {
    const int fd = fd_;
    fd_ = -1;
    if (CloseIgnoringEintr(fd) != 0) return FromErrno("close", path_);
    return Status::Ok();
  }

  int fd_;
  std::string path_;
};

class PosixRandomAccessFile final : public RandomAccessFile {
 public:
  PosixRandomAccessFile(FaultController* f, HandleId id, int fd, std::string path)
      : RandomAccessFile(f, id), fd_(fd), path_(std::move(path)) {}
  ~PosixRandomAccessFile() override {
    if (fd_ >= 0) CloseIgnoringEintr(fd_);
  }

 private:
  Status DoRead(uint64_t offset, std::size_t n, Slice* result, char* scratch) override {
    while (true) {  // RIFT_POSIX_RETRY: pread(2) is retried on EINTR
      const ssize_t r = ::pread(fd_, scratch, n, static_cast<off_t>(offset));
      if (r < 0) {
        if (errno == EINTR) continue;
        return FromErrno("pread", path_);
      }
      *result = Slice(scratch, static_cast<std::size_t>(r));
      return Status::Ok();
    }
  }

  Status DoClose() override {
    const int fd = fd_;
    fd_ = -1;
    if (CloseIgnoringEintr(fd) != 0) return FromErrno("close", path_);
    return Status::Ok();
  }

  int fd_;
  std::string path_;
};

class PosixDirectory final : public Directory {
 public:
  PosixDirectory(FaultController* f, HandleId id, int fd, std::string path)
      : Directory(f, id), fd_(fd), path_(std::move(path)) {}
  ~PosixDirectory() override {
    if (fd_ >= 0) CloseIgnoringEintr(fd_);
  }

 private:
  Status DoSync() override {
    // Not decoration: a WAL created, written and fsynced is still losable if
    // the directory entry naming it was never made durable. F_FULLFSYNC is not
    // used here -- a directory entry is metadata, and fsync on the directory fd
    // is the portable call for it.
    if (::fsync(fd_) != 0) return FromErrno("fsync dir", path_);
    return Status::Ok();
  }

  Status DoClose() override {
    const int fd = fd_;
    fd_ = -1;
    if (CloseIgnoringEintr(fd) != 0) return FromErrno("close dir", path_);
    return Status::Ok();
  }

  int fd_;
  std::string path_;
};

class PosixFileLock final : public FileLock {
 public:
  PosixFileLock(int fd, std::string path) : fd_(fd), path_(std::move(path)) {}
  ~PosixFileLock() override {
    if (fd_ >= 0) CloseIgnoringEintr(fd_);
  }
  int fd() const { return fd_; }
  const std::string& path() const { return path_; }
  void Release() { fd_ = -1; }

 private:
  int fd_;
  std::string path_;
};

class PosixEnv final : public Env {
 public:
  PosixEnv(FaultController* f, RawWriteFn raw, RawReadDirFn readdir)
      : Env(f, HandleId()), faults_(f), raw_(raw), readdir_(readdir) {}
  ~PosixEnv() override = default;

 private:
  Status DoNewWritableFile(const std::string& path, WritableFilePtr* out) override {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return FromErrno("open", path);
    *out = WritableFilePtr(new PosixWritableFile(faults_, NextHandleId(), fd, path, raw_));
    return Status::Ok();
  }

  Status DoNewSequentialFile(const std::string& path, SequentialFilePtr* out) override {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return FromErrno("open", path);
    *out = SequentialFilePtr(new PosixSequentialFile(faults_, NextHandleId(), fd, path));
    return Status::Ok();
  }

  Status DoNewRandomAccessFile(const std::string& path, RandomAccessFilePtr* out) override {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return FromErrno("open", path);
    *out = RandomAccessFilePtr(new PosixRandomAccessFile(faults_, NextHandleId(), fd, path));
    return Status::Ok();
  }

  Status DoNewDirectory(const std::string& path, DirectoryPtr* out) override {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return FromErrno("opendir", path);
    *out = DirectoryPtr(new PosixDirectory(faults_, NextHandleId(), fd, path));
    return Status::Ok();
  }

  // Returns children in whatever order the filesystem gives, and does NOT sort.
  // Sorting here would hide the bug it exists to expose: directory order is
  // filesystem-dependent, recovery must sort by parsed file number before
  // anything else, and TestEnv returns children reverse-sorted ON PURPOSE so an
  // engine that forgot fails on the first test rather than on someone else's
  // filesystem. This is the C++ analogue of the map-iteration rule.
  Status DoGetChildren(const std::string& dir, std::vector<std::string>* out) override {
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return FromErrno("opendir", dir);
    Status s = ReadAllNames(d, out, readdir_);
    if (::closedir(d) != 0 && s.ok()) return FromErrno("closedir", dir);
    return s;
  }

  Status DoGetFileSize(const std::string& path, uint64_t* out) override {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return FromErrno("stat", path);
    *out = static_cast<uint64_t>(st.st_size);
    return Status::Ok();
  }

  Status DoFileExists(const std::string& path, bool* out) override {
    struct stat st;
    if (::stat(path.c_str(), &st) == 0) {
      *out = true;
      return Status::Ok();
    }
    if (errno == ENOENT) {
      *out = false;
      return Status::Ok();
    }
    return FromErrno("stat", path);
  }

  Status DoDeleteFile(const std::string& path) override {
    if (::unlink(path.c_str()) != 0) return FromErrno("unlink", path);
    return Status::Ok();
  }

  Status DoRenameFile(const std::string& from, const std::string& to) override {
    if (::rename(from.c_str(), to.c_str()) != 0) return FromErrno("rename", from);
    return Status::Ok();
  }

  Status DoCreateDir(const std::string& path) override {
    if (::mkdir(path.c_str(), 0755) != 0) return FromErrno("mkdir", path);
    return Status::Ok();
  }

  Status DoLockFile(const std::string& path, FileLockPtr* out) override {
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) return FromErrno("open lock", path);
    struct flock fl;
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    fl.l_pid = 0;
    if (::fcntl(fd, F_SETLK, &fl) != 0) {
      Status s = FromErrno("lock", path);
      CloseIgnoringEintr(fd);
      return s;
    }
    *out = FileLockPtr(new PosixFileLock(fd, path));
    return Status::Ok();
  }

  Status DoUnlockFile(FileLockPtr lock) override {
    if (lock == nullptr) return Status::InvalidArgument("UnlockFile(nullptr)");
    PosixFileLock* pl = static_cast<PosixFileLock*>(lock.get());
    struct flock fl;
    fl.l_type = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    fl.l_pid = 0;
    const int fd = pl->fd();
    Status s = Status::Ok();
    if (::fcntl(fd, F_SETLK, &fl) != 0) s = FromErrno("unlock", pl->path());
    pl->Release();
    if (CloseIgnoringEintr(fd) != 0 && s.ok()) s = FromErrno("close lock", pl->path());
    return s;
  }

  FaultController* faults_;
  RawWriteFn raw_;
  RawReadDirFn readdir_;
};

}  // namespace

std::unique_ptr<Env> NewPosixEnv(FaultController* faults, posix::RawWriteFn raw_write,
                                 posix::RawReadDirFn raw_readdir) {
  RIFT_CHECK(faults != nullptr);
  return std::unique_ptr<Env>(new PosixEnv(faults, raw_write, raw_readdir));
}

}  // namespace rift
