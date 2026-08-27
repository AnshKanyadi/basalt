// COMPACTION THROUGH THE WHOLE ENGINE: the trigger, the install ordering, the
// two levels, and the promises a compaction must not break.
//
// The unit-level drop rules live in compaction_test.cc, judged by the two
// instruments B3-D7a names. What is here is everything those cannot see: that
// the right files are chosen, that the manifest and the directory move in the
// order B2-D5 fixed, and that a restart afterwards finds what it was promised.
//
// EVERY TEST HERE SETS caps.flush_bytes LOW, which puts these runs in a
// non-default REGIME by construction (section 8.4).
#include "db.h"

#include <cstdio>
#include <memory>
#include <algorithm>
#include <atomic>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "manifest.h"
#include "memtable.h"
#include "manifest_image.h"
#include "table_check.h"
#include "single_caller.h"
#include "test_env.h"

namespace rift {
namespace {

using testenv::TestEnvironment;
using wal::Caps;
using wal::SeqNum;

const std::string kDir = "db";

Caps FlushingCaps() {
  Caps c;
  c.flush_bytes = 32u * 1024;
  return c;
}

std::string KeyAt(int i) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "key%06d", i);
  return buf;
}

std::string Value(int i, std::size_t bytes) {
  std::string v = "v" + std::to_string(i) + ":";
  v.append(bytes, 'x');
  return v;
}

SeqNum Put(DB* db, const std::string& k, const std::string& v) {
  WriteBatch b;
  b.Set(Slice(k), Slice(v));
  SeqNum s = 0;
  EXPECT_TRUE(db->Write(b, &s).ok());
  return s;
}

SeqNum Del(DB* db, const std::string& k) {
  WriteBatch b;
  b.Delete(Slice(k));
  SeqNum s = 0;
  EXPECT_TRUE(db->Write(b, &s).ok());
  return s;
}

std::string Get(const DB& db, const std::string& key) {
  std::string v;
  const Status s = db.Get(Slice(key), &v);
  return s.ok() ? v : std::string("<absent>");
}

std::vector<std::string> Tables(TestEnvironment* t) {
  std::vector<std::string> all;
  EXPECT_TRUE(t->env()->GetChildren(kDir, &all).ok());
  std::vector<std::string> out;
  for (const std::string& c : all) {
    if (c.size() > 4 && c.compare(c.size() - 4, 4, ".sst") == 0) out.push_back(c);
  }
  std::sort(out.begin(), out.end());
  return out;
}

// How many tables the manifest names at each level.
//
// PARSED, NEVER OPENED. `Manifest::Open` rotates, deletes the manifest it
// replaced and installs a new CURRENT -- so the first version of this helper
// destroyed the live manifest underneath the running engine, and the next
// append failed on a vanished file. See rig/manifest_image.h: the
// artifact/belief split as a rule about what an OBSERVATION may cost.
std::pair<int, int> LevelCounts(TestEnvironment* t) {
  std::string current = t->ContentNow(sst::CurrentPath(kDir));
  while (!current.empty() && current.back() == '\n') current.pop_back();
  const std::string image = t->ContentNow(kDir + "/" + current);
  sst::ManifestState state;
  std::string why;
  EXPECT_TRUE(rig::ReplayManifestImage(Slice(image), &state, &why)) << why;
  int l0 = 0;
  int l1 = 0;
  for (const auto& e : state.tables) (e.second.level == 0 ? l0 : l1)++;
  return {l0, l1};
}

// `Slice(KeyAt(i))` will not compile: the deleted `Slice(std::string&&)` stops a
// Slice binding to a temporary, which is HARNESS-007's fix doing its job. The
// string is bound to a local here and `Bound::At` copies it.
Bound BoundAt(int i) {
  const std::string k = KeyAt(i);
  return Bound::At(Slice(k));
}

SeqNum FillAndFlush(DB* db, int from, int count) {
  for (int i = from; i < from + count; ++i) Put(db, KeyAt(i), Value(i, 512));
  SeqNum watermark = 0;
  const Status s = db->Sync(&watermark);
  EXPECT_TRUE(s.ok()) << s.ToString();
  return watermark;
}

// ---------------------------------------------------------------- the trigger

TEST(Compact, FourFlushesCollapseIntoALevelOneRun) {
  TestEnvironment t;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  for (int round = 0; round < 3; ++round) {
    FillAndFlush(db.get(), round * 50, 50);
    EXPECT_EQ(round + 1, static_cast<int>(Tables(&t).size()))
        << "a flush should add an L0 file and nothing should compact yet";
  }
  FillAndFlush(db.get(), 150, 50);
  // The fourth flush crosses the trigger, and the compaction runs in the same
  // Sync -- so L0 is empty and the four inputs are gone from the directory.
  const std::pair<int, int> levels = LevelCounts(&t);
  EXPECT_EQ(0, levels.first) << "L0 should be empty after a compaction";
  EXPECT_GT(levels.second, 1) << "an output capped at the flush threshold "
                                 "should produce a RUN, not one file -- a run "
                                 "of one is candidate (a) wearing (b)'s name";
  EXPECT_EQ(static_cast<std::size_t>(levels.second), Tables(&t).size())
      << "every file on disk should be one the manifest names";
  for (int i = 0; i < 200; ++i) {
    EXPECT_EQ(Value(i, 512), Get(*db, KeyAt(i))) << "key " << i;
  }
  ASSERT_TRUE(db->Close().ok());
}

TEST(Compact, ADeletionWrittenBeforeItStaysDeletedAfterIt) {
  TestEnvironment t;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  FillAndFlush(db.get(), 0, 50);
  Del(db.get(), KeyAt(10));
  for (int round = 1; round < 4; ++round) FillAndFlush(db.get(), round * 50, 50);
  EXPECT_EQ(0, LevelCounts(&t).first);
  EXPECT_EQ("<absent>", Get(*db, KeyAt(10)));
  EXPECT_EQ(Value(11, 512), Get(*db, KeyAt(11)));
  ASSERT_TRUE(db->Close().ok());
}

// ------------------------------------------------------------- the two levels

TEST(Compact, AReadCrossesLevelZeroAndLevelOne) {
  TestEnvironment t;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  for (int round = 0; round < 4; ++round) FillAndFlush(db.get(), round * 50, 50);
  const std::pair<int, int> after = LevelCounts(&t);
  ASSERT_EQ(0, after.first);
  ASSERT_GT(after.second, 1) << "the read below is only a two-level read if L1 "
                                "is a run the binary search has to search";

  // Now one more flush: an L0 file over an L1 run. A deletion in L0 must hide
  // the value L1 still holds, and an untouched key must read through.
  Del(db.get(), KeyAt(20));
  Put(db.get(), KeyAt(21), "rewritten");
  FillAndFlush(db.get(), 200, 50);
  ASSERT_EQ(1, LevelCounts(&t).first);
  EXPECT_EQ("<absent>", Get(*db, KeyAt(20)));
  EXPECT_EQ("rewritten", Get(*db, KeyAt(21)));
  EXPECT_EQ(Value(22, 512), Get(*db, KeyAt(22)));
  EXPECT_EQ(Value(200, 512), Get(*db, KeyAt(200)));
  ASSERT_TRUE(db->Close().ok());
}

// INPUT SELECTION IS THE REASON L1 IS A RUN. A compaction reads only the L1
// files its inputs overlap; the rest keep their file numbers and are never
// rewritten. That is the whole of what candidate (b) buys over (a), and if it
// were not true the write amplification B3-D3 rejected (a) for would be back.
TEST(Compact, ASecondCompactionRewritesOnlyTheOverlappingPartOfTheRun) {
  TestEnvironment t;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  for (int round = 0; round < 4; ++round) FillAndFlush(db.get(), round * 50, 50);
  const std::vector<std::string> first_run = Tables(&t);
  ASSERT_GT(first_run.size(), 1u);

  // Four flushes touching ONLY the low end of the key space. The keys must stay
  // inside the existing range: a fixture that added keys ABOVE it would give L0
  // a span covering the whole run, every L1 file would overlap, and the test
  // would be measuring its own workload rather than input selection.
  //
  // Enough bytes per round that every round certainly crosses the flush
  // threshold -- a round that did not flush would leave L0 short of the trigger
  // and this test would be asserting about a compaction that never ran.
  for (int round = 0; round < 4; ++round) {
    for (int i = 0; i < 20; ++i) Put(db.get(), KeyAt(i), Value(i, 2048));
    SeqNum w = 0;
    ASSERT_TRUE(db->Sync(&w).ok());
  }
  ASSERT_EQ(0, LevelCounts(&t).first);

  const std::vector<std::string> second_run = Tables(&t);
  int survivors = 0;
  for (const std::string& name : first_run) {
    for (const std::string& now : second_run) {
      if (name == now) ++survivors;
    }
  }
  EXPECT_GT(survivors, 0)
      << "every L1 file was rewritten, so this is candidate (a): the run bought "
         "nothing and a compaction still costs the whole database";
  for (int i = 0; i < 20; ++i) EXPECT_EQ(Value(i, 2048), Get(*db, KeyAt(i)));
  for (int i = 20; i < 200; ++i) EXPECT_EQ(Value(i, 512), Get(*db, KeyAt(i)));
  ASSERT_TRUE(db->Close().ok());
}

// A KEY'S VERSIONS MAY NOT SPLIT ACROSS TWO FILES OF THE RUN.
//
// The output rolls at the flush threshold, and it may roll ONLY at a user key
// boundary. Reaching that case needs a key with MANY surviving versions, which
// needs live snapshots -- without them the drop rule leaves one version per key
// and no key is ever large enough to span a roll. So the snapshots here are not
// decoration: they are what makes the situation exist at all.
TEST(Compact, AKeysVersionsAreNeverSplitAcrossTwoFilesOfTheRun) {
  TestEnvironment t;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  std::vector<std::unique_ptr<Snapshot>> held;
  std::vector<std::string> expected;
  for (int round = 0; round < 4; ++round) {
    for (int rep = 0; rep < 10; ++rep) {
      for (int i = 0; i < 2; ++i) {
        Put(db.get(), KeyAt(i), Value(round * 10 + rep, 2048));
      }
      // Every version stays observable, so the compaction must carry all forty
      // versions of each key into the run.
      held.push_back(db->NewSnapshot());
      expected.push_back(Value(round * 10 + rep, 2048));
    }
    SeqNum w = 0;
    ASSERT_TRUE(db->Sync(&w).ok());
  }
  ASSERT_EQ(0, LevelCounts(&t).first);
  ASSERT_GT(LevelCounts(&t).second, 1)
      << "forty 2 KiB versions of two keys should not fit in one output file";

  // The run check at Open is what refuses a split key, so the reopen IS the
  // assertion -- and the versions must all still be readable through it.
  for (std::size_t i = 0; i < held.size(); ++i) {
    std::string v;
    const std::string k0 = KeyAt(0);
    ASSERT_TRUE(held[i]->Get(Slice(k0), &v).ok()) << "snapshot " << i;
    EXPECT_EQ(expected[i], v) << "snapshot " << i << " lost its version";
    ASSERT_TRUE(held[i]->Close().ok());
  }
  ASSERT_TRUE(db->Close().ok());
  std::unique_ptr<DB> reopened;
  const Status s = DB::Open(t.env(), kDir, FlushingCaps(), &reopened);
  ASSERT_TRUE(s.ok()) << s.ToString();
  EXPECT_EQ(expected.back(), Get(*reopened, KeyAt(0)));
  ASSERT_TRUE(reopened->Close().ok());
}

// ------------------------------------------------- what a compaction may not break

// THE PROMISE THE DROP CLAIM DOES NOT MENTION. `Open` re-derives the durable
// floor as the maximum largest_seq over the live tables, because the manifest
// may not record a durable sequence (D7). A compaction free to drop the
// highest-sequenced entry would lower that maximum and DurableSeq would go
// backwards across a restart.
TEST(Compact, TheWatermarkDoesNotGoBackwardsAcrossARestart) {
  TestEnvironment t;
  SeqNum before = 0;
  {
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
    for (int round = 0; round < 3; ++round) FillAndFlush(db.get(), round * 50, 50);
    // The last thing written before the compacting flush is a DELETION of a key
    // nothing older survives for -- exactly the entry the claim permits
    // dropping, and exactly the one carrying the top sequence.
    for (int i = 150; i < 200; ++i) Put(db.get(), KeyAt(i), Value(i, 512));
    Del(db.get(), "zzz-never-written");
    SeqNum watermark = 0;
    ASSERT_TRUE(db->Sync(&watermark).ok());
    ASSERT_EQ(0, LevelCounts(&t).first);
    before = db->DurableSeq();
    ASSERT_TRUE(db->Close().ok());
  }
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  EXPECT_GE(db->DurableSeq(), before)
      << "a compaction dropped the entry the durable floor is derived from";
  ASSERT_TRUE(db->Close().ok());
}

TEST(Compact, EverythingIsStillThereAfterAReopen) {
  TestEnvironment t;
  {
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
    for (int round = 0; round < 4; ++round) FillAndFlush(db.get(), round * 50, 50);
    ASSERT_EQ(0, LevelCounts(&t).first);
    ASSERT_TRUE(db->Close().ok());
  }
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  for (int i = 0; i < 200; ++i) {
    EXPECT_EQ(Value(i, 512), Get(*db, KeyAt(i))) << "key " << i << " after reopen";
  }
  // The inputs are gone and nothing was left behind: an orphan .sst is removed
  // at Open, so what is left on disk must be exactly what the manifest names.
  EXPECT_EQ(static_cast<std::size_t>(LevelCounts(&t).second), Tables(&t).size());
  ASSERT_TRUE(db->Close().ok());
}

// A SNAPSHOT PINS ITS STORES, and it also enters `S` -- so the version it can
// see is required rather than merely reachable. Both halves are asserted:
// the snapshot reads its own value, and the live DB reads the new one.
TEST(Compact, ASnapshotTakenBeforeACompactionStillReadsItsVersion) {
  TestEnvironment t;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  FillAndFlush(db.get(), 0, 50);
  std::unique_ptr<Snapshot> snap = db->NewSnapshot();
  Put(db.get(), KeyAt(7), "after-the-snapshot");
  for (int round = 1; round < 4; ++round) FillAndFlush(db.get(), round * 50, 50);
  ASSERT_EQ(0, LevelCounts(&t).first);

  std::string v;
  const std::string k7 = KeyAt(7);
  ASSERT_TRUE(snap->Get(Slice(k7), &v).ok());
  EXPECT_EQ(Value(7, 512), v) << "the snapshot's version did not survive";
  EXPECT_EQ("after-the-snapshot", Get(*db, KeyAt(7)));
  ASSERT_TRUE(snap->Close().ok());
  ASSERT_TRUE(db->Close().ok());
}

// -------------------------------------------------- range deletes, end to end
//
// B3.5c-d: THE TOMBSTONE THAT HIDES A KEY NEED NOT LIVE WHERE THE KEY DOES.
// Every test here puts the two in different stores on purpose, because a read
// path that asked only the store holding the value would pass a test that put
// them together.

TEST(RangeDelete, ARangeInTheMemtableHidesAValueInATable) {
  TestEnvironment t;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  FillAndFlush(db.get(), 0, 50);          // the values are now in a table
  ASSERT_EQ(1u, Tables(&t).size());

  WriteBatch b;                            // the tombstone stays in the memtable
  b.DeleteRange(BoundAt(10), BoundAt(20));
  SeqNum s = 0;
  ASSERT_TRUE(db->Write(b, &s).ok());

  EXPECT_EQ(Value(9, 512), Get(*db, KeyAt(9))) << "below the range";
  EXPECT_EQ("<absent>", Get(*db, KeyAt(10))) << "the start is INCLUSIVE";
  EXPECT_EQ("<absent>", Get(*db, KeyAt(15)));
  EXPECT_EQ(Value(20, 512), Get(*db, KeyAt(20))) << "the end is EXCLUSIVE";
  ASSERT_TRUE(db->Close().ok());
}

// AND IT SURVIVES THE FLUSH THAT MOVES IT INTO A TABLE, which is the step the
// memtable's own answer cannot cover.
TEST(RangeDelete, ARangeSurvivesTheFlushThatWritesItIntoATable) {
  TestEnvironment t;
  {
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
    FillAndFlush(db.get(), 0, 50);
    WriteBatch b;
    b.DeleteRange(BoundAt(10), BoundAt(20));
    SeqNum s = 0;
    ASSERT_TRUE(db->Write(b, &s).ok());
    FillAndFlush(db.get(), 100, 50);   // flushes the tombstone into a table
    ASSERT_EQ(2u, Tables(&t).size());
    EXPECT_EQ("<absent>", Get(*db, KeyAt(15)));
    ASSERT_TRUE(db->Close().ok());
  }
  // AND ACROSS A REOPEN, which is the WAL replay path: recovery INSERTS the
  // tombstone and computes nothing.
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  EXPECT_EQ("<absent>", Get(*db, KeyAt(15)));
  EXPECT_EQ(Value(9, 512), Get(*db, KeyAt(9)));
  EXPECT_EQ(Value(20, 512), Get(*db, KeyAt(20)));
  ASSERT_TRUE(db->Close().ok());
}

// A WRITE ABOVE THE TOMBSTONE IS NOT HIDDEN BY IT. Strictly-newer-hides, and
// this is the half that a `>=` comparison would break.
TEST(RangeDelete, AValueWrittenAfterTheRangeSurvivesIt) {
  TestEnvironment t;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  FillAndFlush(db.get(), 0, 50);
  WriteBatch b;
  b.DeleteRange(BoundAt(10), BoundAt(20));
  SeqNum s = 0;
  ASSERT_TRUE(db->Write(b, &s).ok());
  Put(db.get(), KeyAt(15), "written-after");
  EXPECT_EQ("written-after", Get(*db, KeyAt(15)));
  ASSERT_TRUE(db->Close().ok());
}

// THE MODEL'S INTRA-BATCH RULE: a DeleteRange covers keys written EARLIER in
// the same batch, and a Set AFTER it re-adds the key. Every op in one batch
// shares a sequence, so this is the case the tombstone alone cannot express --
// it is resolved in the batch, and this is what says so.
TEST(RangeDelete, WithinOneBatchASetAfterTheRangeSurvivesAndOneBeforeDoesNot) {
  TestEnvironment t;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  WriteBatch b;
  const std::string lo = "a";
  const std::string hi = "z";
  const std::string before = "before";
  const std::string after = "after";
  b.Set(Slice(before), Slice("1"));
  b.DeleteRange(Bound::At(Slice(lo)), Bound::At(Slice(hi)));
  b.Set(Slice(after), Slice("2"));
  SeqNum s = 0;
  ASSERT_TRUE(db->Write(b, &s).ok());
  EXPECT_EQ("<absent>", Get(*db, "before"));
  EXPECT_EQ("2", Get(*db, "after"));
  ASSERT_TRUE(db->Close().ok());
}

// AN ITERATOR SEES WHAT A POINT READ SEES. The two walks are separate code and
// a range delete that only one of them honoured would be invisible to the
// other's tests.
TEST(RangeDelete, AnIteratorSkipsWhatARangeDeleteHides) {
  TestEnvironment t;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  FillAndFlush(db.get(), 0, 50);
  WriteBatch b;
  b.DeleteRange(BoundAt(10), BoundAt(20));
  SeqNum s = 0;
  ASSERT_TRUE(db->Write(b, &s).ok());

  std::vector<std::string> forward;
  std::unique_ptr<Iterator> it = db->NewIter(IterOptions());
  for (bool ok = it->First(); ok; ok = it->Next()) forward.push_back(it->Key().ToString());
  ASSERT_TRUE(it->Close().ok());
  EXPECT_EQ(40u, forward.size()) << "ten keys are covered";
  for (int i = 10; i < 20; ++i) {
    EXPECT_EQ(forward.end(), std::find(forward.begin(), forward.end(), KeyAt(i)));
  }
  // AND BACKWARDS, which is a different loop with its own visibility walk.
  std::vector<std::string> backward;
  std::unique_ptr<Iterator> rit = db->NewIter(IterOptions());
  for (bool ok = rit->Last(); ok; ok = rit->Prev()) backward.push_back(rit->Key().ToString());
  ASSERT_TRUE(rit->Close().ok());
  EXPECT_EQ(40u, backward.size());
  ASSERT_TRUE(db->Close().ok());
}

// A SNAPSHOT BELOW THE RANGE STILL SEES WHAT IT HID -- the same shape as
// ASnapshotBelowATombstoneKeepsIt one level up, and the reason a range delete
// cannot simply be applied to the state when it arrives.
TEST(RangeDelete, ASnapshotBelowARangeStillSeesTheValue) {
  TestEnvironment t;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  FillAndFlush(db.get(), 0, 50);
  std::unique_ptr<Snapshot> snap = db->NewSnapshot();
  WriteBatch b;
  b.DeleteRange(BoundAt(10), BoundAt(20));
  SeqNum s = 0;
  ASSERT_TRUE(db->Write(b, &s).ok());

  std::string v;
  const std::string k = KeyAt(15);
  ASSERT_TRUE(snap->Get(Slice(k), &v).ok()) << "the snapshot predates the range";
  EXPECT_EQ(Value(15, 512), v);
  EXPECT_EQ("<absent>", Get(*db, KeyAt(15)));
  ASSERT_TRUE(snap->Close().ok());
  ASSERT_TRUE(db->Close().ok());
}

// `[A3]`'s CLEAR-EVERYTHING CASE, END TO END: one unbounded tombstone, through
// the memtable, the flush, the table and a reopen.
TEST(RangeDelete, AnUnboundedClearEverythingSurvivesAFlushAndAReopen) {
  TestEnvironment t;
  {
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
    FillAndFlush(db.get(), 0, 50);
    WriteBatch b;
    b.DeleteRange(Bound::Unbounded(), Bound::Unbounded());
    SeqNum s = 0;
    ASSERT_TRUE(db->Write(b, &s).ok());
    EXPECT_EQ("<absent>", Get(*db, KeyAt(0)));
    EXPECT_EQ("<absent>", Get(*db, KeyAt(49)));
    FillAndFlush(db.get(), 100, 50);   // writes the tombstone into a table
    EXPECT_EQ("<absent>", Get(*db, KeyAt(0)));
    EXPECT_EQ(Value(100, 512), Get(*db, KeyAt(100))) << "written after the clear";
    ASSERT_TRUE(db->Close().ok());
  }
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  EXPECT_EQ("<absent>", Get(*db, KeyAt(0)));
  EXPECT_EQ("<absent>", Get(*db, KeyAt(49)));
  EXPECT_EQ(Value(100, 512), Get(*db, KeyAt(100)));
  ASSERT_TRUE(db->Close().ok());
}

// THE CRASH-ONLY HALF: the range is still IN THE LIVE WAL at reopen.
//
// Every other test here flushes before closing, so by the time anything
// reopens, the tombstone is in a TABLE and replay never sees a `kDeleteRange`
// record at all. That leaves the recovery path -- the one section 8.1's whole
// argument was about -- unexercised, and `BM99` proved it: a mutant that
// replays a range as nothing SURVIVED every test above.
//
// So this one syncs and closes WITHOUT flushing, which is the only way the log
// still carries the range when the next Open reads it.
TEST(RangeDelete, ARangeStillInTheWalIsReplayedAtOpen) {
  TestEnvironment t;
  {
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
    for (int i = 0; i < 20; ++i) Put(db.get(), KeyAt(i), Value(i, 8));
    WriteBatch b;
    b.DeleteRange(BoundAt(5), BoundAt(10));
    SeqNum s = 0;
    ASSERT_TRUE(db->Write(b, &s).ok());
    SeqNum mark = 0;
    ASSERT_TRUE(db->Sync(&mark).ok());
    ASSERT_EQ(0u, Tables(&t).size())
        << "this test is only about replay if nothing was flushed";
    ASSERT_TRUE(db->Close().ok());
  }
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  EXPECT_EQ(Value(4, 8), Get(*db, KeyAt(4))) << "below the range";
  EXPECT_EQ("<absent>", Get(*db, KeyAt(5))) << "the start is INCLUSIVE";
  EXPECT_EQ("<absent>", Get(*db, KeyAt(9)));
  EXPECT_EQ(Value(10, 8), Get(*db, KeyAt(10))) << "the end is EXCLUSIVE";
  ASSERT_TRUE(db->Close().ok());
}

// AND THE UNBOUNDED FORM THROUGH THE SAME PATH, because an empty end is how
// unboundedness travels in the log and nothing else asserts that round trip.
TEST(RangeDelete, AnUnboundedRangeStillInTheWalIsReplayedAtOpen) {
  TestEnvironment t;
  {
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
    for (int i = 0; i < 20; ++i) Put(db.get(), KeyAt(i), Value(i, 8));
    WriteBatch b;
    b.DeleteRange(BoundAt(5), Bound::Unbounded());
    SeqNum s = 0;
    ASSERT_TRUE(db->Write(b, &s).ok());
    SeqNum mark = 0;
    ASSERT_TRUE(db->Sync(&mark).ok());
    ASSERT_TRUE(db->Close().ok());
  }
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  EXPECT_EQ(Value(4, 8), Get(*db, KeyAt(4)));
  EXPECT_EQ("<absent>", Get(*db, KeyAt(5)));
  EXPECT_EQ("<absent>", Get(*db, KeyAt(19))) << "and everything above it";
  ASSERT_TRUE(db->Close().ok());
}

// THE SAME RULE AT EVERY PLACE IT IS WRITTEN, AND THE RESIDUAL IT CANNOT CLOSE.
//
// "A tombstone at S hides sequences strictly below S" is spelled three times.
// Three copies of one rule is the shape that drifts, so they are asserted
// together rather than by inspection -- and what this CANNOT do is stated,
// because a test that claims more than it covers is worse than none:
//
//   `VersionGet` and `IterImpl` are reached by ONE WORKLOAD here, which is what
//   makes them genuinely compared: the same batch, the same sequence, two
//   readers.
//
//   `MemTable::Get` IS NOT ON ANY DB READ PATH -- no caller in `src/`, because
//   the DB reads memtables through `MergedIter` -- so no workload can reach it
//   from here. It is asserted against a hand-built memtable instead. THE
//   RESIDUAL IS THAT THE THIRD COPY IS COMPARED AGAINST THE RULE AND NOT
//   AGAINST THE OTHER TWO, and nothing in this test would notice if the DB's
//   two drifted together away from it.
TEST(RangeDelete, TheSameRuleHoldsAtEveryPlaceItIsWritten) {
  TestEnvironment t;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());

  // ONE BATCH: a Set AFTER a DeleteRange that covers it. Both land at the same
  // sequence, which is the only way to reach the equal case at all.
  WriteBatch b;
  const std::string lo = "a";
  const std::string hi = "z";
  const std::string k = "same-seq";
  b.DeleteRange(Bound::At(Slice(lo)), Bound::At(Slice(hi)));
  b.Set(Slice(k), Slice("survives"));
  SeqNum s = 0;
  ASSERT_TRUE(db->Write(b, &s).ok());

  EXPECT_EQ("survives", Get(*db, k)) << "VersionGet: `>` not `>=`";

  bool seen = false;
  std::unique_ptr<Iterator> it = db->NewIter(IterOptions());
  for (bool ok = it->First(); ok; ok = it->Next()) {
    if (it->Key().ToString() == k) seen = true;
  }
  ASSERT_TRUE(it->Close().ok());
  EXPECT_TRUE(seen) << "IterImpl: the same rule, a different walk";
  ASSERT_TRUE(db->Close().ok());

  // The third copy, against the rule rather than against the other two.
  MemTable m;
  m.Add(5, ValueType::kValue, Slice(k), Slice("survives"));
  m.AddRangeTombstone(5, Slice(lo), Slice(hi), false);
  std::string v;
  EXPECT_TRUE(m.Get(Slice(k), 5, &v).ok()) << "MemTable::Get: equal does not hide";
  EXPECT_EQ("survives", v);
  // And strictly-below DOES hide, or the assertion above would pass for a copy
  // that never hid anything.
  MemTable m2;
  m2.Add(4, ValueType::kValue, Slice(k), Slice("hidden"));
  m2.AddRangeTombstone(5, Slice(lo), Slice(hi), false);
  EXPECT_FALSE(m2.Get(Slice(k), 5, &v).ok());
}

// ------------------------------------------- B3.5e: tombstones through compaction

// A RANGE DELETE SURVIVES THE COMPACTION THAT MOVES IT TO L1 -- the step BM97
// named and no earlier workload could reach.
TEST(RangeDelete, ARangeSurvivesTheCompactionThatMovesItToLevelOne) {
  TestEnvironment t;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  // A snapshot below the range keeps it alive: without one, clause 2 drops the
  // tombstone at the compaction and the test would pass for the wrong reason.
  FillAndFlush(db.get(), 0, 50);
  std::unique_ptr<Snapshot> below = db->NewSnapshot();
  WriteBatch b;
  b.DeleteRange(BoundAt(10), BoundAt(20));
  SeqNum s = 0;
  ASSERT_TRUE(db->Write(b, &s).ok());
  for (int round = 1; round < 4; ++round) FillAndFlush(db.get(), round * 50, 50);
  ASSERT_EQ(0, LevelCounts(&t).first) << "the compaction ran";

  EXPECT_EQ(Value(9, 512), Get(*db, KeyAt(9)));
  EXPECT_EQ("<absent>", Get(*db, KeyAt(15))) << "the tombstone reached L1";
  EXPECT_EQ(Value(20, 512), Get(*db, KeyAt(20)));
  // AND THE SNAPSHOT BELOW IT STILL SEES WHAT IT HID.
  std::string v;
  const std::string k = KeyAt(15);
  ASSERT_TRUE(below->Get(Slice(k), &v).ok());
  EXPECT_EQ(Value(15, 512), v);
  ASSERT_TRUE(below->Close().ok());
  ASSERT_TRUE(db->Close().ok());
}

// AND ACROSS A REOPEN, which is where the run check and the exclusive bound are
// read back: an Open that refuses is a split that produced overlapping bounds.
TEST(RangeDelete, ACompactedRangeSurvivesAReopen) {
  TestEnvironment t;
  {
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
    FillAndFlush(db.get(), 0, 50);
    std::unique_ptr<Snapshot> below = db->NewSnapshot();
    WriteBatch b;
    b.DeleteRange(BoundAt(10), BoundAt(20));
    SeqNum s = 0;
    ASSERT_TRUE(db->Write(b, &s).ok());
    for (int round = 1; round < 4; ++round) FillAndFlush(db.get(), round * 50, 50);
    ASSERT_TRUE(below->Close().ok());
    ASSERT_TRUE(db->Close().ok());
  }
  std::unique_ptr<DB> db;
  const Status s = DB::Open(t.env(), kDir, FlushingCaps(), &db);
  ASSERT_TRUE(s.ok()) << s.ToString()
                      << " -- a refused Open here means the split produced "
                         "bounds that overlap";
  EXPECT_EQ(Value(9, 512), Get(*db, KeyAt(9)));
  EXPECT_EQ("<absent>", Get(*db, KeyAt(15)));
  EXPECT_EQ(Value(20, 512), Get(*db, KeyAt(20)));
  ASSERT_TRUE(db->Close().ok());
}

// A TOMBSTONE SPANNING SEVERAL OUTPUT FILES IS SPLIT, AND EVERY PIECE APPLIES.
// The range covers most of the key space, so it crosses the roll boundaries --
// and a piece lost at any boundary shows up as a key that came back.
TEST(RangeDelete, ARangeSpanningOutputFilesIsSplitAndEveryPieceApplies) {
  TestEnvironment t;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  FillAndFlush(db.get(), 0, 200);
  std::unique_ptr<Snapshot> below = db->NewSnapshot();
  WriteBatch b;
  b.DeleteRange(BoundAt(10), BoundAt(190));
  SeqNum s = 0;
  ASSERT_TRUE(db->Write(b, &s).ok());
  for (int round = 0; round < 3; ++round) FillAndFlush(db.get(), 1000 + round * 50, 50);
  const std::pair<int, int> levels = LevelCounts(&t);
  ASSERT_EQ(0, levels.first);
  ASSERT_GT(levels.second, 1) << "the output must be a RUN for a split to exist";

  for (int i = 0; i < 10; ++i) EXPECT_EQ(Value(i, 512), Get(*db, KeyAt(i))) << i;
  for (int i = 10; i < 190; ++i) EXPECT_EQ("<absent>", Get(*db, KeyAt(i))) << i;
  for (int i = 190; i < 200; ++i) EXPECT_EQ(Value(i, 512), Get(*db, KeyAt(i))) << i;
  ASSERT_TRUE(below->Close().ok());
  ASSERT_TRUE(db->Close().ok());

  // AND THE REOPEN, WHICH IS WHERE THE CLIP IS ACTUALLY CHECKED. In-process
  // reads cannot see an unclipped tombstone: every file it was wrongly written
  // into answers the same way the clipped one would. What an unclipped
  // tombstone breaks is the BOUNDS -- it reaches past its own file's range, so
  // two files of the run overlap -- and `VerifyL1IsARun` is the thing that
  // says so. `BM102` survived until this reopen existed.
  std::unique_ptr<DB> re;
  const Status s2 = DB::Open(t.env(), kDir, FlushingCaps(), &re);
  ASSERT_TRUE(s2.ok()) << s2.ToString()
                       << " -- a tombstone was written past its file's bounds, "
                          "so the run overlaps";
  for (int i = 10; i < 190; ++i) EXPECT_EQ("<absent>", Get(*re, KeyAt(i))) << i;
  ASSERT_TRUE(re->Close().ok());
}

// CLAUSE 2 OVER A RANGE: with nothing observable below it, a tombstone whose
// work is done is DROPPED -- and what it hid goes with it.
TEST(RangeDelete, ATombstoneWithNothingLeftToHideIsDroppedByCompaction) {
  TestEnvironment t;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  FillAndFlush(db.get(), 0, 50);
  WriteBatch b;
  b.DeleteRange(BoundAt(10), BoundAt(20));
  SeqNum s = 0;
  ASSERT_TRUE(db->Write(b, &s).ok());
  for (int round = 1; round < 4; ++round) FillAndFlush(db.get(), round * 50, 50);
  ASSERT_EQ(0, LevelCounts(&t).first);

  // The answers are unchanged whether or not the tombstone survived -- which is
  // the point of the drop claim -- so what is asserted is the ANSWER.
  for (int i = 10; i < 20; ++i) EXPECT_EQ("<absent>", Get(*db, KeyAt(i))) << i;
  EXPECT_EQ(Value(9, 512), Get(*db, KeyAt(9)));
  ASSERT_TRUE(db->Close().ok());

  // AND IT REALLY WAS DROPPED: no table names a range tombstone any more.
  std::unique_ptr<DB> re;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &re).ok());
  ASSERT_TRUE(re->Close().ok());
  int with_tombstones = 0;
  for (const std::string& name : Tables(&t)) {
    const std::string image = t.ContentNow(kDir + "/" + name);
    const sst::TableCheck v = sst::ValidateTable(Slice(image));
    ASSERT_TRUE(v.ok()) << v.why;
    if (v.range_tombstones > 0) ++with_tombstones;
  }
  EXPECT_EQ(0, with_tombstones)
      << "the tombstone had nothing left to hide and should have gone with it";
}

// AN UNBOUNDED TOMBSTONE THROUGH A COMPACTION: only the LAST output may carry
// the unbounded piece, and every earlier one gets it clipped. The Open would
// refuse otherwise, so a passing reopen is the assertion.
TEST(RangeDelete, AnUnboundedRangeIsSplitWithTheOpenPieceInTheLastFile) {
  TestEnvironment t;
  {
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
    FillAndFlush(db.get(), 0, 200);
    std::unique_ptr<Snapshot> below = db->NewSnapshot();
    WriteBatch b;
    b.DeleteRange(BoundAt(50), Bound::Unbounded());
    SeqNum s = 0;
    ASSERT_TRUE(db->Write(b, &s).ok());
    for (int round = 0; round < 3; ++round) FillAndFlush(db.get(), 1000 + round * 50, 50);
    ASSERT_EQ(0, LevelCounts(&t).first);
    ASSERT_TRUE(below->Close().ok());
    ASSERT_TRUE(db->Close().ok());
  }
  std::unique_ptr<DB> db;
  const Status s = DB::Open(t.env(), kDir, FlushingCaps(), &db);
  ASSERT_TRUE(s.ok()) << s.ToString()
                      << " -- the unbounded piece landed somewhere other than "
                         "the last file of the run";
  for (int i = 0; i < 50; ++i) EXPECT_EQ(Value(i, 512), Get(*db, KeyAt(i))) << i;
  for (int i = 50; i < 200; ++i) EXPECT_EQ("<absent>", Get(*db, KeyAt(i))) << i;
  ASSERT_TRUE(db->Close().ok());
}

// ------------------------------- B3.6: file lifetime by reference count
//
// THE ASSERTION IS ABOUT THE FILE, NOT ABOUT THE ANSWER. A snapshot reading
// through a deleted file returns the right answer today because `table.h` holds
// the image resident -- so a test that only read through the snapshot would
// pass with or without the reference count. What is asserted is that THE FILE
// IS STILL THERE while a reader holds it, and gone once the reader does not.

TEST(FileLifetime, AnInputFileOutlivesTheCompactionWhileASnapshotHoldsIt) {
  TestEnvironment t;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  FillAndFlush(db.get(), 0, 50);
  const std::vector<std::string> inputs = Tables(&t);
  ASSERT_EQ(1u, inputs.size());

  std::unique_ptr<Snapshot> snap = db->NewSnapshot();
  for (int round = 1; round < 4; ++round) FillAndFlush(db.get(), round * 50, 50);
  ASSERT_EQ(0, LevelCounts(&t).first) << "the compaction ran";

  // THE INPUT FILE IS STILL ON DISK, because the snapshot's Version holds it.
  const std::vector<std::string> after = Tables(&t);
  EXPECT_NE(after.end(), std::find(after.begin(), after.end(), inputs[0]))
      << "an input file was deleted while a snapshot was still reading it; that "
         "is correct today only because the bytes happen to be resident";

  // AND IT GOES WHEN THE SNAPSHOT DOES.
  ASSERT_TRUE(snap->Close().ok());
  snap.reset();
  SeqNum mark = 0;
  ASSERT_TRUE(db->Sync(&mark).ok());
  const std::vector<std::string> reclaimed = Tables(&t);
  EXPECT_EQ(reclaimed.end(),
            std::find(reclaimed.begin(), reclaimed.end(), inputs[0]))
      << "the last reader is gone and the file was not reclaimed";
  ASSERT_TRUE(db->Close().ok());
}

// GF-14: THE OTHER HALF. With no reader, an input file is deleted by the
// compaction itself and never enters the obsolete list at all.
TEST(FileLifetime, WithNoReaderTheInputFilesGoImmediately) {
  TestEnvironment t;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  FillAndFlush(db.get(), 0, 50);
  const std::vector<std::string> inputs = Tables(&t);
  for (int round = 1; round < 4; ++round) FillAndFlush(db.get(), round * 50, 50);
  ASSERT_EQ(0, LevelCounts(&t).first);
  const std::vector<std::string> after = Tables(&t);
  for (const std::string& in : inputs) {
    EXPECT_EQ(after.end(), std::find(after.begin(), after.end(), in))
        << in << " outlived a compaction nobody was reading through";
  }
  ASSERT_TRUE(db->Close().ok());
}

// AN ITERATOR COUNTS AS A READER TOO. `NewIter` was a NAMED GAP at B3.4 -- the
// frozen interface promises pinning for `Snapshot` and says nothing about
// `Iterator` -- and this is where it stops being a gap: the iterator holds its
// Version, so it holds its files, and the count sees it.
TEST(FileLifetime, AnOpenIteratorHoldsItsInputFilesToo) {
  TestEnvironment t;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  FillAndFlush(db.get(), 0, 50);
  const std::vector<std::string> inputs = Tables(&t);
  ASSERT_EQ(1u, inputs.size());

  std::unique_ptr<Iterator> it = db->NewIter(IterOptions());
  ASSERT_TRUE(it->First());
  for (int round = 1; round < 4; ++round) FillAndFlush(db.get(), round * 50, 50);
  ASSERT_EQ(0, LevelCounts(&t).first);
  const std::vector<std::string> after = Tables(&t);
  EXPECT_NE(after.end(), std::find(after.begin(), after.end(), inputs[0]))
      << "an iterator's input file was deleted underneath it";

  // AND THE ITERATOR STILL WALKS, which is the answer half beside the file half.
  //
  // FIFTY, NOT TWO HUNDRED: an iterator captures its Version and its sequence
  // when it is created, so it sees the database as it was then -- the later
  // writes are invisible to it by construction. The first version of this test
  // expected 200 and was asserting that an iterator is live rather than
  // snapshotted, which is not what the frozen interface says.
  int seen = 1;
  while (it->Next()) ++seen;
  EXPECT_EQ(50, seen) << "an iterator sees the database as of its creation";
  ASSERT_TRUE(it->Close().ok());
  ASSERT_TRUE(db->Close().ok());
}

// TWO COMPACTIONS UNDER ONE SNAPSHOT, which is B3-D5's stated gate: "a test that
// takes a snapshot, compacts twice, and reads through it".
TEST(FileLifetime, ASnapshotSurvivesTwoCompactionsAndReadsThroughThem) {
  TestEnvironment t;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(t.env(), kDir, FlushingCaps(), &db).ok());
  FillAndFlush(db.get(), 0, 50);
  std::unique_ptr<Snapshot> snap = db->NewSnapshot();
  for (int round = 1; round < 8; ++round) FillAndFlush(db.get(), round * 50, 50);

  std::string v;
  const std::string k = KeyAt(7);
  ASSERT_TRUE(snap->Get(Slice(k), &v).ok()) << "the snapshot lost its version";
  EXPECT_EQ(Value(7, 512), v);
  std::unique_ptr<Iterator> it = snap->NewIter(IterOptions());
  int seen = 0;
  for (bool ok = it->First(); ok; ok = it->Next()) ++seen;
  EXPECT_EQ(50, seen) << "the snapshot sees exactly what it saw when taken";
  ASSERT_TRUE(it->Close().ok());
  ASSERT_TRUE(snap->Close().ok());
  ASSERT_TRUE(db->Close().ok());
}

// ------------------------------------------------------- the Sync precondition
//
// SINGLE-CALLER, INDUCED IN BOTH DIRECTIONS (GF-14). One direction alone would
// not distinguish a guard that fires from a guard that always fires.
//
// The contract was always single-caller -- db.h says "B5's poller owns this",
// and the TSan harness says it in the strongest form available: "one writer and
// one syncer ... NOT MORE, BECAUSE MORE WOULD BE A CLAIM THE CONTRACT DOES NOT
// MAKE." What B3.4 changed is the COST of violating it: the manifest now has
// two appenders, and AppendGroup takes no lock.
//
// The guard is induced HERE rather than by racing two real Syncs, because a
// race induces it only probably and this induces it every time.
TEST(SyncPrecondition, ASecondConcurrentClaimAborts) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  std::atomic<bool> held{false};
  EXPECT_DEATH(
      {
        const SingleCaller first(&held);
        const SingleCaller second(&held);
        (void)first;
        (void)second;
      },
      "");
}

// AND THAT `Sync` ITSELF CLAIMS IT -- which the two tests around this one do
// NOT show. `BM82` removes the claim from `Sync` and leaves `SingleCaller`
// intact, and it SURVIVED the pair above: they prove the guard works, not that
// the guarded path uses it. BM55's question, asked again and answered the same
// way -- *is the line this patch is aimed at actually the line that carries the
// property?*
//
// DETERMINISTIC, AND NOT A RACE. The promotion hook fires inside `Sync`, on the
// durable image changing, so re-entering `Sync` from it claims the guard a
// second time on ONE thread. Racing two Syncs would induce this only probably;
// this induces it every time.
//
// The hook fires ONCE on purpose. Without that, a build with the claim removed
// would recurse until the stack gave out -- and a death test cannot tell a
// guard firing from a crash, so the mutant would pass for the wrong reason.
void ReenterSync(void* ctx, const testenv::DurableImage&) {
  static bool fired = false;
  if (fired) return;
  fired = true;
  SeqNum inner = 0;
  (void)static_cast<DB*>(ctx)->Sync(&inner);
}

TEST(SyncPrecondition, ReEnteringSyncFromInsideItAborts) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_DEATH(
      {
        TestEnvironment t;
        std::unique_ptr<DB> db;
        if (!DB::Open(t.env(), kDir, FlushingCaps(), &db).ok()) return;
        Put(db.get(), KeyAt(0), "1");
        t.set_promotion_hook(&ReenterSync, db.get());
        SeqNum w = 0;
        (void)db->Sync(&w);
      },
      "");
}

TEST(SyncPrecondition, SequentialClaimsAreFine) {
  std::atomic<bool> held{false};
  { const SingleCaller first(&held); }
  { const SingleCaller second(&held); }
  SUCCEED();
}

}  // namespace
}  // namespace rift
