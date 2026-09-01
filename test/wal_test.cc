#include "wal.h"

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cap_adjudication.h"
#include "env_guard.h"
#include "reader.h"
#include "regime.h"
#include "sha256.h"
#include "test_env.h"

namespace basalt {
namespace wal {
namespace {

using testenv::TestEnvironment;

const std::string kDir = "db";
const std::string kLog = "db/000001.log";

std::vector<Op> OneSet(const std::string& k, const std::string& v) {
  std::vector<Op> ops;
  Op op;
  op.kind = OpKind::kSet;
  op.key = Slice(k);
  op.value = Slice(v);
  ops.push_back(op);
  return ops;
}

// --------------------------------------------------------------- sha256

TEST(Sha256, MatchesTheStandardVectors) {
  EXPECT_EQ(Sha256Hex("", 0),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(Sha256Hex("abc", 3),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const std::string long_in(1000, 'a');
  EXPECT_EQ(Sha256Hex(long_in.data(), long_in.size()).size(), 64u);
}

// ------------------------------------------------------ writer / reader

TEST(Wal, WhatTheWriterWritesIsWhatTheReaderAlreadyRefusedToMisread) {
  TestEnvironment t;
  std::unique_ptr<Wal> w;
  ASSERT_TRUE(Wal::Open(t.env(), kDir, 1, Caps(), &w).ok());
  ASSERT_TRUE(w->Apply(1, OneSet("a", "1")).ok());
  ASSERT_TRUE(w->Apply(2, OneSet("b", "2")).ok());
  SeqNum mark = 0;
  ASSERT_TRUE(w->Sync(&mark).ok());
  EXPECT_EQ(mark, 2u);
  EXPECT_EQ(w->DurableSeq(), 2u);

  const std::string bytes = t.ContentNow(kLog);
  const ScanResult r = ScanLog(Slice(bytes));
  ASSERT_EQ(r.outcome, ScanOutcome::kCleanEnd) << r.failure_reason;
  // FILE_HEADER, BATCH, BATCH, GROUP_END
  ASSERT_EQ(r.records.size(), 4u);
  EXPECT_EQ(r.records[0].kind, RecordKind::kFileHeader);
  EXPECT_EQ(r.records[3].kind, RecordKind::kGroupEnd);
  EXPECT_EQ(r.committed_count, 4u);
  EXPECT_EQ(r.last_committed_seq, 2u);
}

// A record larger than a block must fragment, and the fragments must reassemble
// into exactly what went in.
TEST(Wal, ARecordLargerThanABlockFragmentsAndReassembles) {
  TestEnvironment t;
  std::unique_ptr<Wal> w;
  ASSERT_TRUE(Wal::Open(t.env(), kDir, 1, Caps(), &w).ok());
  const std::string big_key(3 * kBlockBytes + 17, 'k');
  ASSERT_TRUE(w->Apply(1, OneSet(big_key, "v")).ok());
  SeqNum mark = 0;
  ASSERT_TRUE(w->Sync(&mark).ok());

  const std::string bytes = t.ContentNow(kLog);
  const ScanResult r = ScanLog(Slice(bytes));
  ASSERT_EQ(r.outcome, ScanOutcome::kCleanEnd) << r.failure_reason;
  ASSERT_EQ(r.records.size(), 3u);
  std::string expected;
  EncodeBatch(1, OneSet(big_key, "v"), &expected);
  EXPECT_EQ(r.records[1].payload, expected)
      << "the record did not survive its own fragmentation";
  EXPECT_GT(bytes.size(), 3 * kBlockBytes);
}

// ------------------------------------------------------- the byte digest

// THE PINNED WAL BYTES. Same workload, same bytes, SHA-256 pinned.
//
// This one test catches three things: ambient randomness, uninitialized padding
// reaching the file, and any float that reached a serialization path. It is
// also why MSan stays declined -- its value here is covered at a fraction of
// the cost.
//
// If this changes, the on-disk format changed. That is either a bug or a
// deliberate re-specification, and a deliberate one updates this constant in
// its own commit.
constexpr const char* kFixedWorkloadDigest =
    "08ca1b9bae0f7a0640a50cffcaa89104239f7584625112ba89d381bcaa4ebf36";

std::string WriteFixedWorkload(TestEnvironment* t) {
  std::unique_ptr<Wal> w;
  EXPECT_TRUE(Wal::Open(t->env(), kDir, 1, Caps(), &w).ok());
  // Chosen to cross a block boundary, so the zero-fill padding path is inside
  // the digest rather than beside it.
  EXPECT_TRUE(w->Apply(1, OneSet(std::string(20000, 'a'), "v1")).ok());
  EXPECT_TRUE(w->Apply(2, OneSet(std::string(20000, 'b'), "v2")).ok());
  EXPECT_TRUE(w->Apply(3, OneSet("", "")).ok());
  SeqNum mark = 0;
  EXPECT_TRUE(w->Sync(&mark).ok());
  return t->ContentNow(kLog);
}

TEST(Wal, ByteDigestIsPinned) {
  TestEnvironment t;
  const std::string bytes = WriteFixedWorkload(&t);
  EXPECT_EQ(Sha256Hex(bytes.data(), bytes.size()), kFixedWorkloadDigest);
}

TEST(Wal, TheSameWorkloadWritesTheSameBytes) {
  TestEnvironment a, b;
  const std::string x = WriteFixedWorkload(&a);
  const std::string y = WriteFixedWorkload(&b);
  EXPECT_EQ(Sha256Hex(x.data(), x.size()), Sha256Hex(y.data(), y.size()))
      << "two runs of one workload produced different WAL bytes, so something "
         "on the serialization path is not a function of the input";
}

// ----------------------------------------------- section 8.3's two assertions

// 1. THE ENV-CALL COUNTER DOES NOT MOVE ACROSS Apply.
TEST(Wal, ApplyMakesZeroEnvCalls) {
  TestEnvironment t;
  std::unique_ptr<Wal> w;
  ASSERT_TRUE(Wal::Open(t.env(), kDir, 1, Caps(), &w).ok());

  const uint64_t before = EnvCallsOnThisThread();
  for (int i = 0; i < 50; ++i) {
    ASSERT_TRUE(w->Apply(static_cast<SeqNum>(i + 1),
                         OneSet("k" + std::to_string(i), "v")).ok());
  }
  EXPECT_EQ(EnvCallsOnThisThread(), before)
      << "Apply performed I/O. 'Never blocks on I/O' is a contract, and an "
         "internal buffer that flushes when it fills satisfies it only until "
         "the moment it does not";

  SeqNum mark = 0;
  ASSERT_TRUE(w->Sync(&mark).ok());
  EXPECT_GT(EnvCallsOnThisThread(), before) << "and Sync must actually do I/O, "
                                               "or the assertion above is "
                                               "measuring nothing";
}

// 2. THE DB MUTEX IS NEVER HELD ACROSS AN ENV CALL.
int g_violations = 0;
void CountViolation(const char*) { ++g_violations; }

TEST(Wal, TheDbMutexIsNeverHeldAcrossAnEnvCall) {
  g_violations = 0;
  SetGuardViolationHandler(&CountViolation);
  {
    TestEnvironment t;
    std::unique_ptr<Wal> w;
    ASSERT_TRUE(Wal::Open(t.env(), kDir, 1, Caps(), &w).ok());
    for (int i = 0; i < 20; ++i) {
      ASSERT_TRUE(w->Apply(static_cast<SeqNum>(i + 1), OneSet("k", "v")).ok());
    }
    SeqNum mark = 0;
    ASSERT_TRUE(w->Sync(&mark).ok());
    ASSERT_TRUE(w->Close().ok());
  }
  SetGuardViolationHandler(nullptr);
  EXPECT_EQ(g_violations, 0)
      << "a Sync holding the DB mutex across an fsync blocks every reader for "
         "the fsync's duration -- the failure the lock ruling opened, and the "
         "one this guard exists to close";
}

// The guard must be able to see a violation, or the test above proves nothing.
TEST(Wal, TheMutexGuardCanActuallyFire) {
  g_violations = 0;
  SetGuardViolationHandler(&CountViolation);
  {
    TestEnvironment t;
    std::unique_ptr<Wal> w;
    ASSERT_TRUE(Wal::Open(t.env(), kDir, 1, Caps(), &w).ok());
    MutexHeldMarker pretend_the_db_lock_is_held;
    bool exists = false;
    (void)t.env()->FileExists(kLog, &exists);
  }
  SetGuardViolationHandler(nullptr);
  EXPECT_EQ(g_violations, 1);
}

// -------------------------------------------------------------- the caps

TEST(WalCaps, ConstructionFailsWhenTheBufferCapIsBelowTwiceTheRecordCap) {
  TestEnvironment t;
  Caps bad;
  bad.max_record_bytes = 1024;
  bad.wal_buffer_bytes = 2047;  // one byte short of 2x
  // BACKPRESSURE OFF, AND IT IS A REGIME STATEMENT RATHER THAN A CONVENIENCE.
  // With the policy on, the tripwire is UNREACHABLE through Apply: kBusy fires
  // strictly earlier by Caps::Ordered()'s margin, so this test would assert
  // nothing and would be green for it. Turning the policy off is what puts the
  // tripwire back in reach of the thing that tests it.
  bad.busy_bytes = 0;
  EXPECT_FALSE(bad.Ordered());
  std::unique_ptr<Wal> w;
  const Status s = Wal::Open(t.env(), kDir, 1, bad, &w);
  EXPECT_EQ(s.code(), Status::Code::kInvalidArgument) << s.ToString();

  bad.wal_buffer_bytes = 2048;
  EXPECT_TRUE(bad.Ordered());
  EXPECT_TRUE(Wal::Open(t.env(), kDir, 2, bad, &w).ok());
}

TEST(WalCaps, TheDefaultsSatisfyTheOrderingInvariantWithMargin) {
  EXPECT_TRUE(Caps().Ordered());
  EXPECT_EQ(kWalBufferBytes, 4 * kMaxRecordBytes);
}

// The tripwire HALTS. Unbounded growth in a fault-injected harness means an OOM
// kill, which is the worst possible failure signal because it destroys the run
// that would have explained it.
TEST(WalCaps, TheBufferTripwireHaltsInsteadOfGrowing) {
  TestEnvironment t;
  Caps small;
  small.max_record_bytes = 200;
  small.wal_buffer_bytes = 1000;
  // BACKPRESSURE OFF, AND IT IS A REGIME STATEMENT RATHER THAN A CONVENIENCE.
  // With the policy on, the tripwire is UNREACHABLE through Apply: kBusy fires
  // strictly earlier by Caps::Ordered()'s margin, so this test would assert
  // nothing and would be green for it. Turning the policy off is what puts the
  // tripwire back in reach of the thing that tests it.
  small.busy_bytes = 0;
  std::unique_ptr<Wal> w;
  ASSERT_TRUE(Wal::Open(t.env(), kDir, 1, small, &w).ok());

  int accepted = 0;
  Status last;
  for (int i = 0; i < 100; ++i) {
    last = w->Apply(static_cast<SeqNum>(i + 1), OneSet("key" + std::to_string(i), "v"));
    if (!last.ok()) break;
    ++accepted;
  }
  EXPECT_EQ(last.code(), Status::Code::kWalBufferFull);
  EXPECT_GT(accepted, 0);
  EXPECT_LT(accepted, 100);
}

TEST(WalCaps, AnOverCapRecordIsRefusedAndAppliesNothing) {
  TestEnvironment t;
  Caps small;
  small.max_record_bytes = 100;
  small.wal_buffer_bytes = 10000;
  // BACKPRESSURE OFF, AND IT IS A REGIME STATEMENT RATHER THAN A CONVENIENCE.
  // With the policy on, the tripwire is UNREACHABLE through Apply: kBusy fires
  // strictly earlier by Caps::Ordered()'s margin, so this test would assert
  // nothing and would be green for it. Turning the policy off is what puts the
  // tripwire back in reach of the thing that tests it.
  small.busy_bytes = 0;
  std::unique_ptr<Wal> w;
  ASSERT_TRUE(Wal::Open(t.env(), kDir, 1, small, &w).ok());
  const Status s = w->Apply(1, OneSet(std::string(200, 'k'), "v"));
  EXPECT_EQ(s.code(), Status::Code::kRecordTooLarge);
  SeqNum mark = 0;
  ASSERT_TRUE(w->Sync(&mark).ok());
  EXPECT_EQ(mark, 0u) << "the refused batch left something in the buffer";
}

// -------------------------------------------- adjudication, end to end
//
// The rig unit tests check the TABLE. These check the ENGINE against it: submit
// a batch, compute record_bytes from the harness's own record of what was
// submitted, ask the engine only whether it reported the error, and adjudicate.
// The engine's account of its own size arithmetic is never consulted.

rig::CapVerdict SubmitAndAdjudicate(Wal* w, SeqNum seq, std::size_t key_bytes,
                                    uint64_t cap, Status::Code cap_code) {
  const std::string key(key_bytes, 'k');
  const Status s = w->Apply(seq, OneSet(key, "v"));
  rig::SubmittedOp op;
  op.kind = rig::SubmittedOp::Kind::kSet;
  op.key_bytes = key_bytes;
  op.value_bytes = 1;
  const uint64_t harness = rig::HarnessRecordBytes({op});
  return rig::AdjudicateCap(harness, cap, s.code() == cap_code);
}

TEST(WalAdjudication, RecordCapBothDirections) {
  TestEnvironment t;
  Caps c;
  c.max_record_bytes = 100;
  c.wal_buffer_bytes = 100000;
  // BACKPRESSURE OFF, AND IT IS A REGIME STATEMENT RATHER THAN A CONVENIENCE.
  // With the policy on, the tripwire is UNREACHABLE through Apply: kBusy fires
  // strictly earlier by Caps::Ordered()'s margin, so this test would assert
  // nothing and would be green for it. Turning the policy off is what puts the
  // tripwire back in reach of the thing that tests it.
  c.busy_bytes = 0;
  std::unique_ptr<Wal> w;
  ASSERT_TRUE(Wal::Open(t.env(), kDir, 1, c, &w).ok());

  // Direction 1: legal input. The engine tripping here is a DIVERGENCE and the
  // run fails -- it does not void, because an engine that spuriously trips the
  // cap would delete the evidence of its own bug.
  EXPECT_EQ(SubmitAndAdjudicate(w.get(), 1, 50, c.max_record_bytes,
                                Status::Code::kRecordTooLarge),
            rig::CapVerdict::kNormal);

  // Direction 2: over-cap input. The engine accepting it is also a divergence.
  // Only a satisfied harness-side predicate WITH the matching error voids.
  EXPECT_EQ(SubmitAndAdjudicate(w.get(), 2, 500, c.max_record_bytes,
                                Status::Code::kRecordTooLarge),
            rig::CapVerdict::kVoid);
}

TEST(WalAdjudication, BufferCapBothDirections) {
  TestEnvironment t;
  Caps c;
  c.max_record_bytes = 200;
  c.wal_buffer_bytes = 400;
  // BACKPRESSURE OFF, AND IT IS A REGIME STATEMENT RATHER THAN A CONVENIENCE.
  // With the policy on, the tripwire is UNREACHABLE through Apply: kBusy fires
  // strictly earlier by Caps::Ordered()'s margin, so this test would assert
  // nothing and would be green for it. Turning the policy off is what puts the
  // tripwire back in reach of the thing that tests it.
  c.busy_bytes = 0;
  std::unique_ptr<Wal> w;
  ASSERT_TRUE(Wal::Open(t.env(), kDir, 1, c, &w).ok());

  // The harness tracks occupancy itself: the sum of record_bytes submitted
  // since the last Sync START.
  uint64_t occupancy = 0;
  auto submit = [&](SeqNum seq, std::size_t key_bytes) {
    const std::string key(key_bytes, 'k');
    const Status s = w->Apply(seq, OneSet(key, "v"));
    rig::SubmittedOp op;
    op.kind = rig::SubmittedOp::Kind::kSet;
    op.key_bytes = key_bytes;
    op.value_bytes = 1;
    occupancy += rig::HarnessRecordBytes({op});
    return rig::AdjudicateCap(occupancy, c.wal_buffer_bytes,
                              s.code() == Status::Code::kWalBufferFull);
  };

  // Each of these is 123 bytes by the frozen formula: 13 + (1 + 4 + 100) +
  // (4 + 1). Three fit under 400; the fourth does not.
  EXPECT_EQ(submit(1, 100), rig::CapVerdict::kNormal);
  EXPECT_EQ(submit(2, 100), rig::CapVerdict::kNormal);
  EXPECT_EQ(submit(3, 100), rig::CapVerdict::kNormal);
  EXPECT_EQ(submit(4, 100), rig::CapVerdict::kVoid);
  EXPECT_GT(occupancy, c.wal_buffer_bytes) << "the harness's own occupancy "
                                              "never crossed the cap, so the "
                                              "assertion above tested nothing";
}

// ---------------------------------------------------------- directory sync

TEST(Wal, OpenMakesTheDirectoryEntryDurableBeforeReturning) {
  TestEnvironment t;
  std::unique_ptr<Wal> w;
  ASSERT_TRUE(Wal::Open(t.env(), kDir, 1, Caps(), &w).ok());
  t.Kill();
  const testenv::DurableImage image = t.Image();
  EXPECT_EQ(image.count(kLog), 1u)
      << "the WAL's bytes were fsynced and its NAME was not, so a kill takes "
         "the whole file -- and section 7.2's gapless-file-number check is what "
         "turns that loss into a failed open instead of silence";
}

}  // namespace
}  // namespace wal
}  // namespace basalt
