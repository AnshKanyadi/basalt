// The flush, the merged view, and the partition invariant that replaces the
// gapless check.
//
// EVERY TEST HERE SETS caps.flush_bytes LOW, which puts these runs in a
// non-default REGIME by construction (section 8.4) -- runs at non-default caps
// never aggregate with default-cap runs, and that is why the caps are
// configurable at all. A flush threshold that only fires after four megabytes
// of writes is a flush no unit test would ever reach, and an untested flush is
// the one that runs in production first.
#include "db.h"

#include <cstdio>
#include <memory>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "call_site.h"
#include "env_guard.h"
#include "manifest.h"
#include "read_whole_file.h"
#include "recovery.h"
#include "regime.h"
#include "table_check.h"
#include "test_env.h"

namespace rift {
namespace {

using testenv::FaultPlan;
using testenv::Injection;
using testenv::TestEnvironment;
using wal::Caps;
using wal::SeqNum;

const std::string kDir = "db";

// Small enough that a few dozen kilobytes of writes cross it, large enough that
// a single batch does not. Both halves matter: the first makes flushes reachable
// in a test, the second keeps the tests about flushing rather than about the
// batch that happened to trip the threshold.
Caps FlushingCaps() {
  Caps c;
  c.flush_bytes = 32u * 1024;
  return c;
}

std::string Value(int i, std::size_t bytes) {
  std::string v = "v" + std::to_string(i) + ":";
  v.append(bytes, 'x');
  return v;
}

std::string KeyAt(int i) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "key%06d", i);
  return buf;
}

SeqNum Put(DB* db, const std::string& k, const std::string& v) {
  WriteBatch b;
  b.Set(Slice(k), Slice(v));
  SeqNum s = 0;
  EXPECT_TRUE(db->Write(b, &s).ok());
  return s;
}

std::string Get(const DB& db, const std::string& key) {
  std::string v;
  const Status s = db.Get(Slice(key), &v);
  return s.ok() ? v : std::string("<absent>");
}

std::vector<std::string> Scan(const DB& db) {
  std::vector<std::string> out;
  std::unique_ptr<Iterator> it = db.NewIter(IterOptions());
  for (bool ok = it->First(); ok; ok = it->Next()) {
    out.push_back(it->Key().ToString() + "=" + it->Value().ToString());
  }
  EXPECT_TRUE(it->Close().ok());
  return out;
}

std::vector<std::string> ScanBackward(const DB& db) {
  std::vector<std::string> out;
  std::unique_ptr<Iterator> it = db.NewIter(IterOptions());
  for (bool ok = it->Last(); ok; ok = it->Prev()) {
    out.push_back(it->Key().ToString() + "=" + it->Value().ToString());
  }
  EXPECT_TRUE(it->Close().ok());
  return out;
}

std::vector<std::string> Children(TestEnvironment* t, const char* suffix) {
  std::vector<std::string> all;
  EXPECT_TRUE(t->env()->GetChildren(kDir, &all).ok());
  std::vector<std::string> out;
  const std::string s(suffix);
  for (const std::string& c : all) {
    if (c.size() >= s.size() && c.compare(c.size() - s.size(), s.size(), s) == 0) {
      out.push_back(c);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

// Writes until the memtable crosses the threshold, then Syncs -- which is where
// the flush runs and can run nowhere else.
SeqNum FillAndFlush(DB* db, int from, int count, std::size_t value_bytes = 512) {
  for (int i = from; i < from + count; ++i) Put(db, KeyAt(i), Value(i, value_bytes));
  SeqNum watermark = 0;
  EXPECT_TRUE(db->Sync(&watermark).ok());
  return watermark;
}

// ------------------------------------------------------------- the flush

TEST(Flush, TheMemtableIsWrittenOutAndEveryKeyIsStillThere) {
  TestEnvironment t;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  FillAndFlush(db.get(), 0, 200);

  ASSERT_EQ(1u, Children(&t, ".sst").size()) << "the flush wrote no table";
  for (int i = 0; i < 200; ++i) {
    const std::string k = KeyAt(i);
    EXPECT_EQ(Value(i, 512), Get(*db, k)) << "key " << i << " after a flush";
  }
  EXPECT_EQ(200u, Scan(*db).size());
  ASSERT_TRUE(db->Close().ok());
}

TEST(Flush, TheWatermarkNeverGoesBackwards) {
  // ROLLING THE WAL GIVES THE NEW ONE A DURABLE SEQUENCE OF ZERO. A DurableSeq
  // that read only the live WAL would drop to zero at every flush, and the
  // frozen contract says monotone non-decreasing.
  TestEnvironment t;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  SeqNum highest = 0;
  for (int round = 0; round < 4; ++round) {
    FillAndFlush(db.get(), round * 100, 100);
    const SeqNum now = db->DurableSeq();
    EXPECT_GE(now, highest) << "the watermark went backwards at flush " << round;
    highest = now;
  }
  EXPECT_GT(highest, 0u);
  ASSERT_TRUE(db->Close().ok());
}

TEST(Flush, TheRetiredWalIsDeletedAndAFreshOneTakesOver) {
  TestEnvironment t;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  const std::vector<std::string> before = Children(&t, ".log");
  ASSERT_EQ(1u, before.size());
  FillAndFlush(db.get(), 0, 200);
  const std::vector<std::string> after = Children(&t, ".log");
  ASSERT_EQ(1u, after.size()) << "a retired WAL was left behind";
  EXPECT_NE(before[0], after[0]) << "the WAL was not rolled";
  ASSERT_TRUE(db->Close().ok());
}

TEST(Flush, EveryTableItWritesIsOneTheClassifierAccepts) {
  TestEnvironment t;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  for (int round = 0; round < 3; ++round) FillAndFlush(db.get(), round * 100, 100);
  const std::vector<std::string> tables = Children(&t, ".sst");
  ASSERT_EQ(3u, tables.size());
  for (const std::string& name : tables) {
    std::string image;
    ASSERT_TRUE(ReadWholeFile(t.env(), kDir + "/" + name, &image).ok());
    const sst::TableCheck v = sst::ValidateTable(Slice(image));
    EXPECT_TRUE(v.ok()) << name << ": " << sst::TableFaultName(v.fault) << " " << v.why;
  }
  ASSERT_TRUE(db->Close().ok());
}

// -------------------------------------------------------- the merged view

TEST(Flush, ReadsSeeTheMemtableAndTheTablesTogether) {
  TestEnvironment t;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  FillAndFlush(db.get(), 0, 200);          // these are in a table
  for (int i = 1000; i < 1010; ++i) Put(db.get(), KeyAt(i), Value(i, 8));  // memtable

  EXPECT_EQ(Value(5, 512), Get(*db, KeyAt(5)));
  EXPECT_EQ(Value(1005, 8), Get(*db, KeyAt(1005)));
  const std::vector<std::string> forward = Scan(*db);
  EXPECT_EQ(210u, forward.size());

  // AND BACKWARDS, which is where a merge that cannot switch direction fails:
  // stepping back without re-seeking reads whatever the other cursors happen to
  // be sitting on.
  std::vector<std::string> backward = ScanBackward(*db);
  std::reverse(backward.begin(), backward.end());
  EXPECT_EQ(forward, backward);
  ASSERT_TRUE(db->Close().ok());
}

TEST(Flush, ANewerDeletionHidesAValueInATable) {
  // The merge order is not cosmetic. A deletion in the memtable must hide a
  // value in a table, and a table's value must hide an older table's.
  TestEnvironment t;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  FillAndFlush(db.get(), 0, 200);
  WriteBatch d;
  const std::string gone = KeyAt(7);
  d.Delete(Slice(gone));
  SeqNum s = 0;
  ASSERT_TRUE(db->Write(d, &s).ok());
  EXPECT_EQ("<absent>", Get(*db, gone));
  EXPECT_EQ(199u, Scan(*db).size());

  // And once the deletion itself is flushed, into a NEWER table than the value.
  FillAndFlush(db.get(), 500, 200);
  EXPECT_EQ("<absent>", Get(*db, gone));
  ASSERT_TRUE(db->Close().ok());
}

TEST(Flush, AnOverwriteInANewerTableWinsOverAnOlderOne) {
  TestEnvironment t;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  FillAndFlush(db.get(), 0, 200);
  Put(db.get(), KeyAt(3), "rewritten");
  FillAndFlush(db.get(), 500, 200);
  EXPECT_EQ("rewritten", Get(*db, KeyAt(3)));
  ASSERT_TRUE(db->Close().ok());
}

TEST(Flush, AnIteratorSurvivesTheFlushThatRetiresItsMemtable) {
  // memtable.h's expiring note, discharged. In B1 an iterator held a raw
  // pointer into an arena that was never freed. A flush retires a memtable, so
  // an iterator that outlived it would read freed memory -- and under ASan this
  // test is what says otherwise.
  TestEnvironment t;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  for (int i = 0; i < 200; ++i) Put(db.get(), KeyAt(i), Value(i, 512));

  std::unique_ptr<Iterator> it = db->NewIter(IterOptions());
  ASSERT_TRUE(it->First());
  const std::string first = it->Key().ToString();

  SeqNum watermark = 0;
  ASSERT_TRUE(db->Sync(&watermark).ok());  // flushes, retiring the memtable
  ASSERT_EQ(1u, Children(&t, ".sst").size());

  std::size_t seen = 1;
  while (it->Next()) ++seen;
  EXPECT_EQ(KeyAt(0), first);
  EXPECT_EQ(200u, seen);
  ASSERT_TRUE(it->Close().ok());
  ASSERT_TRUE(db->Close().ok());
}

TEST(Flush, ASnapshotSurvivesTheFlushThatRetiresItsMemtable) {
  TestEnvironment t;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  for (int i = 0; i < 200; ++i) Put(db.get(), KeyAt(i), Value(i, 512));
  std::unique_ptr<Snapshot> snap = db->NewSnapshot();

  SeqNum watermark = 0;
  ASSERT_TRUE(db->Sync(&watermark).ok());
  Put(db.get(), KeyAt(5), "after the snapshot");

  std::string v;
  const std::string k = KeyAt(5);
  ASSERT_TRUE(snap->Get(Slice(k), &v).ok());
  EXPECT_EQ(Value(5, 512), v) << "the snapshot saw a write taken after it";
  ASSERT_TRUE(snap->Close().ok());
  ASSERT_TRUE(db->Close().ok());
}

// ------------------------------------------------- DeleteRange, merged view

TEST(Flush, DeleteRangeReachesKeysThatHaveBeenFlushed) {
  // B2-D7 section 8: at B1 the expansion read the memtable. Reading only the
  // memtable now would make a DeleteRange silently miss every key already in a
  // table -- and "silently" is the word that matters, because the batch would
  // succeed and the keys would still be there.
  TestEnvironment t;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  FillAndFlush(db.get(), 0, 200);
  ASSERT_EQ(1u, Children(&t, ".sst").size());

  WriteBatch b;
  const std::string from = KeyAt(0);
  const std::string to = KeyAt(100);
  b.DeleteRange(Bound::At(Slice(from)), Bound::At(Slice(to)));
  SeqNum s = 0;
  ASSERT_TRUE(db->Write(b, &s).ok());
  for (int i = 0; i < 100; ++i) {
    const std::string k = KeyAt(i);
    EXPECT_EQ("<absent>", Get(*db, k)) << "key " << i << " survived a DeleteRange";
  }
  EXPECT_EQ(Value(100, 512), Get(*db, KeyAt(100)));
  EXPECT_EQ(100u, Scan(*db).size());
  ASSERT_TRUE(db->Close().ok());
}

TEST(Flush, ApplyMakesNoEnvCallAndNoLongerNeedsToReadAnything) {
  // THE ASSERTION SURVIVES ITS REASON, AND THE REASON IS WHAT CHANGED.
  //
  // B2-D7 called this a real constraint rather than a detail: the expansion
  // happened at Apply, Apply makes no Env call, and the expansion had to read
  // every live SSTable -- which was only possible because they were RESIDENT.
  // That is what forced `table.h`'s whole-file residency.
  //
  // B3.5 retires the expansion. Apply now reads NOTHING: a range deletion is
  // one entry whose meaning does not depend on the state around it. So the
  // Env-call assertion stands unchanged and stands for less -- it no longer
  // depends on residency, and residency no longer depends on it.
  //
  // Kept rather than deleted, because "Write never blocks on I/O" is a frozen
  // promise (db.h) independent of how DeleteRange is implemented, and this is
  // the assertion that holds the engine to it on the path most likely to break
  // it. Asserted from the harness's own Env-call counter, which asks the engine
  // nothing.
  TestEnvironment t;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  FillAndFlush(db.get(), 0, 200);

  const uint64_t before = t.ordinal();
  WriteBatch b;
  b.DeleteRange(Bound::Unbounded(), Bound::Unbounded());
  SeqNum s = 0;
  ASSERT_TRUE(db->Write(b, &s).ok());
  EXPECT_EQ(before, t.ordinal()) << "Write made an Env call";
  EXPECT_EQ(0u, Scan(*db).size());
  ASSERT_TRUE(db->Close().ok());
}


TEST(Flush, AnOrphanTableIsRemovedAtTheNextOpen) {
  // A table is created, synced, dirsynced and only THEN named, so an unnamed
  // .sst is one a crash caught before its manifest edit. Nothing refers to it
  // and nothing can, so leaving it would be a leak that grows with every crash.
  TestEnvironment t;
  {
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
    FillAndFlush(db.get(), 0, 200);
    ASSERT_TRUE(db->Close().ok());
  }
  ASSERT_EQ(1u, Children(&t, ".sst").size());
  // A table nobody named, at a number the manifest does not know.
  {
    WritableFilePtr f;
    ASSERT_TRUE(t.env()->NewWritableFile(kDir + "/000099.sst", &f).ok());
    const std::string junk(64, 'z');
    ASSERT_TRUE(f->Append(Slice(junk)).ok());
    ASSERT_TRUE(f->Sync().ok());
    ASSERT_TRUE(f->Close().ok());
  }
  ASSERT_EQ(2u, Children(&t, ".sst").size());
  {
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
    ASSERT_TRUE(db->Close().ok());
  }
  const std::vector<std::string> left = Children(&t, ".sst");
  ASSERT_EQ(1u, left.size()) << "the orphan survived an Open";
  EXPECT_NE("000099.sst", left[0]);
}

// ------------------------------------------------- B2-D8: recovery equivalence

// THE OBVIOUS WAY TO ASSERT THIS IS WRONG, and B2-D8 says so before the code
// exists. Running both paths and comparing them is AGREEMENT BETWEEN TWO PATHS,
// and section 13.4b is exactly that: agreement is not either path being right,
// and two paths that share an assumption agree most confidently where they are
// both wrong. These two share the memtable, the comparator and the internal key
// encoding, so they would agree for reasons unrelated to either being correct.
//
// So each path is compared against THE HARNESS'S OWN REFERENCE first -- a map
// the test builds as it writes, which asks the engine nothing -- and only then
// are the two compared with each other. THEIR AGREEMENT IS A THIRD CHECK, NOT
// THE CHECK.
//
// This is section 13.4b arriving in C++ before it could cost anything. Track A
// paid for that lesson; it was inherited as a design constraint rather than as
// a postmortem.
struct EquivalenceRun {
  std::map<std::string, std::string> recovered;
  SeqNum watermark = 0;
  std::size_t tables = 0;
};

// Runs one fixed workload under the given caps, kills, reopens, and reports what
// recovery produced. The workload is IDENTICAL in both regimes; only the flush
// threshold differs, which is the whole point.
EquivalenceRun RunOneWorkload(uint64_t flush_bytes,
                              std::map<std::string, std::string>* reference,
                              SeqNum* reference_watermark) {
  Caps caps;
  caps.flush_bytes = flush_bytes;
  TestEnvironment t;
  EquivalenceRun out;
  {
    std::unique_ptr<DB> db;
    EXPECT_TRUE(DB::Open(t.env(), kDir, caps, &db).ok());
    SeqNum mark = 0;
    for (int round = 0; round < 5; ++round) {
      for (int i = 0; i < 60; ++i) {
        const std::string k = KeyAt(round * 37 + i);
        const std::string v = Value(round * 1000 + i, 256);
        Put(db.get(), k, v);
        if (reference != nullptr) (*reference)[k] = v;
      }
      // A delete and a range delete each round, so the two paths have to agree
      // about tombstones and not merely about values.
      const std::string gone = KeyAt(round * 37 + 3);
      WriteBatch d;
      d.Delete(Slice(gone));
      SeqNum s = 0;
      EXPECT_TRUE(db->Write(d, &s).ok());
      if (reference != nullptr) reference->erase(gone);

      EXPECT_TRUE(db->Sync(&mark).ok());
      if (reference_watermark != nullptr) *reference_watermark = mark;
    }
    out.watermark = db->DurableSeq();
  }
  t.Kill();

  std::unique_ptr<TestEnvironment> re =
      TestEnvironment::FromImage(t.Image(), FaultPlan());
  std::unique_ptr<DB> db;
  EXPECT_TRUE(DB::Open(re->env(), kDir, caps, &db).ok());
  std::unique_ptr<Iterator> it = db->NewIter(IterOptions());
  for (bool ok = it->First(); ok; ok = it->Next()) {
    out.recovered[it->Key().ToString()] = it->Value().ToString();
  }
  EXPECT_TRUE(it->Close().ok());
  out.tables = Children(re.get(), ".sst").size();
  EXPECT_TRUE(db->Close().ok());
  return out;
}

TEST(Flush, RecoveryFromWalPlusTablesEqualsRecoveryFromWalAlone) {
  std::map<std::string, std::string> reference;
  SeqNum reference_watermark = 0;
  // WAL ALONE: a threshold this workload cannot reach, so no flush ever runs.
  const EquivalenceRun wal_only =
      RunOneWorkload(64u << 20, &reference, &reference_watermark);
  // WAL PLUS TABLES: the same writes, the same syncs, a threshold it crosses
  // repeatedly.
  const EquivalenceRun with_tables = RunOneWorkload(16u * 1024, nullptr, nullptr);

  // GF-1: A LANE VERIFYING AN EQUIVALENCE MUST RUN WHERE THE TWO SIDES DIFFER.
  // Without these the test compares two runs that took the same path and calls
  // the result equivalence.
  ASSERT_EQ(0u, wal_only.tables) << "the WAL-only path flushed after all";
  ASSERT_GT(with_tables.tables, 1u) << "the table path never flushed";

  // (1) EACH PATH AGAINST THE HARNESS'S OWN REFERENCE. This is the check.
  EXPECT_EQ(reference, wal_only.recovered)
      << "recovery from the WAL alone does not match what the harness wrote";
  EXPECT_EQ(reference, with_tables.recovered)
      << "recovery from the WAL plus SSTables does not match what the harness "
         "wrote";

  // (2) AT THE SAME WATERMARK. Comparing states recovered at different
  // watermarks would be comparing two different questions.
  EXPECT_EQ(reference_watermark, wal_only.watermark);
  EXPECT_EQ(reference_watermark, with_tables.watermark);

  // (3) AND THE TWO AGAINST EACH OTHER -- a THIRD check, not the check. It can
  // only fail if one of the two above already has, and it is here because
  // B2-D8's exit criterion is stated as an equivalence and the artifact should
  // say the sentence it claims.
  EXPECT_EQ(wal_only.recovered, with_tables.recovered);
}

}  // namespace
}  // namespace rift
