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
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "manifest.h"
#include "manifest_image.h"
#include "table_check.h"
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

}  // namespace
}  // namespace rift
