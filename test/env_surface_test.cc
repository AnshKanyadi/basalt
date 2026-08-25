// The choke point, driven.
//
// scripts/cpp-scan.sh proves the SHAPE: one public non-virtual wrapper, one
// private Do*, one CallSite, names matching. That is a statement about the
// source. This file proves the BEHAVIOUR the shape exists to guarantee, which a
// source scan cannot see:
//
//   1. Every public wrapper calls Intercept with its OWN CallSite, exactly
//      once, BEFORE the implementation runs. A wrapper that intercepted with a
//      neighbour's enumerator would satisfy the scan perfectly and would make
//      every kill point and every injector aim at the wrong call.
//
//   2. When Intercept returns non-ok, the implementation is NOT invoked. This
//      is what a kill point IS. Without it the dead flag would stop reporting
//      but not stop acting, and "the engine cannot affect what recovery reads
//      after the kill" would be false.
//
// ON THE RETIREMENT RULE (DESIGN-B1 section 14.1.1). These tests are written
// before TestEnv, the durability observer, exists. They make NO durability
// claim: every assertion is about the order and arguments of calls, recorded by
// a harness-side recorder that the code under test cannot read. Nothing here
// asks the engine what it believes happened, so B1.3 retires none of it -- and
// this paragraph is the classification that step is required to perform rather
// than assume.
//
// The stubs below are test doubles, not an Env implementation. B1.2a lands the
// choke point with zero implementations behind it, which is what makes it
// separable from B1.2b; a recording double in a test file does not change that.
#include "env.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "call_site.h"
#include "fault_controller.h"

namespace rift {
namespace {

// The recorder. It observes; it is never consulted by the thing it observes.
class Recorder {
 public:
  void Intercepted(CallSite s) { log_.push_back(std::string("I:") + CallSiteName(s)); }
  void Implemented(CallSite s) { log_.push_back(std::string("D:") + CallSiteName(s)); }
  void Clear() { log_.clear(); }
  const std::vector<std::string>& log() const { return log_; }

 private:
  std::vector<std::string> log_;
};

class RecordingController : public FaultController {
 public:
  explicit RecordingController(Recorder* r) : rec_(r) {}
  Status Intercept(CallSite site, HandleId) override {
    rec_->Intercepted(site);
    if (fail_) return Status::Killed("injected");
    return Status::Ok();
  }
  void FailEverything() { fail_ = true; }

 private:
  Recorder* rec_;
  bool fail_ = false;
};

class StubWritableFile final : public WritableFile {
 public:
  StubWritableFile(FaultController* f, Recorder* r) : WritableFile(f, HandleId()), rec_(r) {}
  ~StubWritableFile() override = default;

 private:
  Status DoAppend(Slice) override { rec_->Implemented(CallSite::kWritableFileAppend); return Status::Ok(); }
  Status DoFlush() override { rec_->Implemented(CallSite::kWritableFileFlush); return Status::Ok(); }
  Status DoSync() override { rec_->Implemented(CallSite::kWritableFileSync); return Status::Ok(); }
  Status DoClose() override { rec_->Implemented(CallSite::kWritableFileClose); return Status::Ok(); }
  Recorder* rec_;
};

class StubSequentialFile final : public SequentialFile {
 public:
  StubSequentialFile(FaultController* f, Recorder* r) : SequentialFile(f, HandleId()), rec_(r) {}
  ~StubSequentialFile() override = default;

 private:
  Status DoRead(std::size_t, Slice*, char*) override { rec_->Implemented(CallSite::kSequentialFileRead); return Status::Ok(); }
  Status DoClose() override { rec_->Implemented(CallSite::kSequentialFileClose); return Status::Ok(); }
  Recorder* rec_;
};

class StubRandomAccessFile final : public RandomAccessFile {
 public:
  StubRandomAccessFile(FaultController* f, Recorder* r) : RandomAccessFile(f, HandleId()), rec_(r) {}
  ~StubRandomAccessFile() override = default;

 private:
  Status DoRead(uint64_t, std::size_t, Slice*, char*) override { rec_->Implemented(CallSite::kRandomAccessFileRead); return Status::Ok(); }
  Status DoClose() override { rec_->Implemented(CallSite::kRandomAccessFileClose); return Status::Ok(); }
  Recorder* rec_;
};

class StubDirectory final : public Directory {
 public:
  StubDirectory(FaultController* f, Recorder* r) : Directory(f, HandleId()), rec_(r) {}
  ~StubDirectory() override = default;

 private:
  Status DoSync() override { rec_->Implemented(CallSite::kDirectorySync); return Status::Ok(); }
  Status DoClose() override { rec_->Implemented(CallSite::kDirectoryClose); return Status::Ok(); }
  Recorder* rec_;
};

class StubEnv final : public Env {
 public:
  StubEnv(FaultController* f, Recorder* r) : Env(f, HandleId()), rec_(r) {}
  ~StubEnv() override = default;

 private:
  Status DoNewWritableFile(const std::string&, WritableFilePtr*) override { rec_->Implemented(CallSite::kEnvNewWritableFile); return Status::Ok(); }
  Status DoNewSequentialFile(const std::string&, SequentialFilePtr*) override { rec_->Implemented(CallSite::kEnvNewSequentialFile); return Status::Ok(); }
  Status DoNewRandomAccessFile(const std::string&, RandomAccessFilePtr*) override { rec_->Implemented(CallSite::kEnvNewRandomAccessFile); return Status::Ok(); }
  Status DoNewDirectory(const std::string&, DirectoryPtr*) override { rec_->Implemented(CallSite::kEnvNewDirectory); return Status::Ok(); }
  Status DoGetChildren(const std::string&, std::vector<std::string>*) override { rec_->Implemented(CallSite::kEnvGetChildren); return Status::Ok(); }
  Status DoGetFileSize(const std::string&, uint64_t*) override { rec_->Implemented(CallSite::kEnvGetFileSize); return Status::Ok(); }
  Status DoFileExists(const std::string&, bool*) override { rec_->Implemented(CallSite::kEnvFileExists); return Status::Ok(); }
  Status DoDeleteFile(const std::string&) override { rec_->Implemented(CallSite::kEnvDeleteFile); return Status::Ok(); }
  Status DoRenameFile(const std::string&, const std::string&) override { rec_->Implemented(CallSite::kEnvRenameFile); return Status::Ok(); }
  Status DoCreateDir(const std::string&) override { rec_->Implemented(CallSite::kEnvCreateDir); return Status::Ok(); }
  Status DoLockFile(const std::string&, FileLockPtr*) override { rec_->Implemented(CallSite::kEnvLockFile); return Status::Ok(); }
  Status DoUnlockFile(FileLockPtr) override { rec_->Implemented(CallSite::kEnvUnlockFile); return Status::Ok(); }
  Recorder* rec_;
};

// Drives every public wrapper once, in CallSite order, and returns the recorded
// log. `fresh` objects are used for the four Close() calls so idempotence does
// not swallow one.
struct Harness {
  Recorder rec;
  RecordingController ctl{&rec};
};

void DriveEveryWrapper(Harness* h) {
  StubEnv env(&h->ctl, &h->rec);
  std::string path = "p";
  WritableFilePtr wf; SequentialFilePtr sf; RandomAccessFilePtr rf;
  DirectoryPtr dir; FileLockPtr lock;
  std::vector<std::string> children; uint64_t size = 0; bool exists = false;

  env.NewWritableFile(path, &wf);
  env.NewSequentialFile(path, &sf);
  env.NewRandomAccessFile(path, &rf);
  env.NewDirectory(path, &dir);
  env.GetChildren(path, &children);
  env.GetFileSize(path, &size);
  env.FileExists(path, &exists);
  env.DeleteFile(path);
  env.RenameFile(path, path);
  env.CreateDir(path);
  env.LockFile(path, &lock);
  env.UnlockFile(nullptr);

  StubWritableFile w(&h->ctl, &h->rec);
  char scratch[8]; Slice result;
  w.Append(Slice("x", 1));
  w.Flush();
  w.Sync();
  w.Close();

  StubSequentialFile s(&h->ctl, &h->rec);
  s.Read(1, &result, scratch);
  s.Close();

  StubRandomAccessFile r(&h->ctl, &h->rec);
  r.Read(0, 1, &result, scratch);
  r.Close();

  StubDirectory d(&h->ctl, &h->rec);
  d.Sync();
  d.Close();
}

constexpr CallSite kEveryCallSiteInDrivenOrder[] = {
    CallSite::kEnvNewWritableFile,   CallSite::kEnvNewSequentialFile,
    CallSite::kEnvNewRandomAccessFile, CallSite::kEnvNewDirectory,
    CallSite::kEnvGetChildren,       CallSite::kEnvGetFileSize,
    CallSite::kEnvFileExists,        CallSite::kEnvDeleteFile,
    CallSite::kEnvRenameFile,        CallSite::kEnvCreateDir,
    CallSite::kEnvLockFile,          CallSite::kEnvUnlockFile,
    CallSite::kWritableFileAppend,   CallSite::kWritableFileFlush,
    CallSite::kWritableFileSync,     CallSite::kWritableFileClose,
    CallSite::kSequentialFileRead,   CallSite::kSequentialFileClose,
    CallSite::kRandomAccessFileRead, CallSite::kRandomAccessFileClose,
    CallSite::kDirectorySync,        CallSite::kDirectoryClose,
};

// Twenty-two, and asserted here as well as by scripts/cpp-scan.sh, because the
// scan checks the source and this checks the code that was built from it.
constexpr std::size_t kExpectedCallSites = 22;

TEST(EnvSurface, EveryWrapperInterceptsWithItsOwnCallSiteBeforeImplementing) {
  ASSERT_EQ(std::size(kEveryCallSiteInDrivenOrder), kExpectedCallSites);
  Harness h;
  DriveEveryWrapper(&h);

  std::vector<std::string> want;
  for (CallSite s : kEveryCallSiteInDrivenOrder) {
    want.push_back(std::string("I:") + CallSiteName(s));
    want.push_back(std::string("D:") + CallSiteName(s));
  }
  EXPECT_EQ(h.rec.log(), want)
      << "a wrapper either skipped interception, intercepted with the wrong "
         "CallSite, or ran its implementation first";
}

// What a kill point IS. If the implementation still ran, the dead flag would
// stop the engine reporting without stopping it acting.
TEST(EnvSurface, NonOkInterceptSuppressesTheImplementationEverywhere) {
  Harness h;
  h.ctl.FailEverything();
  DriveEveryWrapper(&h);

  for (const std::string& e : h.rec.log()) {
    EXPECT_EQ(e.substr(0, 2), "I:")
        << "implementation " << e << " ran after Intercept refused the call";
  }
  EXPECT_EQ(h.rec.log().size(), kExpectedCallSites);
}

TEST(EnvSurface, CloseIsIdempotentAndIsNotASecondKillPoint) {
  Harness h;
  StubWritableFile w(&h.ctl, &h.rec);
  EXPECT_TRUE(w.Close().ok());
  h.rec.Clear();
  EXPECT_TRUE(w.Close().ok());
  EXPECT_TRUE(h.rec.log().empty())
      << "a second Close must not reach the fault controller; a destructor may "
         "call Close after an explicit one, and that must not consume a kill "
         "point that the sweep is counting";
}

}  // namespace
}  // namespace rift
