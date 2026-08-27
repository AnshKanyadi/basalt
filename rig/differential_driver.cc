#include "differential_driver.h"

#include <cstdio>
#include <memory>
#include <vector>

#include "check.h"
#include "db.h"
#include "rng.h"
#include "test_env.h"

namespace rift {
namespace rig {
namespace {

const std::string kDir = "diff";

wal::Caps CapsFor(DiffRegime r) {
  wal::Caps c;
  switch (r) {  // NO default: arm
    case DiffRegime::kDefault:
      return c;
    case DiffRegime::kFlush:
    case DiffRegime::kCompact:
      // THE SAME THRESHOLD FOR BOTH, and the workload is what differs -- the
      // same choice the sweep's regimes make, for the same reason: changing
      // both would leave nothing to attribute a difference to.
      c.flush_bytes = 8u * 1024;
      return c;
  }
  RIFT_UNREACHABLE("DiffRegime holds a value no enumerator names");
}

std::string KeyOf(uint64_t i) {
  char buf[24];
  std::snprintf(buf, sizeof buf, "k%06llu", static_cast<unsigned long long>(i % 500));
  return buf;
}

// THE SUBMISSION LOG IS AUTHORED BEFORE EITHER ENGINE SEES IT. It is the
// sequence of operations the rig DECIDED to issue, not a transcript of what an
// engine did with them -- which is what makes it a shared input rather than a
// witness.
std::vector<DiffOp> Author(const DiffRunOptions& o) {
  std::vector<DiffOp> ops;
  Pcg64 rng(o.seed);
  const uint32_t count = o.regime == DiffRegime::kCompact ? o.ops * 4 : o.ops;
  for (uint32_t i = 0; i < count; ++i) {
    DiffOp op;
    const uint64_t roll = rng.Below(100);
    if (roll < 55) {
      op.kind = DiffOpKind::kSet;
      op.key = KeyOf(rng.Next());
      op.value = std::string(1 + rng.Below(40), 'v');
    } else if (roll < 70) {
      op.kind = DiffOpKind::kDelete;
      op.key = KeyOf(rng.Next());
    } else if (roll < 78) {
      // DELETE_RANGE IS CENTRAL, NOT INCIDENTAL. [A3] froze it in the interface
      // and the two engines implement it by ENTIRELY DIFFERENT MECHANISMS --
      // natively in the model, by range tombstones here -- so agreement on it
      // is the strongest evidence this rig can produce.
      op.kind = DiffOpKind::kDeleteRange;
      const uint64_t a = rng.Below(500);
      const uint64_t b = rng.Below(500);
      const uint64_t lo = a < b ? a : b;
      const uint64_t hi = a < b ? b : a;
      // A quarter of them unbounded on one side or both, so section 8.2's
      // clear-everything case is in the mix rather than only the easy shape.
      const uint64_t shape = rng.Below(4);
      op.start_bounded = shape != 1 && shape != 3;
      op.end_bounded = shape != 2 && shape != 3;
      if (op.start_bounded) op.key = KeyOf(lo);
      if (op.end_bounded) op.value = KeyOf(hi == lo ? hi + 1 : hi);
    } else if (roll < 92) {
      op.kind = DiffOpKind::kSync;
    } else if (roll < 96) {
      op.kind = DiffOpKind::kSnapshotTake;
    } else {
      op.kind = DiffOpKind::kSnapshotRelease;
    }
    ops.push_back(std::move(op));
  }
  return ops;
}

Bound BoundFrom(bool bounded, const std::string& key) {
  return bounded ? Bound::At(Slice(key)) : Bound::Unbounded();
}

// Issues the authored log against a DB, recording the sequence each op was
// assigned. Returns false once the engine stops accepting (a kill).
bool Issue(DB* db, std::vector<DiffOp>* ops,
           std::vector<std::unique_ptr<Snapshot>>* snapshots, uint64_t* watermark) {
  for (DiffOp& op : *ops) {
    switch (op.kind) {  // NO default: arm
      case DiffOpKind::kSet: {
        WriteBatch b;
        b.Set(Slice(op.key), Slice(op.value));
        wal::SeqNum s = 0;
        if (!db->Write(b, &s).ok()) return false;
        op.seq = s;
        break;
      }
      case DiffOpKind::kDelete: {
        WriteBatch b;
        b.Delete(Slice(op.key));
        wal::SeqNum s = 0;
        if (!db->Write(b, &s).ok()) return false;
        op.seq = s;
        break;
      }
      case DiffOpKind::kDeleteRange: {
        WriteBatch b;
        b.DeleteRange(BoundFrom(op.start_bounded, op.key),
                      BoundFrom(op.end_bounded, op.value));
        wal::SeqNum s = 0;
        if (!db->Write(b, &s).ok()) return false;
        op.seq = s;
        break;
      }
      case DiffOpKind::kSync: {
        wal::SeqNum mark = 0;
        if (!db->Sync(&mark).ok()) return false;
        // `w` IS CAPTURED FROM THE LIVE PROCESS, at every Sync, so the artifact
        // carries the last watermark the engine PROMISED before it died -- not
        // one the survivor was asked for afterwards.
        *watermark = db->DurableSeq();
        break;
      }
      case DiffOpKind::kSnapshotTake:
        snapshots->push_back(db->NewSnapshot());
        break;
      case DiffOpKind::kSnapshotRelease:
        if (!snapshots->empty()) {
          (void)snapshots->back()->Close();
          snapshots->pop_back();
        }
        break;
    }
  }
  return true;
}

std::map<std::string, std::string> ExtractState(const DB& db) {
  std::map<std::string, std::string> out;
  std::unique_ptr<Iterator> it = db.NewIter(IterOptions());
  for (bool ok = it->First(); ok; ok = it->Next()) {
    out[it->Key().ToString()] = it->Value().ToString();
  }
  (void)it->Close();
  return out;
}

}  // namespace

uint64_t DifferentialOrdinalCount(const DiffRunOptions& o) {
  testenv::TestEnvironment probe;
  std::unique_ptr<DB> db;
  if (!DB::Open(probe.env(), kDir, CapsFor(o.regime), &db).ok()) return 0;
  std::vector<DiffOp> ops = Author(o);
  std::vector<std::unique_ptr<Snapshot>> snapshots;
  uint64_t w = 0;
  (void)Issue(db.get(), &ops, &snapshots, &w);
  snapshots.clear();
  (void)db->Close();
  return probe.ordinal();
}

DiffArtifact RunDifferential(const DiffRunOptions& o) {
  DiffArtifact a;
  a.provenance.engine_commit = o.engine_commit;
  a.provenance.model_commit = o.model_commit;
  // THE STRING COMES FROM THE REGIME'S OWN NAME, never from a literal beside
  // the caps: a renamed regime cannot leave a stale label in an artifact.
  a.provenance.regime = DiffRegimeName(o.regime);
  a.provenance.seed = o.seed;
  const wal::Caps caps = CapsFor(o.regime);
  a.provenance.flush_bytes = caps.flush_bytes;
  a.provenance.wal_buffer_bytes = caps.wal_buffer_bytes;
  a.provenance.max_record_bytes = caps.max_record_bytes;

  a.submission = Author(o);

  testenv::FaultPlan plan;
  if (o.kill_ordinal != 0) {
    plan.At(o.kill_ordinal, testenv::Injection::kKill, 0);
  }
  testenv::TestEnvironment t(plan);
  {
    std::unique_ptr<DB> db;
    if (DB::Open(t.env(), kDir, caps, &db).ok()) {
      std::vector<std::unique_ptr<Snapshot>> snapshots;
      const bool survived = Issue(db.get(), &a.submission, &snapshots, &a.watermark);
      snapshots.clear();
      if (survived) (void)db->Close();
    }
  }

  // THE REOPEN IS A FRESH ENVIRONMENT SEEDED FROM THE DURABLE IMAGE, never the
  // same object: a stale pointer then faults under ASan rather than silently
  // working, which is the same discipline the crash rig uses.
  std::unique_ptr<testenv::TestEnvironment> re =
      testenv::TestEnvironment::FromImage(t.Image(), testenv::FaultPlan());
  std::unique_ptr<DB> reopened;
  const Status opened = DB::Open(re->env(), kDir, caps, &reopened);
  if (opened.ok()) {
    a.recovered = ExtractState(*reopened);
    (void)reopened->Close();
  } else {
    // A FAILED REOPEN IS NOT AN EMPTY RECOVERY, AND THE FIRST VERSION COULD NOT
    // TELL THEM APART. It left `recovered` empty and the judge reported "the
    // engine recovered nothing" -- which is a verdict about the ENGINE for what
    // may be a defect in the RIG, and is HARNESS-006's shape exactly.
    //
    // A reopen that fails is a fact about the run, so it is recorded as one and
    // the judge is told rather than left to infer.
    a.reopen_error = opened.ToString();
  }
  // NO VERDICT. This side cannot reach one, and an artifact that carried a
  // verdict this side invented would be an engine judging itself.
  a.outcome = DiffOutcome::kUnrun;
  return a;
}

}  // namespace rig
}  // namespace rift
