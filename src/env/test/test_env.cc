#include "test_env.h"

#include <algorithm>
#include <functional>
#include <unistd.h>

#include <utility>

#include "basalt/check.h"

namespace basalt {
namespace testenv {

const char* InjectionName(Injection injection) {
  switch (injection) {  // NO default: arm
    case Injection::kNone:      return "none";
    case Injection::kIoError:   return "io-error";
    case Injection::kDiskFull:  return "disk-full";
    case Injection::kSyncLoss:  return "sync-loss";
    case Injection::kTornSync:  return "torn-sync";
    case Injection::kSectorSubsetTornSync: return "sector-subset-torn-sync";
    case Injection::kTornFlush: return "torn-flush";
    case Injection::kKill:      return "kill";
    case Injection::kKillAfterEffect: return "kill-after-effect";
  }
  BASALT_UNREACHABLE("Injection holds a value no enumerator names");
}

bool SuspendsExactness(Injection injection) {
  // The registry, applied. kSyncLoss is ExactnessSuspendingInjector::kLyingSync:
  // the device returned success and promoted nothing, the engine is blameless,
  // and holding it to exactness would report the engine for the disk's crime.
  //
  // kSectorSubsetTornSync is ExactnessSuspendingInjector::kSectorSubsetTornSync:
  // a device that promoted an arbitrary set of sectors rather than a prefix,
  // violating fsync's own ordering guarantee. Against such a device the
  // engine's obligation is narrower and still real: DETECT AND REFUSE, which
  // section 5.4(d) already does.
  //
  // kTornSync IS NOT A MEMBER, and that distinction was got wrong once. B1-D5
  // ruled PREFIX granularity as THE CONTRACT MODEL -- section 7.4's two-element
  // set, R in {G_{k-1}, G_k}, is that exact case, and the engine is held to
  // exactness under it. Classifying it as suspending marked bankable runs as
  // characterization-only: conservative, and wrong, and it would have made the
  // two-element set untestable as evidence at B1.9a.
  switch (injection) {  // NO default: arm -- a new injector must be classified
    case Injection::kSyncLoss:
    case Injection::kSectorSubsetTornSync:
      return true;
    case Injection::kNone:
    case Injection::kIoError:
    case Injection::kDiskFull:
    case Injection::kTornSync:
    case Injection::kTornFlush:
    case Injection::kKill:
    case Injection::kKillAfterEffect:
      return false;
  }
  BASALT_UNREACHABLE("Injection holds a value no enumerator names");
}

void FaultPlan::At(uint64_t ordinal, Injection injection, uint64_t prefix_bytes) {
  BASALT_CHECK(ordinal > 0);
  PlannedFault f;
  f.injection = injection;
  f.prefix_bytes = prefix_bytes;
  at_[ordinal] = f;
}

PlannedFault FaultPlan::Lookup(uint64_t ordinal) const {
  auto it = at_.find(ordinal);
  if (it == at_.end()) return PlannedFault();
  return it->second;
}

namespace {

std::string DirNameOf(const std::string& path) {
  const std::size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) return ".";
  if (slash == 0) return "/";
  return path.substr(0, slash);
}

struct FileState {
  bool present = false;        // exists now, in the content view
  bool entry_durable = false;  // the NAME survives a kill
  std::string buf;             // appended, not flushed -- not on the device
  std::string content;         // flushed, visible now
  std::string durable;         // survives a kill
};

}  // namespace

// ---------------------------------------------------------------------------

class TestEnvironment::Impl {
 public:
  explicit Impl(FaultPlan plan) : plan_(std::move(plan)) {}

  // ---- the fault decision, made once per Env call, in one place.
  struct Decision {
    Injection injection = Injection::kNone;
    uint64_t prefix_bytes = 0;
    Status status = Status::Ok();
    bool kill = false;
  };

  Decision Begin(CallSite site, HandleId handle) {
    Decision d;
    if (dead_) {
      post_kill_calls_++;
      // Not an engine bug: it means the workload never checked a Status. A
      // runaway loop inside a dead Env would destroy the run that would have
      // explained it, which is the worst possible failure signal.
      BASALT_CHECK(post_kill_calls_ <= kMaxPostKillCalls);
      d.status = Status::Killed("env is dead");
      d.injection = Injection::kKill;
      return d;
    }
    const uint64_t ord = ++ordinal_;
    observed_[site]++;
    const PlannedFault f = plan_.Lookup(ord);
    d.injection = f.injection;
    d.prefix_bytes = f.prefix_bytes;

    if (SuspendsExactness(f.injection) && !exactness_suspended_) {
      // AT THE POINT OF ENABLING, not at the point of reporting.
      exactness_suspended_ = true;
      suspending_ = (f.injection == Injection::kSyncLoss)
                        ? ExactnessSuspendingInjector::kLyingSync
                        : ExactnessSuspendingInjector::kSectorSubsetTornSync;
      BASALT_CHECK(f.injection == Injection::kSyncLoss ||
                 f.injection == Injection::kSectorSubsetTornSync);
    }

    const std::string path = PathOf(handle);
    uint64_t durable_before = 0;
    uint64_t durable_after = 0;
    bool break_after_ledger = false;
    switch (f.injection) {  // NO default: arm
      case Injection::kNone:
        break;
      case Injection::kIoError:
        d.status = Status::IoError("injected at ordinal " + std::to_string(ord));
        break;
      case Injection::kDiskFull:
        d.status = Status::DiskFull("injected at ordinal " + std::to_string(ord));
        break;
      case Injection::kSyncLoss:
        // Returns OK -- the caller is told the data is durable -- and promotes
        // nothing. The implementation still runs, because the caller must be
        // unable to tell; what is suppressed is the promotion inside it. This
        // is the device lying, so the engine is blameless, and section 7.5
        // makes such a run structurally incapable of being counted as evidence.
        suppress_next_promotion_ = true;
        break;
      case Injection::kTornFlush:
        TornFlush(path, f.prefix_bytes);
        d.kill = true;
        break;
      case Injection::kTornSync:
        durable_before = DurableSizeOf(path);
        TornSync(path, f.prefix_bytes);
        durable_after = DurableSizeOf(path);
        break_after_ledger = true;
        break;
      case Injection::kSectorSubsetTornSync:
        durable_before = DurableSizeOf(path);
        SectorSubsetTornSync(path, f.prefix_bytes);
        durable_after = DurableSizeOf(path);
        break_after_ledger = true;
        break;
      case Injection::kKill:
        d.kill = true;
        break;
      case Injection::kKillAfterEffect:
        // The implementation RUNS. The kill happens in End(), after the effect
        // has landed and before its Status reaches the caller.
        pending_after_kill_ = true;
        break;
    }

    LedgerEntry e;
    e.ordinal = ord;
    e.site = site;
    e.path = path;
    e.injection = f.injection;
    // A TORN Sync PROMOTES SOMETHING, AND THE LEDGER HAS TO SAY WHAT.
    //
    // It was first recorded as promoted=false, because DoSync never runs and
    // RecordPromotion is what sets the flag. But a torn Sync whose prefix
    // happens to cover the whole newly covered extent DID advance the durable
    // image -- and an oracle reading `promoted=false` then refuses to offer the
    // in-flight element of the recovery set, and reports the engine for landing
    // exactly where the ledger's own bytes say it should.
    //
    // The sweep found this on its first run with torn modes enabled. A harness
    // record that UNDER-reports is as damaging as an engine that over-reports:
    // both make the oracle wrong, and this one blames the engine for it.
    if (durable_after > durable_before) {
      e.promoted = true;
      e.durable_bytes_after = durable_after;
    }
    ledger_.push_back(e);
    if (break_after_ledger) d.kill = true;

    if (d.kill) {
      DoKill();
      d.status = Status::Killed("killed at ordinal " + std::to_string(ord));
    }
    return d;
  }

  // Records the size of an Append, on the entry the call just made.
  void RecordAppendBytes(uint64_t bytes) {
    BASALT_CHECK(!ledger_.empty());
    ledger_.back().append_bytes = bytes;
  }

  // Records what a successfully returning call actually promoted. Called by the
  // file objects AFTER the implementation ran, so the ledger's promotion column
  // is the harness's own record of what was promised rather than a prediction.
  void RecordPromotion(const std::string& path, uint64_t durable_bytes,
                       bool promoted) {
    BASALT_CHECK(!ledger_.empty());
    ledger_.back().promoted = promoted;
    ledger_.back().durable_bytes_after = durable_bytes;
    BASALT_CHECK(ledger_.back().path == path);
  }

  // The second half of an Env call. Consumes no ordinal.
  Status End(Status s) {
    if (!pending_after_kill_) return s;
    pending_after_kill_ = false;
    DoKill();
    // The effect is durable and the caller is told nothing but that it died.
    return Status::Killed("killed after the effect landed");
  }

  bool TakeSuppressPromotion() {
    const bool v = suppress_next_promotion_;
    suppress_next_promotion_ = false;
    return v;
  }

  void RegisterHandle(HandleId handle, const std::string& path) {
    handles_[handle.value] = path;
  }
  void ForgetHandle(HandleId handle) { handles_.erase(handle.value); }

  std::string PathOf(HandleId handle) const {
    auto it = handles_.find(handle.value);
    return it == handles_.end() ? std::string() : it->second;
  }

  // ---- the filesystem model
  FileState* Find(const std::string& path) {
    auto it = files_.find(path);
    return it == files_.end() ? nullptr : &it->second;
  }
  FileState& Create(const std::string& path) {
    FileState& f = files_[path];
    f.present = true;
    f.buf.clear();
    f.content.clear();
    return f;
  }
  const FileState* FindConst(const std::string& path) const {
    auto it = files_.find(path);
    return it == files_.end() ? nullptr : &it->second;
  }
  const std::map<std::string, FileState>& files() const { return files_; }
  std::map<std::string, FileState>& files() { return files_; }

  void Flush(const std::string& path) {
    FileState* f = Find(path);
    if (f == nullptr) return;
    f->content += f->buf;
    f->buf.clear();
  }

  void TornFlush(const std::string& path, uint64_t prefix) {
    FileState* f = Find(path);
    if (f == nullptr) return;
    const std::size_t k = std::min<std::size_t>(prefix, f->buf.size());
    f->content += f->buf.substr(0, k);
    f->buf.clear();
  }

  void set_promotion_hook(TestEnvironment::PromotionHook hook, void* ctx) {
    hook_ = hook;
    hook_ctx_ = ctx;
  }
  void FireHook() {
    if (hook_ != nullptr) hook_(hook_ctx_, Image());
  }

  void Sync(const std::string& path) {
    FileState* f = Find(path);
    if (f == nullptr) return;
    f->content += f->buf;
    f->buf.clear();
    f->durable = f->content;
    FireHook();
  }

  void TornSync(const std::string& path, uint64_t prefix) {
    FileState* f = Find(path);
    if (f == nullptr) return;
    f->content += f->buf;
    f->buf.clear();
    const std::size_t already = f->durable.size();
    const std::size_t k = std::min<std::size_t>(already + prefix, f->content.size());
    f->durable = f->content.substr(0, k);
    FireHook();
  }

  // Promotes the whole newly covered extent EXCEPT one 4 KiB sector, which is
  // left as it was before the Sync. The result is not a prefix, so a GROUP_END
  // can be durable while an earlier record in its group is not -- which is the
  // shape no correct device produces and the reason this injector suspends.
  uint64_t DurableSizeOf(const std::string& path) const {
    const FileState* f = FindConst(path);
    return f == nullptr ? 0 : static_cast<uint64_t>(f->durable.size());
  }

  void SectorSubsetTornSync(const std::string& path, uint64_t sector_index) {
    FileState* f = Find(path);
    if (f == nullptr) return;
    f->content += f->buf;
    f->buf.clear();
    const std::size_t already = f->durable.size();
    std::string promoted = f->content;
    const std::size_t hole_start = already + sector_index * kSectorBytes;
    if (hole_start < promoted.size()) {
      const std::size_t hole_end =
          std::min<std::size_t>(hole_start + kSectorBytes, promoted.size());
      for (std::size_t i = hole_start; i < hole_end; ++i) {
        // Beyond the old durable extent there was nothing, so the sector reads
        // as zeros -- which section 5.3.1's reserved type 0 makes unmistakable
        // for a record, and which the engine is therefore obliged to DETECT.
        promoted[i] = (i < already) ? f->durable[i] : '\0';
      }
    }
    f->durable = promoted;
    FireHook();
  }

  void SyncDirectory(const std::string& dir) {
    std::vector<std::string> gone;
    for (auto& kv : files_) {
      if (DirNameOf(kv.first) != dir) continue;
      kv.second.entry_durable = kv.second.present;
      if (!kv.second.present) gone.push_back(kv.first);
    }
    for (const std::string& p : gone) files_.erase(p);
    FireHook();
  }

  void DoKill() {
    if (dead_) return;
    dead_ = true;
    std::vector<std::string> vanished;
    for (auto& kv : files_) {
      FileState& f = kv.second;
      if (!f.entry_durable) { vanished.push_back(kv.first); continue; }
      f.present = true;         // an unlink that was never synced is undone
      f.buf.clear();
      f.content = f.durable;    // content = durable, exactly
    }
    for (const std::string& p : vanished) files_.erase(p);
    if (plan_.kill_mode == KillMode::kRealExit) {
      // No destructor runs, no heap survives. The rig that asked for this mode
      // is responsible for having put the durable image somewhere the parent
      // can read; nothing here can run after this line.
      ::_exit(kRealExitStatus);
    }
  }

  DurableImage Image() const {
    DurableImage image;
    for (const auto& kv : files_) {
      if (!kv.second.entry_durable) continue;
      image[kv.first] = kv.second.durable;
    }
    return image;
  }

  void SeedFromImage(const DurableImage& image) {
    files_.clear();
    for (const auto& kv : image) {
      FileState f;
      f.present = true;
      f.entry_durable = true;
      f.content = kv.second;
      f.durable = kv.second;
      files_[kv.first] = f;
    }
  }

  const std::vector<LedgerEntry>& ledger() const { return ledger_; }
  uint64_t ordinal() const { return ordinal_; }
  bool dead() const { return dead_; }
  int post_kill_calls() const { return post_kill_calls_; }
  bool exactness_suspended() const { return exactness_suspended_; }
  ExactnessSuspendingInjector suspending() const {
    BASALT_CHECK(exactness_suspended_);
    return suspending_;
  }
  uint64_t observed(CallSite s) const {
    auto it = observed_.find(s);
    return it == observed_.end() ? 0 : it->second;
  }

 private:
  FaultPlan plan_;
  std::map<std::string, FileState> files_;
  // Keyed by the handle's INTEGER id, not by its address. Section 6.1 bans
  // pointer-keyed containers outright, and the reason bites here rather than
  // abstractly: this map decides which path a fault is injected against, so
  // an address-ordered version would make a fault schedule depend on the
  // allocator the first time anyone iterated it.
  std::map<uint64_t, std::string> handles_;
  std::map<CallSite, uint64_t> observed_;
  std::vector<LedgerEntry> ledger_;
  uint64_t ordinal_ = 0;
  bool dead_ = false;
  int post_kill_calls_ = 0;
  TestEnvironment::PromotionHook hook_ = nullptr;
  void* hook_ctx_ = nullptr;
  bool pending_after_kill_ = false;
  bool suppress_next_promotion_ = false;
  bool exactness_suspended_ = false;
  ExactnessSuspendingInjector suspending_ = ExactnessSuspendingInjector::kLyingSync;
};

// ---------------------------------------------------------------------------

namespace {

class TestController final : public FaultController {
 public:
  explicit TestController(TestEnvironment::Impl* impl) : impl_(impl) {}
  Status Intercept(CallSite site, HandleId handle) override {
    last_ = impl_->Begin(site, handle);
    return last_.status;
  }
  Status AfterEffect(CallSite, HandleId, Status s) override { return impl_->End(s); }
  const TestEnvironment::Impl::Decision& last() const { return last_; }

 private:
  TestEnvironment::Impl* impl_;
  TestEnvironment::Impl::Decision last_;
};

class TestWritableFile final : public WritableFile {
 public:
  TestWritableFile(FaultController* f, HandleId id, TestEnvironment::Impl* impl, std::string path)
      : WritableFile(f, id), handle_(id), impl_(impl), path_(std::move(path)) {
    impl_->RegisterHandle(handle_, path_);
  }
  ~TestWritableFile() override { impl_->ForgetHandle(handle_); }

 private:
  Status DoAppend(Slice data) override {
    FileState* f = impl_->Find(path_);
    if (f == nullptr) return Status::IoError("appending to a vanished file: " + path_);
    // WHAT THE ENGINE ASKED TO WRITE, recorded from the call rather than
    // inferred from a file size afterwards. See LedgerEntry::append_bytes.
    impl_->RecordAppendBytes(data.size());
    f->buf.append(data.data(), data.size());
    return Status::Ok();
  }
  Status DoFlush() override { impl_->Flush(path_); return Status::Ok(); }
  Status DoSync() override {
    const bool lied = impl_->TakeSuppressPromotion();
    if (!lied) impl_->Sync(path_);
    FileState* f = impl_->Find(path_);
    // The ledger records what actually happened, never what was reported: a
    // lying Sync leaves promoted=false beside a call that returned OK, and that
    // disagreement is the entire content of the injector.
    impl_->RecordPromotion(path_, f == nullptr ? 0 : f->durable.size(), !lied);
    return Status::Ok();
  }
  Status DoClose() override { impl_->Flush(path_); return Status::Ok(); }

  HandleId id_;
  HandleId handle_;
  TestEnvironment::Impl* impl_;
  std::string path_;
};

class TestSequentialFile final : public SequentialFile {
 public:
  TestSequentialFile(FaultController* f, HandleId id, TestEnvironment::Impl* impl, std::string path)
      : SequentialFile(f, id), handle_(id), impl_(impl), path_(std::move(path)) {
    impl_->RegisterHandle(handle_, path_);
  }
  ~TestSequentialFile() override { impl_->ForgetHandle(handle_); }

 private:
  Status DoRead(std::size_t n, Slice* result, char* scratch) override {
    FileState* f = impl_->Find(path_);
    if (f == nullptr) return Status::IoError("reading a vanished file: " + path_);
    const std::size_t avail = f->content.size() - std::min(pos_, f->content.size());
    const std::size_t k = std::min(n, avail);
    if (k > 0) std::copy(f->content.begin() + static_cast<long>(pos_),
                         f->content.begin() + static_cast<long>(pos_ + k), scratch);
    pos_ += k;
    *result = Slice(scratch, k);
    return Status::Ok();
  }
  Status DoClose() override { return Status::Ok(); }

  HandleId handle_;
  TestEnvironment::Impl* impl_;
  std::string path_;
  std::size_t pos_ = 0;
};

class TestRandomAccessFile final : public RandomAccessFile {
 public:
  TestRandomAccessFile(FaultController* f, HandleId id, TestEnvironment::Impl* impl, std::string path)
      : RandomAccessFile(f, id), handle_(id), impl_(impl), path_(std::move(path)) {
    impl_->RegisterHandle(handle_, path_);
  }
  ~TestRandomAccessFile() override { impl_->ForgetHandle(handle_); }

 private:
  Status DoRead(uint64_t offset, std::size_t n, Slice* result, char* scratch) override {
    FileState* f = impl_->Find(path_);
    if (f == nullptr) return Status::IoError("reading a vanished file: " + path_);
    const std::size_t off = static_cast<std::size_t>(offset);
    const std::size_t avail = f->content.size() - std::min(off, f->content.size());
    const std::size_t k = std::min(n, avail);
    if (k > 0) std::copy(f->content.begin() + static_cast<long>(off),
                         f->content.begin() + static_cast<long>(off + k), scratch);
    *result = Slice(scratch, k);
    return Status::Ok();
  }
  Status DoClose() override { return Status::Ok(); }

  HandleId handle_;
  TestEnvironment::Impl* impl_;
  std::string path_;
};

class TestDirectory final : public Directory {
 public:
  TestDirectory(FaultController* f, HandleId id, TestEnvironment::Impl* impl, std::string path)
      : Directory(f, id), handle_(id), impl_(impl), path_(std::move(path)) {
    impl_->RegisterHandle(handle_, path_);
  }
  ~TestDirectory() override { impl_->ForgetHandle(handle_); }

 private:
  Status DoSync() override {
    impl_->SyncDirectory(path_);
    impl_->RecordPromotion(path_, 0, true);
    return Status::Ok();
  }
  Status DoClose() override { return Status::Ok(); }

  HandleId handle_;
  TestEnvironment::Impl* impl_;
  std::string path_;
};

class TestFileLock final : public FileLock {
 public:
  explicit TestFileLock(std::string path) : path_(std::move(path)) {}
  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

class TestEnv final : public Env {
 public:
  TestEnv(FaultController* f, TestEnvironment::Impl* impl)
      : Env(f, HandleId()), faults_(f), impl_(impl) {}
  ~TestEnv() override = default;

 private:
  Status DoNewWritableFile(const std::string& path, WritableFilePtr* out) override {
    impl_->Create(path);
    *out = WritableFilePtr(new TestWritableFile(faults_, NextHandleId(), impl_, path));
    return Status::Ok();
  }
  Status DoNewSequentialFile(const std::string& path, SequentialFilePtr* out) override {
    if (impl_->Find(path) == nullptr) return Status::IoError("no such file: " + path);
    *out = SequentialFilePtr(new TestSequentialFile(faults_, NextHandleId(), impl_, path));
    return Status::Ok();
  }
  Status DoNewRandomAccessFile(const std::string& path, RandomAccessFilePtr* out) override {
    if (impl_->Find(path) == nullptr) return Status::IoError("no such file: " + path);
    *out = RandomAccessFilePtr(new TestRandomAccessFile(faults_, NextHandleId(), impl_, path));
    return Status::Ok();
  }
  Status DoNewDirectory(const std::string& path, DirectoryPtr* out) override {
    *out = DirectoryPtr(new TestDirectory(faults_, NextHandleId(), impl_, path));
    return Status::Ok();
  }

  // REVERSE-SORTED ON PURPOSE. Directory order is filesystem-dependent and
  // therefore nondeterministic; recovery must sort by parsed file number before
  // anything else. Handing back the worst legal order means an engine that
  // forgot to sort fails on the first test rather than on someone else's
  // filesystem. This is the C++ analogue of the map-iteration rule.
  Status DoGetChildren(const std::string& dir, std::vector<std::string>* out) override {
    out->clear();
    for (const auto& kv : impl_->files()) {
      if (!kv.second.present) continue;
      if (DirNameOf(kv.first) != dir) continue;
      const std::size_t slash = kv.first.find_last_of('/');
      out->push_back(slash == std::string::npos ? kv.first : kv.first.substr(slash + 1));
    }
    std::sort(out->begin(), out->end(), std::greater<std::string>());
    return Status::Ok();
  }

  Status DoGetFileSize(const std::string& path, uint64_t* out) override {
    FileState* f = impl_->Find(path);
    if (f == nullptr || !f->present) return Status::IoError("no such file: " + path);
    *out = static_cast<uint64_t>(f->content.size());
    return Status::Ok();
  }
  Status DoFileExists(const std::string& path, bool* out) override {
    FileState* f = impl_->Find(path);
    *out = (f != nullptr && f->present);
    return Status::Ok();
  }
  Status DoDeleteFile(const std::string& path) override {
    FileState* f = impl_->Find(path);
    if (f == nullptr || !f->present) return Status::IoError("no such file: " + path);
    // The unlink lands in `content` and NOT in `durable` until the directory is
    // synced: the name is gone now and comes back after a kill.
    f->present = false;
    if (!f->entry_durable) impl_->files().erase(path);
    return Status::Ok();
  }
  Status DoRenameFile(const std::string& from, const std::string& to) override {
    FileState* f = impl_->Find(from);
    if (f == nullptr || !f->present) return Status::IoError("no such file: " + from);
    // Both halves of a rename are directory-entry changes, and neither is
    // durable until the directory is synced. So the new name appears now and
    // vanishes on a kill, and the old name disappears now and COMES BACK on a
    // kill -- which is the injector that finds a missing directory sync around
    // an atomic rename (B2's manifest swap).
    FileState moved = *f;
    moved.entry_durable = false;
    f->present = false;
    if (!f->entry_durable) impl_->files().erase(from);
    impl_->files()[to] = moved;
    return Status::Ok();
  }
  Status DoCreateDir(const std::string&) override { return Status::Ok(); }
  Status DoLockFile(const std::string& path, FileLockPtr* out) override {
    *out = FileLockPtr(new TestFileLock(path));
    return Status::Ok();
  }
  Status DoUnlockFile(FileLockPtr lock) override {
    if (lock == nullptr) return Status::InvalidArgument("UnlockFile(nullptr)");
    return Status::Ok();
  }

  FaultController* faults_;
  TestEnvironment::Impl* impl_;
};

}  // namespace

// ---------------------------------------------------------------------------

TestEnvironment::TestEnvironment(FaultPlan plan)
    : impl_(new Impl(std::move(plan))) {
  controller_.reset(new TestController(impl_.get()));
  env_.reset(new TestEnv(controller_.get(), impl_.get()));
}
TestEnvironment::TestEnvironment() : TestEnvironment(FaultPlan()) {}
TestEnvironment::~TestEnvironment() = default;

const std::vector<LedgerEntry>& TestEnvironment::ledger() const { return impl_->ledger(); }
uint64_t TestEnvironment::ordinal() const { return impl_->ordinal(); }
bool TestEnvironment::dead() const { return impl_->dead(); }
int TestEnvironment::post_kill_calls() const { return impl_->post_kill_calls(); }
bool TestEnvironment::exactness_suspended() const { return impl_->exactness_suspended(); }
ExactnessSuspendingInjector TestEnvironment::suspending_injector() const {
  return impl_->suspending();
}
uint64_t TestEnvironment::observed(CallSite site) const { return impl_->observed(site); }
DurableImage TestEnvironment::Image() const { return impl_->Image(); }
std::string TestEnvironment::ContentNow(const std::string& path) const {
  const FileState* f = impl_->FindConst(path);
  return f == nullptr ? std::string() : f->content;
}
void TestEnvironment::Kill() { impl_->DoKill(); }
void TestEnvironment::set_promotion_hook(PromotionHook hook, void* ctx) {
  impl_->set_promotion_hook(hook, ctx);
}

std::vector<CallSite> TestEnvironment::unvisited() const {
  std::vector<CallSite> out;
  for (const CallSite s : AllCallSites()) {
    if (impl_->observed(s) == 0) out.push_back(s);
  }
  return out;
}

std::unique_ptr<TestEnvironment> TestEnvironment::FromImage(const DurableImage& image,
                                                            FaultPlan plan) {
  std::unique_ptr<TestEnvironment> t(new TestEnvironment(std::move(plan)));
  t->impl_->SeedFromImage(image);
  return t;
}

}  // namespace testenv
}  // namespace basalt
