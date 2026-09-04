// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ansh Kanyadi

#include "engine_surface.h"

#include <utility>
#include <vector>

#include "basalt/basalt.h"
#include "basalt/basalt_cxx.h"
#include "basalt/check.h"
#include "basalt/db.h"

namespace basalt {
namespace rig {
namespace {

// ---------------------------------------------------------------- the C++ one

class CxxSurface : public EngineSurface {
 public:
  Status::Code Open(Env* env, const std::string& dir,
                    const wal::Caps& caps) override {
    std::unique_ptr<DB> db;
    const Status s = DB::Open(env, dir, caps, &db);
    if (s.ok()) db_ = std::move(db);
    return s.code();
  }
  bool IsOpen() const override { return db_ != nullptr; }

  Status::Code Put(const std::string& key, const std::string& value) override {
    WriteBatch b;
    b.Set(Slice(key), Slice(value));
    wal::SeqNum s = 0;
    return db_->Write(b, &s).code();
  }

  Status::Code Sync(uint64_t* watermark) override {
    wal::SeqNum mark = 0;
    const Status s = db_->Sync(&mark);
    *watermark = mark;
    return s.code();
  }

  uint64_t DurableSeq() const override { return db_->DurableSeq(); }

  void TakeSnapshot() override { snapshot_ = db_->NewSnapshot(); }
  void ReleaseSnapshot() override {
    if (snapshot_ == nullptr) return;
    (void)snapshot_->Close();
    snapshot_.reset();
  }

  std::map<std::string, std::string> ExtractState() const override {
    std::map<std::string, std::string> out;
    std::unique_ptr<Iterator> it = db_->NewIter(IterOptions());
    for (bool ok = it->First(); ok; ok = it->Next()) {
      out[it->Key().ToString()] = it->Value().ToString();
    }
    (void)it->Close();
    return out;
  }

  void Close() override {
    ReleaseSnapshot();
    if (db_ == nullptr) return;
    (void)db_->Close();
    db_.reset();
  }

 private:
  std::unique_ptr<DB> db_;
  std::unique_ptr<Snapshot> snapshot_;
};

// ------------------------------------------------------------------ the C one

// A BOUNDARY-ONLY CODE ARRIVING HERE IS A BOUNDARY BUG, NOT AN ENGINE OUTCOME,
// and the difference matters enough to abort on.
//
// BASALT_INTERNAL and BASALT_BUFFER_TOO_SMALL correspond to no engine state,
// so there is no Status::Code to map them onto. Mapping them onto the nearest
// one -- kIoError, say -- would feed the adjudicator a verdict about the DATA
// derived from a defect in the TRANSLATION, and the sweep would report a
// durability violation with a cause that has nothing to do with durability.
// Every call site below either cannot produce them or handles them first.
Status::Code EngineCode(basalt_status st) {
  BASALT_CHECK_MSG(st < BASALT_INTERNAL,
                   "a boundary-only status reached the sweep's adjudication; "
                   "it describes the C API and not the engine");
  return static_cast<Status::Code>(st);
}

class CSurface : public EngineSurface {
 public:
  ~CSurface() override {
    Close();
    if (env_ != nullptr) basalt_env_free(env_);
  }

  Status::Code Open(Env* env, const std::string& dir,
                    const wal::Caps& caps) override {
    if (env_ != nullptr) basalt_env_free(env_);
    // BORROWED: the sweep owns the TestEnvironment and reuses it after this
    // surface is gone. basalt_env_free releases the wrapper and leaves the
    // caller's Env alone -- test/c_api_test.cc holds that to account.
    env_ = BorrowEnv(env);
    BASALT_CHECK(env_ != nullptr);

    basalt_caps c;
    c.max_record_bytes = caps.max_record_bytes;
    c.wal_buffer_bytes = caps.wal_buffer_bytes;
    c.flush_bytes = caps.flush_bytes;
    c.busy_bytes = caps.busy_bytes;

    const basalt_status st =
        basalt_db_open_env(env_, dir.data(), dir.size(), &c, &db_);
    if (st != BASALT_OK) db_ = nullptr;
    return EngineCode(st);
  }
  bool IsOpen() const override { return db_ != nullptr; }

  Status::Code Put(const std::string& key, const std::string& value) override {
    basalt_batch* b = basalt_batch_new();
    BASALT_CHECK(b != nullptr);
    const basalt_status set =
        basalt_batch_set(b, key.data(), key.size(), value.data(), value.size());
    BASALT_CHECK(set == BASALT_OK);
    uint64_t seq = 0;
    const basalt_status st = basalt_db_write(db_, b, &seq);
    basalt_batch_free(b);
    return EngineCode(st);
  }

  Status::Code Sync(uint64_t* watermark) override {
    return EngineCode(basalt_db_sync(db_, watermark));
  }

  uint64_t DurableSeq() const override { return basalt_db_durable_seq(db_); }

  void TakeSnapshot() override {
    const basalt_status st = basalt_db_snapshot(db_, &snapshot_);
    if (st != BASALT_OK) snapshot_ = nullptr;
  }
  void ReleaseSnapshot() override {
    if (snapshot_ == nullptr) return;
    (void)basalt_snapshot_close(snapshot_);
    snapshot_ = nullptr;
  }

  std::map<std::string, std::string> ExtractState() const override {
    std::map<std::string, std::string> out;
    basalt_iter* it = nullptr;
    if (basalt_db_iter(db_, nullptr, 0, nullptr, 0, &it) != BASALT_OK)
      return out;
    int valid = 0;
    if (basalt_iter_seek(it, BASALT_SEEK_FIRST, nullptr, 0, &valid) !=
            BASALT_OK ||
        valid == 0) {
      basalt_iter_free(it);
      return out;
    }

    // THE BLOCK PATH, WITH A REAL GROW-AND-RETRY, AND THAT IS THE POINT OF
    // DRAINING IT THIS WAY RATHER THAN ONE PAIR AT A TIME.
    //
    // A block size of one with enormous buffers would read the same data and
    // would never exercise the short-block contract -- the held pair, the
    // needed-capacity report, the retry -- which is the part of this boundary
    // with state in it and therefore the part a crash can corrupt. Starting
    // small and growing on demand puts that machinery on the swept path.
    const std::size_t kBlock = 8;
    std::vector<uint32_t> klen(kBlock), vlen(kBlock);
    std::vector<char> keys(256), vals(256);
    for (;;) {
      std::size_t filled = 0, ku = 0, vu = 0;
      const basalt_status st = basalt_iter_block(
          it, 1, kBlock, klen.data(), vlen.data(), keys.data(), keys.size(),
          &ku, vals.data(), vals.size(), &vu, &filled);
      if (st == BASALT_BUFFER_TOO_SMALL) {
        // ku and vu carry what THAT PAIR needs, so one grow always suffices.
        if (ku > keys.size()) keys.resize(ku);
        if (vu > vals.size()) vals.resize(vu);
        continue;
      }
      BASALT_CHECK(st == BASALT_OK);
      if (filled == 0) break;
      std::size_t ko = 0, vo = 0;
      for (std::size_t i = 0; i < filled; i++) {
        out[std::string(keys.data() + ko, klen[i])] =
            std::string(vals.data() + vo, vlen[i]);
        ko += klen[i];
        vo += vlen[i];
      }
    }
    basalt_iter_free(it);
    return out;
  }

  void Close() override {
    ReleaseSnapshot();
    if (db_ == nullptr) return;
    (void)basalt_db_close(db_);
    db_ = nullptr;
  }

 private:
  basalt_env* env_ = nullptr;
  basalt_db* db_ = nullptr;
  basalt_snapshot* snapshot_ = nullptr;
};

}  // namespace

const char* SweepSurfaceName(SweepSurface s) {
  switch (s) {  // NO default: arm
    case SweepSurface::kCxx:
      return "cxx";
    case SweepSurface::kC:
      return "c";
  }
  BASALT_UNREACHABLE("SweepSurface holds a value no enumerator names");
}

std::unique_ptr<EngineSurface> NewSurface(SweepSurface which) {
  switch (which) {  // NO default: arm
    case SweepSurface::kCxx:
      return std::unique_ptr<EngineSurface>(new CxxSurface());
    case SweepSurface::kC:
      return std::unique_ptr<EngineSurface>(new CSurface());
  }
  BASALT_UNREACHABLE("SweepSurface holds a value no enumerator names");
}

}  // namespace rig
}  // namespace basalt
