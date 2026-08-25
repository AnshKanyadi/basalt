#include "env.h"

#include <utility>

namespace rift {

// EVERY DEFINITION IN THIS FILE HAS THE SAME TWO LINES, AND THAT IS THE DESIGN.
//
//     Status s = faults_->Intercept(<this call's CallSite>, id_);
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

FileLock::~FileLock() = default;

WritableFile::WritableFile(FaultController* faults, HandleId id) : faults_(faults), id_(id) {}
WritableFile::~WritableFile() = default;

Status WritableFile::Append(Slice data) {
  Status s = faults_->Intercept(CallSite::kWritableFileAppend, id_);
  if (!s.ok()) return s;
  return DoAppend(data);
}

Status WritableFile::Flush() {
  Status s = faults_->Intercept(CallSite::kWritableFileFlush, id_);
  if (!s.ok()) return s;
  return DoFlush();
}

Status WritableFile::Sync() {
  Status s = faults_->Intercept(CallSite::kWritableFileSync, id_);
  if (!s.ok()) return s;
  return DoSync();
}

Status WritableFile::Close() {
  // Idempotent, so a destructor may call it after an explicit Close.
  if (closed_) return Status::Ok();
  Status s = faults_->Intercept(CallSite::kWritableFileClose, id_);
  if (!s.ok()) return s;
  // closed_ is set BEFORE DoClose and regardless of what DoClose returns,
  // because close(2) releases the descriptor even when it reports EIO. The
  // error still reaches the caller, and it must: `Close` is a write call, and
  // treating it as bookkeeping is a known way to lose data. close(2) reports
  // EIO for writeback that failed after the last Sync, which is the last
  // moment anyone can learn that the data is gone (mutant BM7).
  closed_ = true;
  return DoClose();
}

SequentialFile::SequentialFile(FaultController* faults, HandleId id) : faults_(faults), id_(id) {}
SequentialFile::~SequentialFile() = default;

Status SequentialFile::Read(std::size_t n, Slice* result, char* scratch) {
  Status s = faults_->Intercept(CallSite::kSequentialFileRead, id_);
  if (!s.ok()) return s;
  return DoRead(n, result, scratch);
}

Status SequentialFile::Close() {
  if (closed_) return Status::Ok();
  Status s = faults_->Intercept(CallSite::kSequentialFileClose, id_);
  if (!s.ok()) return s;
  closed_ = true;
  return DoClose();
}

RandomAccessFile::RandomAccessFile(FaultController* faults, HandleId id) : faults_(faults), id_(id) {}
RandomAccessFile::~RandomAccessFile() = default;

Status RandomAccessFile::Read(uint64_t offset, std::size_t n, Slice* result, char* scratch) {
  Status s = faults_->Intercept(CallSite::kRandomAccessFileRead, id_);
  if (!s.ok()) return s;
  return DoRead(offset, n, result, scratch);
}

Status RandomAccessFile::Close() {
  if (closed_) return Status::Ok();
  Status s = faults_->Intercept(CallSite::kRandomAccessFileClose, id_);
  if (!s.ok()) return s;
  closed_ = true;
  return DoClose();
}

Directory::Directory(FaultController* faults, HandleId id) : faults_(faults), id_(id) {}
Directory::~Directory() = default;

Status Directory::Sync() {
  // Not decoration. A WAL created, written and fsynced is still losable if the
  // directory entry naming it was never made durable: the bytes survive and the
  // name does not. Section 7.2's gapless-file-number check is what turns that
  // loss into a failed open instead of silence.
  Status s = faults_->Intercept(CallSite::kDirectorySync, id_);
  if (!s.ok()) return s;
  return DoSync();
}

Status Directory::Close() {
  if (closed_) return Status::Ok();
  Status s = faults_->Intercept(CallSite::kDirectoryClose, id_);
  if (!s.ok()) return s;
  closed_ = true;
  return DoClose();
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
  Status s = faults_->Intercept(CallSite::kEnvNewWritableFile, id_);
  if (!s.ok()) return s;
  return DoNewWritableFile(path, out);
}

Status Env::NewSequentialFile(const std::string& path, SequentialFilePtr* out) {
  Status s = faults_->Intercept(CallSite::kEnvNewSequentialFile, id_);
  if (!s.ok()) return s;
  return DoNewSequentialFile(path, out);
}

Status Env::NewRandomAccessFile(const std::string& path, RandomAccessFilePtr* out) {
  Status s = faults_->Intercept(CallSite::kEnvNewRandomAccessFile, id_);
  if (!s.ok()) return s;
  return DoNewRandomAccessFile(path, out);
}

Status Env::NewDirectory(const std::string& path, DirectoryPtr* out) {
  Status s = faults_->Intercept(CallSite::kEnvNewDirectory, id_);
  if (!s.ok()) return s;
  return DoNewDirectory(path, out);
}

Status Env::GetChildren(const std::string& dir, std::vector<std::string>* out) {
  Status s = faults_->Intercept(CallSite::kEnvGetChildren, id_);
  if (!s.ok()) return s;
  return DoGetChildren(dir, out);
}

Status Env::GetFileSize(const std::string& path, uint64_t* out) {
  Status s = faults_->Intercept(CallSite::kEnvGetFileSize, id_);
  if (!s.ok()) return s;
  return DoGetFileSize(path, out);
}

Status Env::FileExists(const std::string& path, bool* out) {
  Status s = faults_->Intercept(CallSite::kEnvFileExists, id_);
  if (!s.ok()) return s;
  return DoFileExists(path, out);
}

Status Env::DeleteFile(const std::string& path) {
  Status s = faults_->Intercept(CallSite::kEnvDeleteFile, id_);
  if (!s.ok()) return s;
  return DoDeleteFile(path);
}

Status Env::RenameFile(const std::string& from, const std::string& to) {
  Status s = faults_->Intercept(CallSite::kEnvRenameFile, id_);
  if (!s.ok()) return s;
  return DoRenameFile(from, to);
}

Status Env::CreateDir(const std::string& path) {
  Status s = faults_->Intercept(CallSite::kEnvCreateDir, id_);
  if (!s.ok()) return s;
  return DoCreateDir(path);
}

Status Env::LockFile(const std::string& path, FileLockPtr* out) {
  Status s = faults_->Intercept(CallSite::kEnvLockFile, id_);
  if (!s.ok()) return s;
  return DoLockFile(path, out);
}

Status Env::UnlockFile(FileLockPtr lock) {
  Status s = faults_->Intercept(CallSite::kEnvUnlockFile, id_);
  if (!s.ok()) return s;
  return DoUnlockFile(std::move(lock));
}

}  // namespace rift
