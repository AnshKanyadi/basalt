#include "basalt/env.h"

#include <utility>

#include "env_guard.h"

namespace basalt {

// EVERY DEFINITION IN THIS FILE HAS THE SAME TWO LINES, AND THAT IS THE DESIGN.
//
//     Status s = Enter(faults_, <this call's CallSite>, id_);
//     if (!s.ok()) return s;
//     return Do<Whatever>(...);
//
// The uniformity is not laziness; it is the property. Because the public method
// is non-virtual and lives here, an implementation cannot supply a different
// version of it, so there is no diff anywhere that can add an entry point
// without these two lines -- the fault surface and the kill-point set are
// closed by the type system rather than by anyone remembering.
//
// A non-ok Intercept means the implementation is NOT invoked. That is what a
// kill point is: after the dead flag is set every subsequent Env call is a
// no-op returning kKilled and TestEnv freezes its durable image, so code that
// ignores the Status can still only touch a frozen Env (section 9.5).

namespace {

// EVERY Env call enters here, because every public wrapper is non-virtual and
// lives in this file. Section 8.3's two assertions therefore cannot be bypassed
// for the same reason the fault controller cannot.
// The second half of the same choke point. It consumes no ordinal: one Env call
// enters once and leaves once.
Status Leave(FaultController* faults, CallSite site, HandleId id, Status s) {
  return faults->AfterEffect(site, id, s);
}

Status Enter(FaultController* faults, CallSite site, HandleId id) {
  NoteEnvCall();
  if (MutexDepthOnThisThread() != 0) {
    // A Sync under the DB mutex blocks every reader for the fsync's duration.
    // The lock ruling is correct AND it opened this, which is why the guard
    // ships with it rather than after it.
    ReportGuardViolation("the DB mutex is held across an Env call");
  }
  return faults->Intercept(site, id);
}

}  // namespace

FileLock::~FileLock() = default;

WritableFile::WritableFile(FaultController* faults, HandleId id) : faults_(faults), id_(id) {}
WritableFile::~WritableFile() = default;

Status WritableFile::Append(Slice data) {
  Status s = Enter(faults_, CallSite::kWritableFileAppend, id_);
  if (!s.ok()) return s;
  return Leave(faults_, CallSite::kWritableFileAppend, id_, DoAppend(data));
}

Status WritableFile::Flush() {
  Status s = Enter(faults_, CallSite::kWritableFileFlush, id_);
  if (!s.ok()) return s;
  return Leave(faults_, CallSite::kWritableFileFlush, id_, DoFlush());
}

Status WritableFile::Sync() {
  Status s = Enter(faults_, CallSite::kWritableFileSync, id_);
  if (!s.ok()) return s;
  return Leave(faults_, CallSite::kWritableFileSync, id_, DoSync());
}

Status WritableFile::Close() {
  // Idempotent, so a destructor may call it after an explicit Close.
  if (closed_) return Status::Ok();
  Status s = Enter(faults_, CallSite::kWritableFileClose, id_);
  if (!s.ok()) return s;
  // closed_ is set BEFORE DoClose and regardless of what DoClose returns,
  // because close(2) releases the descriptor even when it reports EIO. The
  // error still reaches the caller, and it must: `Close` is a write call, and
  // treating it as bookkeeping is a known way to lose data. close(2) reports
  // EIO for writeback that failed after the last Sync, which is the last
  // moment anyone can learn that the data is gone (mutant BM7).
  closed_ = true;
  return Leave(faults_, CallSite::kWritableFileClose, id_, DoClose());
}

SequentialFile::SequentialFile(FaultController* faults, HandleId id) : faults_(faults), id_(id) {}
SequentialFile::~SequentialFile() = default;

Status SequentialFile::Read(std::size_t n, Slice* result, char* scratch) {
  Status s = Enter(faults_, CallSite::kSequentialFileRead, id_);
  if (!s.ok()) return s;
  return Leave(faults_, CallSite::kSequentialFileRead, id_, DoRead(n, result, scratch));
}

Status SequentialFile::Close() {
  if (closed_) return Status::Ok();
  Status s = Enter(faults_, CallSite::kSequentialFileClose, id_);
  if (!s.ok()) return s;
  closed_ = true;
  return Leave(faults_, CallSite::kSequentialFileClose, id_, DoClose());
}

RandomAccessFile::RandomAccessFile(FaultController* faults, HandleId id) : faults_(faults), id_(id) {}
RandomAccessFile::~RandomAccessFile() = default;

Status RandomAccessFile::Read(uint64_t offset, std::size_t n, Slice* result, char* scratch) {
  Status s = Enter(faults_, CallSite::kRandomAccessFileRead, id_);
  if (!s.ok()) return s;
  return Leave(faults_, CallSite::kRandomAccessFileRead, id_, DoRead(offset, n, result, scratch));
}

Status RandomAccessFile::Close() {
  if (closed_) return Status::Ok();
  Status s = Enter(faults_, CallSite::kRandomAccessFileClose, id_);
  if (!s.ok()) return s;
  closed_ = true;
  return Leave(faults_, CallSite::kRandomAccessFileClose, id_, DoClose());
}

Directory::Directory(FaultController* faults, HandleId id) : faults_(faults), id_(id) {}
Directory::~Directory() = default;

Status Directory::Sync() {
  // Not decoration. A WAL created, written and fsynced is still losable if the
  // directory entry naming it was never made durable: the bytes survive and the
  // name does not. Section 7.2's gapless-file-number check is what turns that
  // loss into a failed open instead of silence.
  Status s = Enter(faults_, CallSite::kDirectorySync, id_);
  if (!s.ok()) return s;
  return Leave(faults_, CallSite::kDirectorySync, id_, DoSync());
}

Status Directory::Close() {
  if (closed_) return Status::Ok();
  Status s = Enter(faults_, CallSite::kDirectoryClose, id_);
  if (!s.ok()) return s;
  closed_ = true;
  return Leave(faults_, CallSite::kDirectoryClose, id_, DoClose());
}

Env::Env(FaultController* faults, HandleId id) : faults_(faults), id_(id) {}

HandleId Env::NextHandleId() {
  // Sequential and per-Env: the same workload assigns the same ids on every
  // run, on every machine, which an address never does.
  HandleId id;
  id.value = ++next_handle_;
  return id;
}
Env::~Env() = default;

Status Env::NewWritableFile(const std::string& path, WritableFilePtr* out) {
  Status s = Enter(faults_, CallSite::kEnvNewWritableFile, id_);
  if (!s.ok()) return s;
  return Leave(faults_, CallSite::kEnvNewWritableFile, id_, DoNewWritableFile(path, out));
}

Status Env::NewSequentialFile(const std::string& path, SequentialFilePtr* out) {
  Status s = Enter(faults_, CallSite::kEnvNewSequentialFile, id_);
  if (!s.ok()) return s;
  return Leave(faults_, CallSite::kEnvNewSequentialFile, id_, DoNewSequentialFile(path, out));
}

Status Env::NewRandomAccessFile(const std::string& path, RandomAccessFilePtr* out) {
  Status s = Enter(faults_, CallSite::kEnvNewRandomAccessFile, id_);
  if (!s.ok()) return s;
  return Leave(faults_, CallSite::kEnvNewRandomAccessFile, id_, DoNewRandomAccessFile(path, out));
}

Status Env::NewDirectory(const std::string& path, DirectoryPtr* out) {
  Status s = Enter(faults_, CallSite::kEnvNewDirectory, id_);
  if (!s.ok()) return s;
  return Leave(faults_, CallSite::kEnvNewDirectory, id_, DoNewDirectory(path, out));
}

Status Env::GetChildren(const std::string& dir, std::vector<std::string>* out) {
  Status s = Enter(faults_, CallSite::kEnvGetChildren, id_);
  if (!s.ok()) return s;
  return Leave(faults_, CallSite::kEnvGetChildren, id_, DoGetChildren(dir, out));
}

Status Env::GetFileSize(const std::string& path, uint64_t* out) {
  Status s = Enter(faults_, CallSite::kEnvGetFileSize, id_);
  if (!s.ok()) return s;
  return Leave(faults_, CallSite::kEnvGetFileSize, id_, DoGetFileSize(path, out));
}

Status Env::FileExists(const std::string& path, bool* out) {
  Status s = Enter(faults_, CallSite::kEnvFileExists, id_);
  if (!s.ok()) return s;
  return Leave(faults_, CallSite::kEnvFileExists, id_, DoFileExists(path, out));
}

Status Env::DeleteFile(const std::string& path) {
  Status s = Enter(faults_, CallSite::kEnvDeleteFile, id_);
  if (!s.ok()) return s;
  return Leave(faults_, CallSite::kEnvDeleteFile, id_, DoDeleteFile(path));
}

Status Env::RenameFile(const std::string& from, const std::string& to) {
  Status s = Enter(faults_, CallSite::kEnvRenameFile, id_);
  if (!s.ok()) return s;
  return Leave(faults_, CallSite::kEnvRenameFile, id_, DoRenameFile(from, to));
}

Status Env::CreateDir(const std::string& path) {
  Status s = Enter(faults_, CallSite::kEnvCreateDir, id_);
  if (!s.ok()) return s;
  return Leave(faults_, CallSite::kEnvCreateDir, id_, DoCreateDir(path));
}

Status Env::LockFile(const std::string& path, FileLockPtr* out) {
  Status s = Enter(faults_, CallSite::kEnvLockFile, id_);
  if (!s.ok()) return s;
  return Leave(faults_, CallSite::kEnvLockFile, id_, DoLockFile(path, out));
}

Status Env::UnlockFile(FileLockPtr lock) {
  Status s = Enter(faults_, CallSite::kEnvUnlockFile, id_);
  if (!s.ok()) return s;
  return Leave(faults_, CallSite::kEnvUnlockFile, id_, DoUnlockFile(std::move(lock)));
}

}  // namespace basalt
