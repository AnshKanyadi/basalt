// The semantics suite, mirroring engine/model's.
//
// This step's REAL acceptance test is B4's differential rig, where "correct"
// means byte-identical to engine/model. The suite here exists to make B4's
// failures debuggable -- to say WHICH rule broke rather than that the two
// engines disagree -- and not to substitute for it.
#include "db.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "call_site.h"
#include "env_guard.h"
#include "regime.h"
#include "reader.h"
#include "test_env.h"

namespace rift {
namespace {

using testenv::TestEnvironment;
using wal::Caps;
using wal::SeqNum;

const std::string kDir = "db";

// The live WAL, found rather than named -- and found HARNESS-SIDE, from the
// ledger, which consumes no ordinal.
//
// B1 could hardcode 000001.log because the WAL was the first file the engine
// ever created. B2 allocates file numbers from the MANIFEST's counter, which
// the manifest draws on first, so the number is an artefact of allocation
// order and a test that hardcodes one rots the next time the order changes.
//
// ASKING THE Env WOULD BE WORSE THAN HARDCODING. A GetChildren consumes an Env
// ordinal, so a probe run that called one would shift every ordinal after it
// relative to the real run that replays the same workload -- and a fault plan
// built from the probe's ordinals would then inject at the wrong call. That is
// how this helper's first version broke a kill-point test.
std::string LiveWalPath(testenv::TestEnvironment* t) {
  std::string best;
  for (const testenv::LedgerEntry& e : t->ledger()) {
    if (e.path.size() < 4) continue;
    if (e.path.compare(e.path.size() - 4, 4, ".log") != 0) continue;
    if (e.path > best) best = e.path;
  }
  EXPECT_FALSE(best.empty()) << "no WAL was ever opened";
  return best;
}

std::string Get(const DB& db, const std::string& key) {
  std::string v;
  const Status s = db.Get(Slice(key), &v);
  return s.ok() ? v : std::string("<absent>");
}

std::vector<std::string> Scan(const DB& db, const IterOptions& o) {
  std::vector<std::string> out;
  std::unique_ptr<Iterator> it = db.NewIter(o);
  for (bool ok = it->First(); ok; ok = it->Next()) {
    out.push_back(it->Key().ToString() + "=" + it->Value().ToString());
  }
  EXPECT_TRUE(it->Close().ok());
  return out;
}

std::vector<std::string> ScanBackward(const DB& db, const IterOptions& o) {
  std::vector<std::string> out;
  std::unique_ptr<Iterator> it = db.NewIter(o);
  for (bool ok = it->Last(); ok; ok = it->Prev()) {
    out.push_back(it->Key().ToString() + "=" + it->Value().ToString());
  }
  EXPECT_TRUE(it->Close().ok());
  return out;
}

struct Fixture {
  TestEnvironment env;
  std::unique_ptr<DB> db;
  Fixture() { EXPECT_TRUE(DB::Open(env.env(), kDir, Caps(), &db).ok()); }
};

SeqNum Put(DB* db, const std::string& k, const std::string& v) {
  WriteBatch b;
  b.Set(Slice(k), Slice(v));
  SeqNum s = 0;
  EXPECT_TRUE(db->Write(b, &s).ok());
  return s;
}

// ------------------------------------------------------------- basics

TEST(DB, SetGetDelete) {
  Fixture f;
  Put(f.db.get(), "a", "1");
  Put(f.db.get(), "b", "2");
  EXPECT_EQ(Get(*f.db, "a"), "1");
  EXPECT_EQ(Get(*f.db, "b"), "2");
  EXPECT_EQ(Get(*f.db, "c"), "<absent>");

  WriteBatch d;
  d.Delete(Slice("a"));
  SeqNum s = 0;
  ASSERT_TRUE(f.db->Write(d, &s).ok());
  EXPECT_EQ(Get(*f.db, "a"), "<absent>");
  EXPECT_EQ(Get(*f.db, "b"), "2");
}

// The sequence advances by one per Write INCLUDING EMPTY ONES, identical to
// engine/model's counter. A space that skipped empty batches would put a
// translation table inside B4's oracle.
TEST(DB, SequenceAdvancesOncePerWriteIncludingEmptyOnes) {
  Fixture f;
  EXPECT_EQ(Put(f.db.get(), "a", "1"), 1u);
  WriteBatch empty;
  SeqNum s = 0;
  ASSERT_TRUE(f.db->Write(empty, &s).ok());
  EXPECT_EQ(s, 2u);
  EXPECT_EQ(Put(f.db.get(), "b", "2"), 3u);
}

TEST(DB, WithinABatchTheLastWriteToAKeyWins) {
  Fixture f;
  WriteBatch b;
  b.Set(Slice("k"), Slice("first"));
  b.Delete(Slice("k"));
  b.Set(Slice("k"), Slice("last"));
  SeqNum s = 0;
  ASSERT_TRUE(f.db->Write(b, &s).ok());
  EXPECT_EQ(Get(*f.db, "k"), "last");
}

TEST(DB, TheEmptyKeyIsAKey) {
  Fixture f;
  Put(f.db.get(), "", "empty");
  EXPECT_EQ(Get(*f.db, ""), "empty");
  EXPECT_EQ(Scan(*f.db, IterOptions()), (std::vector<std::string>{"=empty"}));
}

// ---------------------------------------------------------- iteration

TEST(DB, IterationIsBytewiseAndHalfOpen) {
  Fixture f;
  for (const char* k : {"a", "b", "c", "d", "e"}) Put(f.db.get(), k, k);
  EXPECT_EQ(Scan(*f.db, IterOptions()),
            (std::vector<std::string>{"a=a", "b=b", "c=c", "d=d", "e=e"}));

  IterOptions o;
  o.lower = Bound::At(Slice("b"));
  o.upper = Bound::At(Slice("d"));
  EXPECT_EQ(Scan(*f.db, o), (std::vector<std::string>{"b=b", "c=c"}))
      << "[start, end) -- the upper bound is exclusive";
  EXPECT_EQ(ScanBackward(*f.db, o), (std::vector<std::string>{"c=c", "b=b"}));
}

TEST(DB, IterationSkipsDeletedKeys) {
  Fixture f;
  for (const char* k : {"a", "b", "c"}) Put(f.db.get(), k, k);
  WriteBatch d;
  d.Delete(Slice("b"));
  SeqNum s = 0;
  ASSERT_TRUE(f.db->Write(d, &s).ok());
  EXPECT_EQ(Scan(*f.db, IterOptions()), (std::vector<std::string>{"a=a", "c=c"}));
  EXPECT_EQ(ScanBackward(*f.db, IterOptions()),
            (std::vector<std::string>{"c=c", "a=a"}));
}

TEST(DB, IterationSeesOnlyTheNewestVersionOfEachKey) {
  Fixture f;
  Put(f.db.get(), "k", "one");
  Put(f.db.get(), "k", "two");
  Put(f.db.get(), "k", "three");
  EXPECT_EQ(Scan(*f.db, IterOptions()), (std::vector<std::string>{"k=three"}));
}

TEST(DB, SeekGEAndSeekLT) {
  Fixture f;
  for (const char* k : {"a", "c", "e"}) Put(f.db.get(), k, k);
  std::unique_ptr<Iterator> it = f.db->NewIter(IterOptions());
  EXPECT_TRUE(it->SeekGE(Slice("b")));
  EXPECT_EQ(it->Key().ToString(), "c");
  EXPECT_TRUE(it->SeekGE(Slice("c")));
  EXPECT_EQ(it->Key().ToString(), "c");
  EXPECT_FALSE(it->SeekGE(Slice("z")));
  EXPECT_TRUE(it->SeekLT(Slice("c")));
  EXPECT_EQ(it->Key().ToString(), "a");
  EXPECT_FALSE(it->SeekLT(Slice("a")));
  EXPECT_TRUE(it->Close().ok());
}

// ---------------------------------------------------------- snapshots

TEST(DB, ASnapshotSeesTheStateAsOfWhenItWasTaken) {
  Fixture f;
  Put(f.db.get(), "a", "before");
  std::unique_ptr<Snapshot> snap = f.db->NewSnapshot();
  Put(f.db.get(), "a", "after");
  Put(f.db.get(), "b", "new");

  std::string v;
  ASSERT_TRUE(snap->Get(Slice("a"), &v).ok());
  EXPECT_EQ(v, "before");
  EXPECT_EQ(snap->Get(Slice("b"), &v).code(), Status::Code::kNotFound)
      << "a key written after the snapshot must be invisible through it";
  EXPECT_EQ(Get(*f.db, "a"), "after") << "and the live view must be unaffected";

  std::vector<std::string> out;
  std::unique_ptr<Iterator> it = snap->NewIter(IterOptions());
  for (bool ok = it->First(); ok; ok = it->Next()) {
    out.push_back(it->Key().ToString() + "=" + it->Value().ToString());
  }
  EXPECT_EQ(out, (std::vector<std::string>{"a=before"}));
  EXPECT_TRUE(it->Close().ok());
  EXPECT_TRUE(snap->Close().ok());
}

// -------------------------------------------------------- DeleteRange

TEST(DB, DeleteRangeIsHalfOpen) {
  Fixture f;
  for (const char* k : {"a", "b", "c", "d"}) Put(f.db.get(), k, k);
  WriteBatch b;
  b.DeleteRange(Bound::At(Slice("b")), Bound::At(Slice("d")));
  SeqNum s = 0;
  ASSERT_TRUE(f.db->Write(b, &s).ok());
  EXPECT_EQ(Scan(*f.db, IterOptions()), (std::vector<std::string>{"a=a", "d=d"}));
}

// UNBOUNDED IS NOT THE EMPTY KEY. Go's nil bound and an empty key are different
// things and an empty key is a valid key here, so Bound carries the distinction
// explicitly -- divergence 3 in db.h.
TEST(DB, AnUnboundedRangeIsNotAnEmptyOne) {
  Fixture f;
  for (const char* k : {"", "a", "b"}) Put(f.db.get(), k, "v");
  {
    WriteBatch b;
    b.DeleteRange(Bound::At(Slice("")), Bound::At(Slice("")));
    SeqNum s = 0;
    ASSERT_TRUE(f.db->Write(b, &s).ok());
    EXPECT_EQ(Scan(*f.db, IterOptions()).size(), 3u)
        << "[\"\", \"\") is empty and must delete nothing";
  }
  {
    WriteBatch b;
    b.DeleteRange(Bound::Unbounded(), Bound::Unbounded());
    SeqNum s = 0;
    ASSERT_TRUE(f.db->Write(b, &s).ok());
    EXPECT_TRUE(Scan(*f.db, IterOptions()).empty())
        << "the clear half of snapshot application's clear-then-ingest";
  }
}

// Section 8.1's intra-batch rule: at the DeleteRange the expansion covers the
// current state AND keys written earlier in the same batch, and a Set after it
// re-adds the key.
TEST(DB, DeleteRangeRemovesEarlierWritesInTheSameBatchAndALaterSetReAddsThem) {
  Fixture f;
  Put(f.db.get(), "existing", "old");
  WriteBatch b;
  b.Set(Slice("earlier"), Slice("v"));
  b.DeleteRange(Bound::Unbounded(), Bound::Unbounded());
  b.Set(Slice("later"), Slice("v"));
  SeqNum s = 0;
  ASSERT_TRUE(f.db->Write(b, &s).ok());
  EXPECT_EQ(Get(*f.db, "existing"), "<absent>");
  EXPECT_EQ(Get(*f.db, "earlier"), "<absent>")
      << "a key written earlier in the same batch was not covered";
  EXPECT_EQ(Get(*f.db, "later"), "v")
      << "a Set after the range must re-add the key";
}

// THE WAL RECORDS THE EXPANSION, not the raw DeleteRange -- so recovery replays
// point deletes and never has to expand against a state it is still rebuilding.
TEST(DB, TheWalRecordsTheExpansionAndNotTheRange) {
  TestEnvironment t;
  {
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(t.env(), kDir, Caps(), &db).ok());
    for (const char* k : {"a", "b", "c"}) Put(db.get(), k, "v");
    WriteBatch b;
    b.DeleteRange(Bound::Unbounded(), Bound::Unbounded());
    SeqNum s = 0;
    ASSERT_TRUE(db->Write(b, &s).ok());
    SeqNum mark = 0;
    ASSERT_TRUE(db->Sync(&mark).ok());
  }
  const std::string image = t.ContentNow(LiveWalPath(&t));
  const wal::ScanResult scan = wal::ScanLog(Slice(image));
  ASSERT_EQ(scan.outcome, wal::ScanOutcome::kCleanEnd);
  bool saw_range = false;
  int deletes = 0;
  for (const wal::LogicalRecord& rec : scan.records) {
    if (rec.kind != wal::RecordKind::kBatch) continue;
    wal::DecodedBatch db_rec;
    ASSERT_TRUE(wal::DecodeBatch(Slice(rec.payload), &db_rec));
    for (const wal::Op& op : db_rec.ops) {
      if (op.kind == wal::OpKind::kDeleteRange) saw_range = true;
      if (op.kind == wal::OpKind::kDelete) ++deletes;
    }
  }
  EXPECT_FALSE(saw_range) << "a raw DELETE_RANGE reached the log; recovery would "
                             "have to expand it against a state it is still "
                             "rebuilding, which is correctness by an argument "
                             "with a moving premise";
  EXPECT_EQ(deletes, 3);
}

// ------------------------------------- the assertions this step re-makes

// Re-asserted here because DeleteRange expansion is the operation most likely
// to violate it: it READS the memtable, and a reader that reached the file
// layer would be I/O inside Apply.
TEST(DB, WriteMakesZeroEnvCallsEvenWhenExpandingARange) {
  Fixture f;
  for (int i = 0; i < 500; ++i) {
    const std::string k = "k" + std::to_string(i);
    Put(f.db.get(), k, "v");
  }
  const uint64_t before = EnvCallsOnThisThread();
  WriteBatch b;
  b.DeleteRange(Bound::Unbounded(), Bound::Unbounded());
  SeqNum s = 0;
  ASSERT_TRUE(f.db->Write(b, &s).ok());
  EXPECT_EQ(EnvCallsOnThisThread(), before)
      << "expanding a DeleteRange performed I/O";
}

TEST(DB, ApproximateDiskBytesRespectsItsBounds) {
  Fixture f;
  for (const char* k : {"a", "b", "c"}) Put(f.db.get(), k, "vvvv");
  uint64_t all = 0, some = 0;
  ASSERT_TRUE(f.db->ApproximateDiskBytes(Bound::Unbounded(), Bound::Unbounded(), &all).ok());
  ASSERT_TRUE(f.db->ApproximateDiskBytes(Bound::At(Slice("b")), Bound::Unbounded(), &some).ok());
  EXPECT_GT(all, some);
  EXPECT_GT(some, 0u);
}

// CLOSE IS A WRITE CALL, so its error has to reach the caller.
//
// close(2) reports EIO for writeback that FAILED AFTER THE LAST Sync -- data the
// engine has already reported durable, discovered gone at the only moment
// anyone will ever be told. The first version of this test only asserted that a
// SUCCESSFUL Close returns ok, and mutant BM7 survived it: nothing ever made
// Close fail, so nothing could tell a propagated error from a swallowed one.
TEST(DB, CloseErrorReachesTheCaller) {
  uint64_t close_ordinal = 0;
  {
    TestEnvironment probe;
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(probe.env(), kDir, Caps(), &db).ok());
    Put(db.get(), "a", "1");
    ASSERT_TRUE(db->Close().ok());
    for (const testenv::LedgerEntry& e : probe.ledger()) {
      if (e.site == CallSite::kWritableFileClose) close_ordinal = e.ordinal;
    }
  }
  ASSERT_GT(close_ordinal, 0u);

  testenv::FaultPlan plan;
  plan.At(close_ordinal, testenv::Injection::kIoError);
  TestEnvironment t(plan);
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, Caps(), &db).ok());
  Put(db.get(), "a", "1");
  const Status s = db->Close();
  EXPECT_EQ(s.code(), Status::Code::kIoError)
      << "close(2) failed and the caller was told the database shut down "
         "cleanly. The watermark now stands for bytes that no longer exist";
}

TEST(DB, CloseDoesNotSyncAndReturnsItsError) {
  TestEnvironment t;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, Caps(), &db).ok());
  Put(db.get(), "a", "1");
  ASSERT_TRUE(db->Close().ok());
  EXPECT_EQ(db->DurableSeq(), 0u)
      << "Close synced. The watermark is the engine's only durability promise, "
         "and a Close that synced would make clean shutdown a hidden durability "
         "event engine/model's Close does not have";
}

// ------------------------------------------------------ durability

TEST(DB, ReopensAtExactlyTheLastReturnedSyncsWatermark) {
  TestEnvironment t;
  {
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(t.env(), kDir, Caps(), &db).ok());
    Put(db.get(), "a", "1");
    SeqNum mark = 0;
    ASSERT_TRUE(db->Sync(&mark).ok());
    EXPECT_EQ(mark, 1u);
    Put(db.get(), "b", "2");
  }
  t.Kill();
  std::unique_ptr<TestEnvironment> re =
      TestEnvironment::FromImage(t.Image(), testenv::FaultPlan());
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(re->env(), kDir, Caps(), &db).ok());
  EXPECT_EQ(Get(*db, "a"), "1");
  EXPECT_EQ(Get(*db, "b"), "<absent>");
  EXPECT_EQ(db->DurableSeq(), 0u) << "a fresh WAL has promised nothing yet";
}

// ------------------------------ DeleteRange against the caps and the format
//
// Section 8.2's whole reason for existing: DeleteRange(Unbounded, Unbounded) --
// the clear half of snapshot application's clear-then-ingest, the case
// Amendment A3 was ruled for -- expands to ONE POINT DELETE PER LIVE KEY in a
// single record, and batches are atomic so it cannot be chunked.

// Enough keys that the expansion crosses a 32 KiB block boundary. Each point
// delete costs 1 + 4 + |key| by the frozen formula.
constexpr int kKeysSpanningBlocks = 3000;

// Syncs periodically so the WAL BUFFER cap is not what fires. The lowered-cap
// test below lowers the RECORD cap on purpose, and the ordering invariant means
// the buffer cap moves with it -- so a fill that never drained would trip the
// wrong tripwire and the test would pass for the wrong reason.
void FillKeys(DB* db, int n) {
  for (int i = 0; i < n; ++i) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "key%08d", i);
    const std::string k(buf);
    Put(db, k, "v");
    if ((i + 1) % 500 == 0) {
      SeqNum mark = 0;
      EXPECT_TRUE(db->Sync(&mark).ok());
    }
  }
}

TEST(DBDeleteRange, AnOverCapExpansionIsRefusedAndAppliesNothing) {
  TestEnvironment t;
  // A LOWERED-CAP REGIME. This is precisely why the caps are run-time
  // configurable -- a tripwire nobody has watched fire is decoration -- and
  // precisely why section 8.4 forbids banking this run with default-cap runs.
  Caps small;
  small.max_record_bytes = 20000;
  small.wal_buffer_bytes = 100000;
  ASSERT_TRUE(small.Ordered());
  rig::RunRecord record;
  record.caps = small;
  ASSERT_EQ(record.regime(), rig::Regime::kNonDefault)
      << "this run must be mechanically marked as a different regime, or its "
         "result can be banked with numbers it says nothing about";

  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, small, &db).ok());
  FillKeys(db.get(), kKeysSpanningBlocks);

  WriteBatch b;
  b.DeleteRange(Bound::Unbounded(), Bound::Unbounded());
  SeqNum s = 0;
  const Status st = db->Write(b, &s);
  EXPECT_EQ(st.code(), Status::Code::kRecordTooLarge) << st.ToString();

  // APPLIES NOTHING, ATOMICALLY.
  EXPECT_EQ(Get(*db, "key00000000"), "v")
      << "a refused over-cap DeleteRange partially applied";
  EXPECT_EQ(Get(*db, "key00002999"), "v");
}

// THE MULTI-BLOCK TORN-TAIL CASE, EXERCISED BY A REAL EXPANSION rather than by
// a hand-built fixture. Section 5.4.2's rule is stated for multi-fragment
// records because DeleteRange makes them a ROUTINE path in B1, not an exotic
// one, and a rule exercised only by fixtures is a rule nobody has seen the
// engine actually meet.
TEST(DBDeleteRange, ATornExpansionSpanningBlocksIsDiscardedWholeAndTheGroupStands) {
  uint64_t sync_ordinal = 0;
  uint64_t torn_prefix = 0;
  std::size_t record_bytes = 0;
  {
    TestEnvironment probe;
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(probe.env(), kDir, Caps(), &db).ok());
    FillKeys(db.get(), kKeysSpanningBlocks);
    SeqNum mark = 0;
    ASSERT_TRUE(db->Sync(&mark).ok());
    const std::string wal_path = LiveWalPath(&probe);
    const std::size_t after_fill = probe.ContentNow(wal_path).size();

    WriteBatch b;
    b.DeleteRange(Bound::Unbounded(), Bound::Unbounded());
    SeqNum s = 0;
    ASSERT_TRUE(db->Write(b, &s).ok());
    ASSERT_TRUE(db->Sync(&mark).ok());
    for (const testenv::LedgerEntry& e : probe.ledger()) {
      if (e.site == CallSite::kWritableFileSync) sync_ordinal = e.ordinal;
    }
    record_bytes = probe.ContentNow(wal_path).size() - after_fill;
    // Tear roughly halfway through the expansion, which -- because it spans
    // blocks -- lands inside a MIDDLE fragment.
    torn_prefix = record_bytes / 2;
  }
  ASSERT_GT(record_bytes, wal::kBlockBytes)
      << "the expansion did not span a block, so this test is not exercising "
         "the multi-block case at all";

  testenv::FaultPlan plan;
  plan.At(sync_ordinal, testenv::Injection::kTornSync, torn_prefix);
  TestEnvironment t(plan);
  {
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(t.env(), kDir, Caps(), &db).ok());
    FillKeys(db.get(), kKeysSpanningBlocks);
    SeqNum mark = 0;
    ASSERT_TRUE(db->Sync(&mark).ok());
    WriteBatch b;
    b.DeleteRange(Bound::Unbounded(), Bound::Unbounded());
    SeqNum s = 0;
    ASSERT_TRUE(db->Write(b, &s).ok());
    EXPECT_EQ(db->Sync(&mark).code(), Status::Code::kKilled);
  }
  ASSERT_TRUE(t.dead());
  EXPECT_FALSE(t.exactness_suspended()) << "prefix granularity is the contract "
                                           "model; this run is evidence";

  // The image holds a partially written multi-fragment record. The reader must
  // classify it as a TORN TAIL -- nothing structurally valid follows -- and
  // recovery must discard the whole record, not the part it could parse.
  const std::string image = t.Image().at(LiveWalPath(&t));
  const wal::ScanResult scan = wal::ScanLog(Slice(image));
  EXPECT_EQ(scan.outcome, wal::ScanOutcome::kTornTail) << scan.failure_reason;

  std::unique_ptr<TestEnvironment> re =
      TestEnvironment::FromImage(t.Image(), testenv::FaultPlan());
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(re->env(), kDir, Caps(), &db).ok());
  EXPECT_EQ(Get(*db, "key00000000"), "v")
      << "the torn DeleteRange was partially applied: a multi-fragment record "
         "must be committed whole or not at all";
  EXPECT_EQ(Get(*db, "key00002999"), "v");
}

}  // namespace
}  // namespace rift
