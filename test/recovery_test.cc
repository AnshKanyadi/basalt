// Recovery, asserted against THE HARNESS'S OWN RECORD.
//
// Not against engine/model, and not against a second decoding of the same
// bytes. Section 13.4b: agreement between two paths is not the same as either
// path being right, and two paths that share an assumption agree most
// confidently exactly where they are both wrong. So each test below keeps its
// own list of what it submitted and which Sync returned, and computes the
// expected recovered state from that list alone.
#include "recovery.h"

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "call_site.h"
#include "reader.h"
#include "test_env.h"
#include "wal.h"

namespace rift {
namespace wal {
namespace {

using testenv::DurableImage;
using testenv::TestEnvironment;

const std::string kDir = "db";

// RECOVERY NO LONGER TAKES THE DIRECTORY LOCK -- see recovery.h. These tests
// take it the way DB::Open does.
//
// They also play the MANIFEST'S PART, because the manifest is what supplies the
// fresh WAL's number now that B1-D7's max+1 has expired. The scan below is that
// number computed the way the directory would have answered it in B1: these
// tests are about the WAL and not about the manifest, so the number is supplied
// rather than made a dependency.
Status RecoverHere(Env* env, const std::string& dir, const Caps& caps,
                   RecoveryResult* out,
                   RecoverOptions options = RecoverOptions()) {
  if (options.next_file_number == 1) {
    std::vector<std::string> children;
    Status g = env->GetChildren(dir, &children);
    if (!g.ok() && g.code() != Status::Code::kNotFound) return g;
    uint64_t highest = 0;
    for (const std::string& name : children) {
      if (name.size() != 10 || name.compare(6, 4, ".log") != 0) continue;
      uint64_t n = 0;
      bool digits = true;
      for (std::size_t i = 0; i < 6; ++i) {
        if (name[i] < '0' || name[i] > '9') { digits = false; break; }
        n = n * 10 + static_cast<uint64_t>(name[i] - '0');
      }
      if (digits && n > highest) highest = n;
    }
    options.next_file_number = highest + 1;
  }
  // And the manifest's part again: the set of live WALs. In B1 nothing was
  // ever deleted, so the manifest would have named every number ever issued --
  // 1 through the one this call is about to create. Naming them from what is
  // PRESENT instead would make the set follow the directory, and a deleted WAL
  // would drop out of the set along with the file: the check would then be
  // asking the directory whether the directory was complete.
  if (options.named_wals.empty()) {
    for (uint64_t n = 1; n <= options.next_file_number; ++n) {
      options.named_wals.push_back(n);
    }
  }
  FileLockPtr lock;
  Status s = env->LockFile(dir + "/LOCK", &lock);
  if (!s.ok()) return s;
  s = Recover(env, dir, caps, options, out);
  (void)env->UnlockFile(std::move(lock));
  return s;
}

std::vector<Op> OneSet(const std::string& k, const std::string& v) {
  std::vector<Op> ops;
  Op op;
  op.kind = OpKind::kSet;
  op.key = Slice(k);
  op.value = Slice(v);
  ops.push_back(op);
  return ops;
}

// The harness's own record: what was submitted, and what the last Sync that
// RETURNED had covered when it returned. Nothing here consults the engine.
struct Submission {
  SeqNum seq;
  std::string key;
  std::string value;
  bool covered_by_a_returned_sync = false;
};

std::string Get(const MemTable& m, const std::string& key, SeqNum snap) {
  std::string v;
  const Status s = m.Get(Slice(key), snap, &v);
  return s.ok() ? v : std::string("<absent>");
}

// ------------------------------------------------- the exactness contract

TEST(Recovery, RecoversExactlyWhatTheLastReturnedSyncHadCovered) {
  TestEnvironment t;
  std::vector<Submission> submitted;
  SeqNum promised = 0;
  {
    std::unique_ptr<Wal> w;
    ASSERT_TRUE(Wal::Open(t.env(), kDir, 1, Caps(), &w).ok());

    for (int i = 1; i <= 3; ++i) {
      const std::string k = "k" + std::to_string(i);
      ASSERT_TRUE(w->Apply(static_cast<SeqNum>(i), OneSet(k, "v" + std::to_string(i))).ok());
      submitted.push_back({static_cast<SeqNum>(i), k, "v" + std::to_string(i), false});
    }
    SeqNum mark = 0;
    ASSERT_TRUE(w->Sync(&mark).ok());
    // THE HARNESS RECORDS WHAT THE SYNC RETURNED. Everything submitted before
    // this call is now covered; nothing after it is.
    promised = mark;
    for (Submission& s : submitted) s.covered_by_a_returned_sync = true;

    for (int i = 4; i <= 6; ++i) {
      const std::string k = "k" + std::to_string(i);
      ASSERT_TRUE(w->Apply(static_cast<SeqNum>(i), OneSet(k, "v" + std::to_string(i))).ok());
      submitted.push_back({static_cast<SeqNum>(i), k, "v" + std::to_string(i), false});
    }
    // No second Sync. The kill takes everything since the one that returned.
  }
  t.Kill();

  std::unique_ptr<TestEnvironment> reopened =
      TestEnvironment::FromImage(t.Image(), testenv::FaultPlan());
  RecoveryResult r;
  ASSERT_TRUE(RecoverHere(reopened->env(), kDir, Caps(), &r).ok());

  EXPECT_EQ(r.recovered_seq, promised)
      << "recovery landed on a watermark the harness never saw promised";

  // EXACTLY. Every covered submission present with its value; every uncovered
  // one absent. Computed from the harness's list, not from a second engine.
  for (const Submission& s : submitted) {
    if (s.covered_by_a_returned_sync) {
      EXPECT_EQ(Get(*r.table, s.key, r.recovered_seq), s.value)
          << s.key << " was covered by a returned Sync and did not survive: "
             "committed is not forever";
    } else {
      EXPECT_EQ(Get(*r.table, s.key, r.recovered_seq), "<absent>")
          << s.key << " was never covered by a returned Sync and survived "
             "anyway: recovery returned MORE than was promised";
    }
  }
  EXPECT_EQ(r.committed_batches, 3u);
}

// The other direction of the same contract, and the one an engine that never
// synced would pass: nothing promised means nothing recovered.
TEST(Recovery, ASyncThatNeverRanPromisesNothing) {
  TestEnvironment t;
  {
    std::unique_ptr<Wal> w;
    ASSERT_TRUE(Wal::Open(t.env(), kDir, 1, Caps(), &w).ok());
    ASSERT_TRUE(w->Apply(1, OneSet("a", "1")).ok());
  }
  t.Kill();
  std::unique_ptr<TestEnvironment> reopened =
      TestEnvironment::FromImage(t.Image(), testenv::FaultPlan());
  RecoveryResult r;
  ASSERT_TRUE(RecoverHere(reopened->env(), kDir, Caps(), &r).ok());
  EXPECT_EQ(r.recovered_seq, 0u);
  EXPECT_EQ(Get(*r.table, "a", 100), "<absent>");
}

TEST(Recovery, CloseThenReopenIsIndistinguishableFromKillThenReopen) {
  // Close does not sync, deliberately: a Close that synced would make clean
  // shutdown a hidden durability event engine/model's Close does not have, and
  // the two engines would disagree in precisely the differential rig.
  auto run = [](bool close_cleanly) {
    TestEnvironment t;
    {
      std::unique_ptr<Wal> w;
      EXPECT_TRUE(Wal::Open(t.env(), kDir, 1, Caps(), &w).ok());
      EXPECT_TRUE(w->Apply(1, OneSet("a", "1")).ok());
      SeqNum mark = 0;
      EXPECT_TRUE(w->Sync(&mark).ok());
      EXPECT_TRUE(w->Apply(2, OneSet("b", "2")).ok());
      if (close_cleanly) EXPECT_TRUE(w->Close().ok());
    }
    t.Kill();
    std::unique_ptr<TestEnvironment> re =
        TestEnvironment::FromImage(t.Image(), testenv::FaultPlan());
    RecoveryResult r;
    EXPECT_TRUE(RecoverHere(re->env(), kDir, Caps(), &r).ok());
    return r.recovered_seq;
  };
  EXPECT_EQ(run(true), run(false));
  EXPECT_EQ(run(true), 1u);
}

// --------------------------------- the watermark is READ, never inferred

// A PROBE IMAGE, and it can only be hand-built. The writer always emits a
// GROUP_END whose high_seq equals the last batch's sequence, so in every log
// this engine produces the recorded field and the inferred maximum are EQUAL --
// which is exactly why the distinction cannot be tested with a real log and has
// to be probed with a constructed one.
//
// Track A's BUG-005 was a watermark inferred from the shape of a structure
// rather than read from where it was written. It cost three cycles.
TEST(Recovery, TheWatermarkIsTheRecordedFieldAndNotTheHighestSequenceSeen) {
  TestEnvironment t;
  {
    // Build a real WAL first, so the FILE_HEADER and framing are genuine.
    std::unique_ptr<Wal> w;
    ASSERT_TRUE(Wal::Open(t.env(), kDir, 1, Caps(), &w).ok());
    ASSERT_TRUE(w->Apply(4, OneSet("a", "1")).ok());
    ASSERT_TRUE(w->Apply(9, OneSet("b", "2")).ok());
    SeqNum mark = 0;
    ASSERT_TRUE(w->Sync(&mark).ok());
    ASSERT_EQ(mark, 9u);
  }
  t.Kill();

  // Now rewrite the GROUP_END so its recorded high_seq is 77 while the highest
  // BATCH sequence is still 9. The two candidate sources now differ.
  DurableImage image = t.Image();
  const std::string path = kDir + "/000001.log";
  ASSERT_EQ(image.count(path), 1u);
  std::string bytes = image[path];

  const ScanResult scan = ScanLog(Slice(bytes));
  ASSERT_EQ(scan.outcome, ScanOutcome::kCleanEnd);
  ASSERT_GT(scan.records.size(), 0u);
  const LogicalRecord& ge = scan.records.back();
  ASSERT_EQ(ge.kind, RecordKind::kGroupEnd);

  std::string replacement;
  EncodeGroupEnd(77, 2, &replacement);
  ASSERT_EQ(replacement.size(), ge.payload.size());
  const uint64_t payload_at = ge.offset + kHeaderBytes;
  bytes.replace(payload_at, replacement.size(), replacement);
  // Re-checksum, or the reader rejects it before the watermark is ever read.
  const uint32_t crc = FragmentCrc(static_cast<uint16_t>(replacement.size()),
                                   FragmentType::kFull, Slice(replacement));
  for (int i = 0; i < 4; ++i) {
    bytes[ge.offset + static_cast<std::size_t>(i)] =
        static_cast<char>((crc >> (8 * i)) & 0xff);
  }
  image[path] = bytes;

  std::unique_ptr<TestEnvironment> re =
      TestEnvironment::FromImage(image, testenv::FaultPlan());
  RecoveryResult r;
  ASSERT_TRUE(RecoverHere(re->env(), kDir, Caps(), &r).ok());
  EXPECT_EQ(r.recovered_seq, 77u)
      << "the watermark came from the highest sequence SEEN rather than from "
         "the GROUP_END field where it was WRITTEN. The two are equal in every "
         "log this engine produces, which is why only a probe can tell them "
         "apart -- and why an implementation that infers can pass every other "
         "test in this file";
}

// ---------------------------------------------- a WAL the manifest named

// B1's check was that WAL NUMBERING was gapless, which was a property of no
// file ever being deleted. B2 deletes flushed WALs, so the check is REPLACED:
// the manifest names the live WALs, and one that is named and absent is a lost
// directory entry. THE SITUATION THIS TEST BUILDS IS UNCHANGED -- three WALs,
// lose the middle one -- because it is the situation that matters; only the
// mechanism that notices has moved. See recovery.h for why sequences cannot
// answer this and file identities can.
TEST(Recovery, ADeletedWalMakesTheOpenFail) {
  TestEnvironment t;
  {
    RecoveryResult r1;
    ASSERT_TRUE(RecoverHere(t.env(), kDir, Caps(), &r1).ok());  // creates 000001
    SeqNum mark = 0;
    ASSERT_TRUE(r1.wal->Apply(1, OneSet("a", "1")).ok());
    ASSERT_TRUE(r1.wal->Sync(&mark).ok());
  }
  {
    RecoveryResult r2;
    ASSERT_TRUE(RecoverHere(t.env(), kDir, Caps(), &r2).ok());  // creates 000002
    ASSERT_EQ(r2.file_numbers.size(), 1u);
  }
  {
    RecoveryResult r3;
    ASSERT_TRUE(RecoverHere(t.env(), kDir, Caps(), &r3).ok());  // creates 000003
    ASSERT_EQ(r3.file_numbers.size(), 2u);
  }
  // Three WALs now: 1, 2, 3. Lose the middle one, as a lost directory entry
  // would.
  ASSERT_TRUE(t.env()->DeleteFile(kDir + "/000002.log").ok());
  RecoveryResult r4;
  const Status s = RecoverHere(t.env(), kDir, Caps(), &r4);
  EXPECT_EQ(s.code(), Status::Code::kCorruption) << s.ToString();
  EXPECT_NE(s.message().find("000002.log"), std::string::npos) << s.ToString();
  EXPECT_NE(s.message().find("cannot prove"), std::string::npos) << s.ToString();
}

// THE HALF THE REPLACEMENT DOES NOT COVER, ASSERTED AS A NEGATIVE RATHER THAN
// LEFT SILENT. The highest named WAL may be absent, because that is the one a
// crash caught between naming and creating -- it holds nothing. A rule that
// refused it would refuse every crash in that window, which is the normal case.
TEST(Recovery, TheHighestNamedWalMayBeAbsentBecauseItIsTheOneBeingCreated) {
  TestEnvironment t;
  {
    RecoveryResult r1;
    ASSERT_TRUE(RecoverHere(t.env(), kDir, Caps(), &r1).ok());
    SeqNum mark = 0;
    ASSERT_TRUE(r1.wal->Apply(1, OneSet("a", "1")).ok());
    ASSERT_TRUE(r1.wal->Sync(&mark).ok());
  }
  RecoveryResult r;
  RecoverOptions options;
  options.next_file_number = 2;
  // WAL 2 is named and does not exist: the crash landed between naming it and
  // creating it, which is the window naming-before-creating deliberately opens
  // because it is the harmless one.
  options.named_wals = {1, 2};
  const Status s = RecoverHere(t.env(), kDir, Caps(), &r, options);
  EXPECT_TRUE(s.ok()) << s.ToString();
  EXPECT_EQ(r.recovered_seq, 1u);
}

TEST(Recovery, FileNumbersAreSortedByValueAndNotByDirectoryOrder) {
  TestEnvironment t;
  for (int i = 0; i < 12; ++i) {
    RecoveryResult r;
    ASSERT_TRUE(RecoverHere(t.env(), kDir, Caps(), &r).ok());
  }
  RecoveryResult r;
  ASSERT_TRUE(RecoverHere(t.env(), kDir, Caps(), &r).ok());
  ASSERT_EQ(r.file_numbers.size(), 12u);
  for (std::size_t i = 0; i < r.file_numbers.size(); ++i) {
    EXPECT_EQ(r.file_numbers[i], i + 1)
        << "TestEnv hands children back reverse-sorted on purpose; an engine "
           "that sorted them as strings would put 000010 before 000009";
  }
}

// ------------------------------------------------- torn tails and corruption

// BATCH records past the last GROUP_END are discarded, and that is not an
// error and is not reported as one.
//
// THE ONLY WAY TO GET THEM ONTO DISK IS A TORN SYNC, and the first version of
// this test did not realise it: it applied batches and destroyed the Wal,
// leaving them in the engine's own memory where they never reached a file at
// all. The test passed, asserted nothing, and mutant BM2 SURVIVED it -- the
// checker could not see the defect because the situation it checks never
// occurred.
//
// Under the power-loss model unsynced bytes are lost, so uncommitted records
// survive a kill only when a Sync was torn after writing the batches and before
// its GROUP_END. That is section 7.4's G_{k-1} case, it is inside the contract
// rather than an exception to it, and the run stays bankable as evidence.
TEST(Recovery, BatchesAfterTheLastGroupEndAreDiscardedAndThatIsNotAnError) {
  // Discover the ordinal of the second Sync by running the workload once with
  // no faults. A hand-counted ordinal goes stale the first time the workload
  // changes and takes the test's meaning with it.
  uint64_t sync_ordinal = 0;
  uint64_t torn_prefix = 0;
  {
    TestEnvironment probe;
    std::unique_ptr<Wal> w;
    ASSERT_TRUE(Wal::Open(probe.env(), kDir, 1, Caps(), &w).ok());
    ASSERT_TRUE(w->Apply(1, OneSet("a", "1")).ok());
    SeqNum mark = 0;
    ASSERT_TRUE(w->Sync(&mark).ok());
    const std::size_t after_first_sync = probe.ContentNow(kDir + "/000001.log").size();
    ASSERT_TRUE(w->Apply(2, OneSet("b", "2")).ok());
    ASSERT_TRUE(w->Apply(3, OneSet("c", "3")).ok());
    ASSERT_TRUE(w->Sync(&mark).ok());
    for (const testenv::LedgerEntry& e : probe.ledger()) {
      if (e.site == CallSite::kWritableFileSync) sync_ordinal = e.ordinal;
    }
    // Promote exactly the two BATCH records and not the GROUP_END that follows.
    const std::string bytes = probe.ContentNow(kDir + "/000001.log");
    std::string group_end;
    EncodeGroupEnd(3, 2, &group_end);
    torn_prefix = bytes.size() - after_first_sync - (kHeaderBytes + group_end.size());
  }
  ASSERT_GT(sync_ordinal, 0u);
  ASSERT_GT(torn_prefix, 0u);

  testenv::FaultPlan plan;
  plan.At(sync_ordinal, testenv::Injection::kTornSync, torn_prefix);
  TestEnvironment t(plan);
  {
    std::unique_ptr<Wal> w;
    ASSERT_TRUE(Wal::Open(t.env(), kDir, 1, Caps(), &w).ok());
    ASSERT_TRUE(w->Apply(1, OneSet("a", "1")).ok());
    SeqNum mark = 0;
    ASSERT_TRUE(w->Sync(&mark).ok());
    ASSERT_TRUE(w->Apply(2, OneSet("b", "2")).ok());
    ASSERT_TRUE(w->Apply(3, OneSet("c", "3")).ok());
    EXPECT_EQ(w->Sync(&mark).code(), Status::Code::kKilled);
  }
  ASSERT_TRUE(t.dead());
  EXPECT_FALSE(t.exactness_suspended())
      << "a prefix-granular torn Sync is the contract model; this run is "
         "evidence, not characterization";

  // The bytes ARE on disk. This is the situation the first version never
  // reached, and without it BM2 has nothing to be caught by.
  const DurableImage image = t.Image();
  const ScanResult scan = ScanLog(Slice(image.at(kDir + "/000001.log")));
  ASSERT_GT(scan.records.size(), scan.committed_count)
      << "no uncommitted records reached the file, so this test cannot "
         "distinguish an engine that discards them from one that does not";

  std::unique_ptr<TestEnvironment> re =
      TestEnvironment::FromImage(image, testenv::FaultPlan());
  RecoveryResult r;
  ASSERT_TRUE(RecoverHere(re->env(), kDir, Caps(), &r).ok());
  EXPECT_EQ(r.recovered_seq, 1u);
  EXPECT_EQ(Get(*r.table, "a", 100), "1");
  EXPECT_EQ(Get(*r.table, "b", 100), "<absent>")
      << "a batch written but never covered by a returned Sync became part of "
         "the recovered state: recovery returned MORE than was promised";
  EXPECT_EQ(Get(*r.table, "c", 100), "<absent>");
  EXPECT_EQ(r.discarded_batches, 2u);
}

TEST(Recovery, InteriorCorruptionRefusesTheOpenAndSaysWhere) {
  TestEnvironment t;
  {
    std::unique_ptr<Wal> w;
    ASSERT_TRUE(Wal::Open(t.env(), kDir, 1, Caps(), &w).ok());
    ASSERT_TRUE(w->Apply(1, OneSet("a", "1")).ok());
    SeqNum mark = 0;
    ASSERT_TRUE(w->Sync(&mark).ok());
  }
  DurableImage image = t.Image();
  const std::string path = kDir + "/000001.log";
  std::string bytes = image[path];
  ASSERT_LT(bytes.size(), kBlockBytes);

  // The shape that makes this INTERIOR corruption rather than a torn tail: a
  // damaged region, and then a structurally valid record with a HIGHER sequence
  // sitting at a later block boundary. Prefix truncation cannot produce that,
  // so step 1's premise is false and truncation would be unsafe.
  //
  // Built by hand because the writer cannot produce it -- which is the point.
  bytes.append(kBlockBytes - bytes.size(), '\0');   // finish block 0 with zeros
  bytes.append(kBlockBytes, '\xAB');                // block 1 is garbage
  std::string later;
  EncodeBatch(99, OneSet("survivor", "v"), &later);  // block 2: valid, seq 99 > 1
  const uint32_t crc = FragmentCrc(static_cast<uint16_t>(later.size()),
                                   FragmentType::kFull, Slice(later));
  for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<char>((crc >> (8 * i)) & 0xff));
  bytes.push_back(static_cast<char>(later.size() & 0xff));
  bytes.push_back(static_cast<char>((later.size() >> 8) & 0xff));
  bytes.push_back(static_cast<char>(static_cast<uint8_t>(FragmentType::kFull)));
  bytes.append(later);
  image[path] = bytes;

  std::unique_ptr<TestEnvironment> re =
      TestEnvironment::FromImage(image, testenv::FaultPlan());
  RecoveryResult r;
  const Status s = RecoverHere(re->env(), kDir, Caps(), &r);
  ASSERT_EQ(s.code(), Status::Code::kCorruption) << s.ToString();
  EXPECT_NE(s.message().find("interior corruption"), std::string::npos);
  EXPECT_NE(s.message().find("byte offset"), std::string::npos)
      << "a refused open that cannot say where is one nobody can act on";
  EXPECT_NE(s.message().find("last committed group sequence 1"), std::string::npos)
      << "the report must name what recovery HAD committed, or an operator "
         "cannot tell how much is at stake";
}

TEST(Recovery, AFileWhoseHeaderDisagreesWithItsNameIsRefused) {
  TestEnvironment t;
  {
    std::unique_ptr<Wal> w;
    ASSERT_TRUE(Wal::Open(t.env(), kDir, 1, Caps(), &w).ok());
  }
  DurableImage image = t.Image();
  const std::string from = kDir + "/000001.log";
  // The bytes are perfect and every checksum passes; only the NAME is wrong.
  image[kDir + "/000002.log"] = image[from];
  image[from] = image[from];
  std::unique_ptr<TestEnvironment> re =
      TestEnvironment::FromImage(image, testenv::FaultPlan());
  RecoveryResult r;
  const Status s = RecoverHere(re->env(), kDir, Caps(), &r);
  EXPECT_EQ(s.code(), Status::Code::kCorruption) << s.ToString();
  EXPECT_NE(s.message().find("file number"), std::string::npos);
}

// ------------------------------------------------------------- the collapse

TEST(Recovery, ABatchIsCollapsedToOneOpPerKeyWithTheLastWinning) {
  TestEnvironment t;
  {
    std::unique_ptr<Wal> w;
    ASSERT_TRUE(Wal::Open(t.env(), kDir, 1, Caps(), &w).ok());
    // Held in named locals: a Slice is a pointer and a length owned by someone
    // else, and the first version of this test bound them to temporaries.
    const std::string key = "k";
    const std::string first = "first";
    const std::string last = "last";
    std::vector<Op> ops;
    Op a; a.kind = OpKind::kSet; a.key = Slice(key); a.value = Slice(first);
    Op b; b.kind = OpKind::kDelete; b.key = Slice(key);
    Op c; c.kind = OpKind::kSet; c.key = Slice(key); c.value = Slice(last);
    ops.push_back(a); ops.push_back(b); ops.push_back(c);
    ASSERT_TRUE(w->Apply(1, ops).ok());
    SeqNum mark = 0;
    ASSERT_TRUE(w->Sync(&mark).ok());
  }
  RecoveryResult r;
  ASSERT_TRUE(RecoverHere(t.env(), kDir, Caps(), &r).ok());
  EXPECT_EQ(Get(*r.table, "k", 100), "last")
      << "a Set after a Delete in the same batch must re-add the key, which is "
         "the model's rule reproduced";
  EXPECT_EQ(r.table->Count(), 1u)
      << "no two memtable entries may share a (user_key, seq) pair -- the "
         "invariant B1-D10's collapse exists to make assertable";
}

}  // namespace
}  // namespace wal
}  // namespace rift
