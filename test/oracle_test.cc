// B1.9a: the exactness oracle and the two-element verdict.
//
// Every assertion here is against the harness's own record. The oracle itself
// includes nothing from engine-cpp/src -- cpp-scan enforces that -- so the
// closest it comes to the engine is a std::map somebody else extracted.
#include "exactness_oracle.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "call_site.h"
#include "db.h"
#include "test_env.h"

namespace rift {
namespace rig {
namespace {

using testenv::TestEnvironment;

const std::string kDir = "db";

// ---------------------------------------------------------------------------
// The adapter. THE ONLY ENGINE-FACING CODE IN THIS FILE, kept separate from the
// oracle so that "the oracle asks the engine nothing" is a property of a file
// rather than of a habit.
std::map<std::string, std::string> ExtractState(const DB& db) {
  std::map<std::string, std::string> out;
  std::unique_ptr<Iterator> it = db.NewIter(IterOptions());
  for (bool ok = it->First(); ok; ok = it->Next()) {
    out[it->Key().ToString()] = it->Value().ToString();
  }
  EXPECT_TRUE(it->Close().ok());
  return out;
}

// Summarizes TestEnv's ledger. Also harness-side, and also not the oracle.
LedgerFacts FactsFrom(const TestEnvironment& t, bool sync_in_flight) {
  LedgerFacts f;
  f.sync_in_flight = sync_in_flight;
  for (const testenv::LedgerEntry& e : t.ledger()) {
    if (e.site == CallSite::kWritableFileSync) {
      f.in_flight_durability_applied = e.promoted;
    }
  }
  return f;
}

// Drives a DB and the rig's own log together, so the log is a record of what
// the RIG issued rather than a copy of what the engine did.
struct Driver {
  TestEnvironment* env;
  DB* db;
  SubmissionLog log;

  void Put(const std::string& k, const std::string& v) {
    WriteBatch b;
    b.Set(Slice(k), Slice(v));
    wal::SeqNum engine_seq = 0;
    const Status s = db->Write(b, &engine_seq);
    EXPECT_TRUE(s.ok()) << s.ToString();
    std::vector<RefChange> changes;
    RefChange c;
    c.key = k;
    c.value = v;
    c.present = true;
    changes.push_back(c);
    const OracleSeq mine = log.NoteWrite(changes);
    // THE ENGINE'S SEQUENCE IS COMPARED, NOT TRUSTED. B1-D10 aligned the two
    // sequence spaces precisely so no translation table is needed; a divergence
    // here means the alignment broke, and a rig that adapted to it would be
    // hiding the thing the alignment exists to guarantee.
    EXPECT_EQ(static_cast<OracleSeq>(engine_seq), mine)
        << "the engine's sequence space and the rig's have diverged";
  }

  // Returns true if the Sync RETURNED SUCCESSFULLY.
  bool Sync() {
    log.NoteSyncStart();
    wal::SeqNum mark = 0;
    const Status s = db->Sync(&mark);
    if (s.code() == Status::Code::kKilled) {
      // THE PROCESS IS CONCEPTUALLY GONE. The caller never got an answer, so
      // the Sync stays IN FLIGHT -- which is the state the two-element set
      // exists for. Nothing is polled: a dead engine's opinion of its own
      // watermark is not a promise anyone received.
      return false;
    }
    if (!s.ok()) {
      // It returned, unsuccessfully. NOT in flight -- the caller got an answer,
      // and the answer was no.
      log.NoteSyncFailed();
      PollDurable();
      return false;
    }
    log.NoteSyncReturned(static_cast<OracleSeq>(mark));
    PollDurable();
    return true;
  }

  // DurableSeq() is a durability promise like any other, so the rig records
  // every value it is ever told and holds the engine to the highest.
  void PollDurable() {
    log.NoteDurableSeq(static_cast<OracleSeq>(db->DurableSeq()));
  }
};

// ---------------------------------------------------------------------------
// Oracle unit tests: synthetic logs, no engine at all.

SubmissionLog TwoGroupsWithAMultiBatchSecond() {
  SubmissionLog log;
  auto set = [](const std::string& k, const std::string& v) {
    RefChange c;
    c.key = k;
    c.value = v;
    c.present = true;
    return std::vector<RefChange>{c};
  };
  log.NoteSyncStart();
  log.NoteWrite(set("a", "1"));
  log.NoteSyncReturned(1);          // group 1 closed at seq 1
  log.NoteSyncStart();
  log.NoteWrite(set("b", "2"));     // seq 2
  log.NoteWrite(set("c", "3"));     // seq 3  -- an intermediate boundary
  log.NoteWrite(set("d", "4"));     // seq 4
  // and no return: this Sync is in flight
  return log;
}

TEST(Oracle, MatchingThePreviousGroupIsAPassThatNamesItsElement) {
  const SubmissionLog log = TwoGroupsWithAMultiBatchSecond();
  LedgerFacts f;
  f.sync_in_flight = true;
  f.in_flight_durability_applied = false;
  const RecoveryVerdict v = Adjudicate(log, f, log.StateAt(1));
  EXPECT_EQ(v.outcome, RunOutcome::kContractPass);
  EXPECT_EQ(v.matched, MatchedElement::kPreviousGroup);
  EXPECT_EQ(v.seq, 1u);
  EXPECT_EQ(v.compared, 1u);
}

TEST(Oracle, MatchingTheInFlightGroupIsAPassThatNamesItsElement) {
  const SubmissionLog log = TwoGroupsWithAMultiBatchSecond();
  LedgerFacts f;
  f.sync_in_flight = true;
  f.in_flight_durability_applied = true;
  const RecoveryVerdict v = Adjudicate(log, f, log.StateAt(4));
  EXPECT_EQ(v.outcome, RunOutcome::kContractPass);
  EXPECT_EQ(v.matched, MatchedElement::kInFlightGroup);
  EXPECT_EQ(v.seq, 4u);
}

// THE SET IS TWO ELEMENTS WIDE AND NOT THE GROUP'S WIDTH. This is what BM15
// blinds: an oracle that accepted any batch boundary inside the in-flight group
// would stop checking the thing GROUP_END exists to make checkable -- that a
// group commits WHOLE OR NOT AT ALL.
TEST(Oracle, AnIntermediateBatchBoundaryInsideTheGroupIsRejected) {
  const SubmissionLog log = TwoGroupsWithAMultiBatchSecond();
  ASSERT_EQ(log.write_sequences().size(), 4u);
  LedgerFacts f;
  f.sync_in_flight = true;
  f.in_flight_durability_applied = true;
  for (OracleSeq intermediate : {2u, 3u}) {
    const RecoveryVerdict v = Adjudicate(log, f, log.StateAt(intermediate));
    EXPECT_EQ(v.outcome, RunOutcome::kContractViolation)
        << "the oracle accepted seq " << intermediate
        << ", a batch boundary strictly inside the in-flight group";
    EXPECT_EQ(v.matched, MatchedElement::kNone);
  }
}

// A verdict that cannot say which element it matched is a failure OF THE ORACLE,
// not a pass of the engine.
TEST(Oracle, APassAlwaysNamesAnElementAndAViolationNamesNone) {
  const SubmissionLog log = TwoGroupsWithAMultiBatchSecond();
  LedgerFacts f;
  f.sync_in_flight = true;
  f.in_flight_durability_applied = true;
  for (OracleSeq r : {1u, 2u, 3u, 4u}) {
    const RecoveryVerdict v = Adjudicate(log, f, log.StateAt(r));
    if (v.outcome == RunOutcome::kContractPass) {
      EXPECT_NE(v.matched, MatchedElement::kNone)
          << "a pass with no named element is an oracle that has stopped "
             "distinguishing the two cases it exists to distinguish";
    } else {
      EXPECT_EQ(v.matched, MatchedElement::kNone);
      EXPECT_FALSE(v.why.empty()) << "a violation must say why";
    }
  }
}

// (ii) NO OVER-PROMISE, and this is the assertion the whole rig exists for.
TEST(Oracle, AWatermarkAboveTheRecoveryPointIsAViolation) {
  SubmissionLog log;
  RefChange c;
  c.key = "a";
  c.value = "1";
  c.present = true;
  log.NoteSyncStart();
  log.NoteWrite({c});
  log.NoteSyncReturned(1);
  log.NoteSyncStart();
  RefChange d;
  d.key = "b";
  d.value = "2";
  d.present = true;
  log.NoteWrite({d});
  // The engine claimed 2 was durable and then the kill landed with only group 1
  // on disk. The rig holds it to what it said.
  log.NoteSyncReturned(2);
  log.NoteSyncStart();

  LedgerFacts f;
  f.sync_in_flight = true;
  f.in_flight_durability_applied = false;
  const RecoveryVerdict v = Adjudicate(log, f, log.StateAt(1));
  EXPECT_EQ(v.outcome, RunOutcome::kContractViolation);
  EXPECT_NE(v.why.find("over-promise"), std::string::npos) << v.why;
}

// ---------------------------------------------------------------------------
// The two elements, individually induced against the real engine.
//
// Section 7.4 condition 3: both are induced BY NAMED TESTS, because a
// two-element set where only one element has ever been observed is a
// one-element contract with a spare excuse attached.

uint64_t LastSyncOrdinal(const TestEnvironment& t) {
  uint64_t o = 0;
  for (const testenv::LedgerEntry& e : t.ledger()) {
    if (e.site == CallSite::kWritableFileSync) o = e.ordinal;
  }
  return o;
}

// Runs the workload once with no faults to discover the ordinal of the second
// WritableFile::Sync. Hand-counted ordinals go stale the first time a workload
// changes and take the test's meaning with them.
uint64_t SecondSyncOrdinal() {
  TestEnvironment probe;
  std::unique_ptr<DB> db;
  EXPECT_TRUE(DB::Open(probe.env(), kDir, wal::Caps(), &db).ok());
  Driver d{&probe, db.get(), {}};
  d.Put("a", "1");
  EXPECT_TRUE(d.Sync());
  d.Put("b", "2");
  EXPECT_TRUE(d.Sync());
  return LastSyncOrdinal(probe);
}

RecoveryVerdict RunWithFaultAtSecondSync(testenv::Injection injection,
                                         uint64_t prefix, bool* returned) {
  testenv::FaultPlan plan;
  plan.At(SecondSyncOrdinal(), injection, prefix);
  TestEnvironment t(plan);
  SubmissionLog log;
  {
    std::unique_ptr<DB> db;
    EXPECT_TRUE(DB::Open(t.env(), kDir, wal::Caps(), &db).ok());
    Driver d{&t, db.get(), {}};
    d.Put("a", "1");
    EXPECT_TRUE(d.Sync());
    d.Put("b", "2");
    *returned = d.Sync();
    log = d.log;
  }
  EXPECT_TRUE(t.dead());
  const LedgerFacts facts = FactsFrom(t, !*returned);

  std::unique_ptr<TestEnvironment> re =
      TestEnvironment::FromImage(t.Image(), testenv::FaultPlan());
  std::unique_ptr<DB> db;
  EXPECT_TRUE(DB::Open(re->env(), kDir, wal::Caps(), &db).ok());
  return Adjudicate(log, facts, ExtractState(*db));
}

TEST(Oracle, RecoveryLandsOnPreviousGroupWhenSyncIsTorn) {
  bool returned = true;
  // A kill INSIDE Sync with nothing promoted: the group never closed.
  const RecoveryVerdict v =
      RunWithFaultAtSecondSync(testenv::Injection::kTornSync, 0, &returned);
  EXPECT_FALSE(returned) << "the torn Sync returned; there is no in-flight case";
  EXPECT_EQ(v.outcome, RunOutcome::kContractPass) << v.why;
  EXPECT_EQ(v.matched, MatchedElement::kPreviousGroup);
  EXPECT_EQ(v.seq, 1u);
}

TEST(Oracle, RecoveryLandsOnInFlightGroupWhenSyncCompletesButIsPreempted) {
  bool returned = true;
  // The Sync SUCCEEDS on the device and the process dies before the answer gets
  // home. The bytes are durable; the caller never learned it. No design removes
  // this -- it is "did the RPC commit?", one layer down.
  const RecoveryVerdict v =
      RunWithFaultAtSecondSync(testenv::Injection::kKillAfterEffect, 0, &returned);
  EXPECT_FALSE(returned) << "the preempted Sync returned; the case was not induced";
  EXPECT_EQ(v.outcome, RunOutcome::kContractPass) << v.why;
  EXPECT_EQ(v.matched, MatchedElement::kInFlightGroup);
  EXPECT_EQ(v.seq, 2u);
}

// SECTION 7.4 CONDITION 3, DEMONSTRATED RATHER THAN ASSUMED.
//
// HARNESS-006 is what made this unreachable: a prefix-granular torn Sync was
// classified as exactness-suspending, so a run exercising the G_{k-1} element
// was structurally uncountable as evidence and "both elements observed" could
// never be satisfied. The fix is not assumed to have restored it -- this asserts
// that both elements are observed AND that both runs are bankable.
TEST(Oracle, BothElementsAreObservedAndBothRunsCountAsEvidence) {
  std::vector<MatchedElement> seen;
  for (auto injection : {testenv::Injection::kTornSync,
                         testenv::Injection::kKillAfterEffect}) {
    testenv::FaultPlan plan;
    plan.At(SecondSyncOrdinal(), injection, 0);
    TestEnvironment t(plan);
    SubmissionLog log;
    bool returned = true;
    {
      std::unique_ptr<DB> db;
      ASSERT_TRUE(DB::Open(t.env(), kDir, wal::Caps(), &db).ok());
      Driver d{&t, db.get(), {}};
      d.Put("a", "1");
      ASSERT_TRUE(d.Sync());
      d.Put("b", "2");
      returned = d.Sync();
      log = d.log;
    }
    // THE RUN MUST BE BANKABLE. If either injector suspended exactness, this
    // run could never be counted and the condition below would be unsatisfiable
    // no matter how many times both elements appeared.
    ASSERT_FALSE(t.exactness_suspended())
        << "a run exercising an element of the recovery set was marked "
           "characterization-only, so 'both elements observed' can never be met";
    ASSERT_TRUE(CountsAsRecoveryEvidence(OutcomeFloor(t.exactness_suspended())));

    std::unique_ptr<TestEnvironment> re =
        TestEnvironment::FromImage(t.Image(), testenv::FaultPlan());
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(re->env(), kDir, wal::Caps(), &db).ok());
    const RecoveryVerdict v =
        Adjudicate(log, FactsFrom(t, !returned), ExtractState(*db));
    ASSERT_EQ(v.outcome, RunOutcome::kContractPass) << v.why;
    seen.push_back(v.matched);
  }
  ASSERT_EQ(seen.size(), 2u);
  EXPECT_NE(seen[0], seen[1])
      << "both runs landed on the same element, so the set has degenerated to "
         "one value with a spare excuse attached";
  EXPECT_TRUE(seen[0] == MatchedElement::kPreviousGroup ||
              seen[1] == MatchedElement::kPreviousGroup);
  EXPECT_TRUE(seen[0] == MatchedElement::kInFlightGroup ||
              seen[1] == MatchedElement::kInFlightGroup);
}

// AN FSYNC THAT FAILED MUST NOT ADVANCE THE WATERMARK, and DurableSeq() is
// where that becomes observable.
//
// BM1 survived its first induction because the rig only ever learned a
// watermark from a Sync's RETURN, and a killed Sync returns nothing -- so an
// engine that advanced the watermark before writing anything was invisible: the
// early value died with the process. The same shape as every other survival,
// which is that the test never created the situation it was checking.
//
// This creates it. The Sync fails with an IO error and the process LIVES, so
// the rig can ask DurableSeq() what the engine believes -- and hold it to the
// answer.
TEST(Oracle, AFailedFsyncMustNotAdvanceTheWatermark) {
  uint64_t second_sync = SecondSyncOrdinal();
  testenv::FaultPlan plan;
  plan.At(second_sync, testenv::Injection::kIoError);
  TestEnvironment t(plan);
  SubmissionLog log;
  {
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(t.env(), kDir, wal::Caps(), &db).ok());
    Driver d{&t, db.get(), {}};
    d.Put("a", "1");
    ASSERT_TRUE(d.Sync());
    d.Put("b", "2");
    EXPECT_FALSE(d.Sync()) << "the injected IO error did not reach the caller";
    log = d.log;
  }
  EXPECT_EQ(log.highest_returned_watermark(), 1u)
      << "the engine reported a watermark of "
      << log.highest_returned_watermark()
      << " after an fsync that failed: it has promised durability for bytes no "
         "device accepted";

  t.Kill();
  std::unique_ptr<TestEnvironment> re =
      TestEnvironment::FromImage(t.Image(), testenv::FaultPlan());
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(re->env(), kDir, wal::Caps(), &db).ok());
  LedgerFacts f;
  f.sync_in_flight = false;
  f.in_flight_durability_applied = false;
  const RecoveryVerdict v = Adjudicate(log, f, ExtractState(*db));
  EXPECT_EQ(v.outcome, RunOutcome::kContractPass) << v.why;
  EXPECT_EQ(v.seq, 1u);
}

}  // namespace
}  // namespace rig
}  // namespace rift
