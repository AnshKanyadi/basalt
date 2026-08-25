// Env: the choke point every syscall passes through.
//
// PRIORITY ORDER, AND IT DECIDES TIES (DESIGN-B1 section 3.1):
//   1. A fault-injection surface. Every failure the B1 and B4 rigs need must be
//      expressible as the behaviour of an Env call, and every Env call must be
//      a kill point.
//   2. The A5 boundary for syscalls. Every syscall goes through it for the
//      reason every clock read goes through Clock.
//   3. Portability. Third, and barely: Linux and macOS, nothing else.
// Where a portable-looking abstraction and an injectable-looking one differ,
// the injectable one wins.
//
// WHY THE SHAPE IS NON-VIRTUAL INTERFACE (B1-D1, ruled).
//
// The obvious design -- LevelDB-shaped file objects whose every method begins
// with a call to the fault controller -- was approved in substance and REJECTED
// AS A CONVENTION. "Every method's first act is FaultController::Intercept" is
// a rule that lives in review discipline: a method added during B2 that forgets
// the call compiles, tests green, and silently leaves the fault surface, and
// nothing anywhere reports it. Track A moved hold legality out of the generator
// for the same reason -- a generator-side rule is not a rule.
//
// So the public surface is non-virtual and belongs to the base class, the
// implementation surface is private and pure-virtual, and callers never see the
// derived type. A PosixWritableFile or a TestWritableFile overrides only the
// private Do* methods, so IT IS NOT POSSIBLE FOR AN IMPLEMENTATION TO EXPOSE A
// PUBLIC ENTRY POINT THAT SKIPS THE INTERCEPTION. That is the structural half.
//
// THE RESIDUAL, STATED RATHER THAN IMPLIED (section 3.2.1).
//
// NVI makes bypass impossible from an implementation. It does NOT make it
// impossible from an edit to THIS FILE: adding a public virtual to a base class
// here would bypass, which is exactly what mutant BM17 does. What stops it is
// the 1:1:1 assertion in scripts/cpp-scan.sh, and the residual after that is
// that the assertion could be weakened in the same diff that adds the method --
// which the scan lane's blind patches cover from B1.4.
//
// The honest claim is therefore "bypassing requires defeating two independent
// checks in one diff", NOT "bypassing is impossible". The second sentence would
// be false.
//
// WHAT THIS FILE DELIBERATELY DOES NOT HAVE (section 3.4). LevelDB's Env
// carries NowMicros, SleepForMicroseconds, Schedule and NewLogger. Ours carries
// none, and each omission is a ruling rather than a simplification:
//   * No clock. A wall-clock read is unobtainable by construction, so the C++
//     analogue of clock/real.go's one hatched time.Now() is ZERO hatched calls.
//   * No sleep. A timing dependency in a rig whose value is that timing is
//     authored.
//   * No thread pool. Background work scheduled by Env would make kill points
//     unorderable: the sweep identifies a point by a call ordinal, and an
//     ordinal is meaningless if an invisible thread draws from the same
//     counter. Forward binding for B3: compaction's thread is the engine's,
//     declared and joined explicitly, visible to the sweep as its own stream.
//   * No logger. The engine does not open files to talk about itself.
#ifndef RIFT_ENV_ENV_H_
#define RIFT_ENV_ENV_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "call_site.h"
#include "env_handle.h"
#include "fault_controller.h"
#include "slice.h"
#include "status.h"

namespace rift {

class Env;
class WritableFile;
class SequentialFile;
class RandomAccessFile;
class Directory;

// An opaque token. It has no methods, therefore no CallSite and nothing to
// intercept; the two calls that create and destroy it are Env's.
class FileLock {
 public:
  virtual ~FileLock();
};

using WritableFilePtr = std::unique_ptr<WritableFile>;
using SequentialFilePtr = std::unique_ptr<SequentialFile>;
using RandomAccessFilePtr = std::unique_ptr<RandomAccessFile>;
using DirectoryPtr = std::unique_ptr<Directory>;
using FileLockPtr = std::unique_ptr<FileLock>;

// RIFT-ENV-SURFACE-BEGIN
//
// Everything between these markers is parsed by scripts/cpp-scan.sh under a
// STRICT grammar: a line it cannot classify is a lane failure, not a line it
// skips. That is the point. A parser that silently ignores what it does not
// understand reports the health of its own grammar and calls it coverage.

class WritableFile {
 public:
  virtual ~WritableFile();
  Status Append(Slice data);  // RIFT_ENV_CALL kWritableFileAppend
  Status Flush();  // RIFT_ENV_CALL kWritableFileFlush
  Status Sync();  // RIFT_ENV_CALL kWritableFileSync
  Status Close();  // RIFT_ENV_CALL kWritableFileClose
  WritableFile(const WritableFile&) = delete;
  WritableFile& operator=(const WritableFile&) = delete;
 protected:
  WritableFile(FaultController* faults, HandleId id);  // RIFT_ENV_CTOR
 private:
  virtual Status DoAppend(Slice data) = 0;  // RIFT_ENV_IMPL kWritableFileAppend
  virtual Status DoFlush() = 0;  // RIFT_ENV_IMPL kWritableFileFlush
  virtual Status DoSync() = 0;  // RIFT_ENV_IMPL kWritableFileSync
  virtual Status DoClose() = 0;  // RIFT_ENV_IMPL kWritableFileClose
  FaultController* faults_;  // RIFT_ENV_STATE
  HandleId id_;  // RIFT_ENV_STATE
  bool closed_ = false;  // RIFT_ENV_STATE
};

class SequentialFile {
 public:
  virtual ~SequentialFile();
  Status Read(std::size_t n, Slice* result, char* scratch);  // RIFT_ENV_CALL kSequentialFileRead
  Status Close();  // RIFT_ENV_CALL kSequentialFileClose
  SequentialFile(const SequentialFile&) = delete;
  SequentialFile& operator=(const SequentialFile&) = delete;
 protected:
  SequentialFile(FaultController* faults, HandleId id);  // RIFT_ENV_CTOR
 private:
  virtual Status DoRead(std::size_t n, Slice* result, char* scratch) = 0;  // RIFT_ENV_IMPL kSequentialFileRead
  virtual Status DoClose() = 0;  // RIFT_ENV_IMPL kSequentialFileClose
  FaultController* faults_;  // RIFT_ENV_STATE
  HandleId id_;  // RIFT_ENV_STATE
  bool closed_ = false;  // RIFT_ENV_STATE
};

class RandomAccessFile {
 public:
  virtual ~RandomAccessFile();
  Status Read(uint64_t offset, std::size_t n, Slice* result, char* scratch);  // RIFT_ENV_CALL kRandomAccessFileRead
  Status Close();  // RIFT_ENV_CALL kRandomAccessFileClose
  RandomAccessFile(const RandomAccessFile&) = delete;
  RandomAccessFile& operator=(const RandomAccessFile&) = delete;
 protected:
  RandomAccessFile(FaultController* faults, HandleId id);  // RIFT_ENV_CTOR
 private:
  virtual Status DoRead(uint64_t offset, std::size_t n, Slice* result, char* scratch) = 0;  // RIFT_ENV_IMPL kRandomAccessFileRead
  virtual Status DoClose() = 0;  // RIFT_ENV_IMPL kRandomAccessFileClose
  FaultController* faults_;  // RIFT_ENV_STATE
  HandleId id_;  // RIFT_ENV_STATE
  bool closed_ = false;  // RIFT_ENV_STATE
};

class Directory {
 public:
  virtual ~Directory();
  Status Sync();  // RIFT_ENV_CALL kDirectorySync
  Status Close();  // RIFT_ENV_CALL kDirectoryClose
  Directory(const Directory&) = delete;
  Directory& operator=(const Directory&) = delete;
 protected:
  Directory(FaultController* faults, HandleId id);  // RIFT_ENV_CTOR
 private:
  virtual Status DoSync() = 0;  // RIFT_ENV_IMPL kDirectorySync
  virtual Status DoClose() = 0;  // RIFT_ENV_IMPL kDirectoryClose
  FaultController* faults_;  // RIFT_ENV_STATE
  HandleId id_;  // RIFT_ENV_STATE
  bool closed_ = false;  // RIFT_ENV_STATE
};

class Env {
 public:
  virtual ~Env();
  Status NewWritableFile(const std::string& path, WritableFilePtr* out);  // RIFT_ENV_CALL kEnvNewWritableFile
  Status NewSequentialFile(const std::string& path, SequentialFilePtr* out);  // RIFT_ENV_CALL kEnvNewSequentialFile
  Status NewRandomAccessFile(const std::string& path, RandomAccessFilePtr* out);  // RIFT_ENV_CALL kEnvNewRandomAccessFile
  Status NewDirectory(const std::string& path, DirectoryPtr* out);  // RIFT_ENV_CALL kEnvNewDirectory
  Status GetChildren(const std::string& dir, std::vector<std::string>* out);  // RIFT_ENV_CALL kEnvGetChildren
  Status GetFileSize(const std::string& path, uint64_t* out);  // RIFT_ENV_CALL kEnvGetFileSize
  Status FileExists(const std::string& path, bool* out);  // RIFT_ENV_CALL kEnvFileExists
  Status DeleteFile(const std::string& path);  // RIFT_ENV_CALL kEnvDeleteFile
  Status RenameFile(const std::string& from, const std::string& to);  // RIFT_ENV_CALL kEnvRenameFile
  Status CreateDir(const std::string& path);  // RIFT_ENV_CALL kEnvCreateDir
  Status LockFile(const std::string& path, FileLockPtr* out);  // RIFT_ENV_CALL kEnvLockFile
  Status UnlockFile(FileLockPtr lock);  // RIFT_ENV_CALL kEnvUnlockFile
  Env(const Env&) = delete;
  Env& operator=(const Env&) = delete;
 protected:
  Env(FaultController* faults, HandleId id);  // RIFT_ENV_CTOR
  HandleId NextHandleId();  // RIFT_ENV_HELPER
 private:
  virtual Status DoNewWritableFile(const std::string& path, WritableFilePtr* out) = 0;  // RIFT_ENV_IMPL kEnvNewWritableFile
  virtual Status DoNewSequentialFile(const std::string& path, SequentialFilePtr* out) = 0;  // RIFT_ENV_IMPL kEnvNewSequentialFile
  virtual Status DoNewRandomAccessFile(const std::string& path, RandomAccessFilePtr* out) = 0;  // RIFT_ENV_IMPL kEnvNewRandomAccessFile
  virtual Status DoNewDirectory(const std::string& path, DirectoryPtr* out) = 0;  // RIFT_ENV_IMPL kEnvNewDirectory
  virtual Status DoGetChildren(const std::string& dir, std::vector<std::string>* out) = 0;  // RIFT_ENV_IMPL kEnvGetChildren
  virtual Status DoGetFileSize(const std::string& path, uint64_t* out) = 0;  // RIFT_ENV_IMPL kEnvGetFileSize
  virtual Status DoFileExists(const std::string& path, bool* out) = 0;  // RIFT_ENV_IMPL kEnvFileExists
  virtual Status DoDeleteFile(const std::string& path) = 0;  // RIFT_ENV_IMPL kEnvDeleteFile
  virtual Status DoRenameFile(const std::string& from, const std::string& to) = 0;  // RIFT_ENV_IMPL kEnvRenameFile
  virtual Status DoCreateDir(const std::string& path) = 0;  // RIFT_ENV_IMPL kEnvCreateDir
  virtual Status DoLockFile(const std::string& path, FileLockPtr* out) = 0;  // RIFT_ENV_IMPL kEnvLockFile
  virtual Status DoUnlockFile(FileLockPtr lock) = 0;  // RIFT_ENV_IMPL kEnvUnlockFile
  FaultController* faults_;  // RIFT_ENV_STATE
  HandleId id_;  // RIFT_ENV_STATE
  uint64_t next_handle_ = 0;  // RIFT_ENV_STATE
};

// RIFT-ENV-SURFACE-END

}  // namespace rift

#endif  // RIFT_ENV_ENV_H_
