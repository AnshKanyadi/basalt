// B5.3: the poller rig, and kBusy in BOTH directions.
//
// Section 7.6.1's binding, discharged. Everything here computes `owed` from
// what this file submitted and what its own Sync calls drained -- the engine is
// asked nothing, and its answer is held to that arithmetic rather than
// believed.
#include "poller.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cap_adjudication.h"
#include "db.h"
#include "run_outcome.h"
#include "test_env.h"

namespace rift {
namespace rig {
namespace {

using testenv::TestEnvironment;
using wal::Caps;
using wal::SeqNum;

// ------------------------------------------------- the adjudicator, in the
// ------------------------------------------------- pure, both directions

TEST(Poller, BothDivergencesFailTheRunAndNeitherIsVoided) {
  // Under the threshold and accepted: normal, bankable.
  EXPECT_EQ(BusyVerdict::kNormal, AdjudicateBusy(100, 1000, false));
  // Over and refused: the policy WORKING, and still bankable.
  EXPECT_EQ(BusyVerdict::kBackpressured, AdjudicateBusy(2000, 1000, true));
  // Under and refused: the engine signalled backpressure nobody owes.
  EXPECT_EQ(BusyVerdict::kSpuriousBusy, AdjudicateBusy(100, 1000, true));
  // Over and accepted: the engine took a write it was supposed to refuse.
  EXPECT_EQ(BusyVerdict::kMissingBusy, AdjudicateBusy(2000, 1000, false));

  EXPECT_FALSE(IsBusyDivergence(BusyVerdict::kNormal));
  EXPECT_FALSE(IsBusyDivergence(BusyVerdict::kBackpressured));
  EXPECT_TRUE(IsBusyDivergence(BusyVerdict::kSpuriousBusy));
  EXPECT_TRUE(IsBusyDivergence(BusyVerdict::kMissingBusy));

  // AND THE OUTCOMES, which is the half HARNESS-006 found untested three times.
  // The conservative direction is the one nothing notices: voiding a
  // backpressured run would lose exactly the evidence this rig produces, and
  // every lane would stay green while the number never appeared.
  EXPECT_EQ(RunOutcome::kContractPass, OutcomeForBusyVerdict(BusyVerdict::kNormal));
  EXPECT_EQ(RunOutcome::kContractPass, OutcomeForBusyVerdict(BusyVerdict::kBackpressured));
  EXPECT_EQ(RunOutcome::kContractViolation, OutcomeForBusyVerdict(BusyVerdict::kSpuriousBusy));
  EXPECT_EQ(RunOutcome::kContractViolation, OutcomeForBusyVerdict(BusyVerdict::kMissingBusy));
}

TEST(Poller, TheBoundaryValueItselfIsNotOwed) {
  // EXACTLY AT the threshold is under it, on both sides of the comparison. An
  // off-by-one between the harness's arithmetic and the engine's would present
  // as a divergence at precisely one occupancy and nowhere else, which is the
  // hardest shape there is to attribute.
  EXPECT_EQ(BusyVerdict::kNormal, AdjudicateBusy(1000, 1000, false));
  EXPECT_EQ(BusyVerdict::kSpuriousBusy, AdjudicateBusy(1000, 1000, true));
  EXPECT_EQ(BusyVerdict::kMissingBusy, AdjudicateBusy(1001, 1000, false));
}

TEST(Poller, DisabledMeansNothingIsEverOwed) {
  // busy_bytes == 0 is a REGIME, not a hole: it is what the WAL tripwire's own
  // tests run at, because with the policy on the tripwire is unreachable.
  EXPECT_EQ(BusyVerdict::kNormal, AdjudicateBusy(1ull << 40, 0, false));
  EXPECT_EQ(BusyVerdict::kSpuriousBusy, AdjudicateBusy(1ull << 40, 0, true));
}

// ------------------------------------------------- the rig, driving a real Db

// THE RECORD IS THE RIG'S OWN. Nothing below reads an engine counter.
class DrivenPoller {
 public:
  DrivenPoller(DB* db, uint64_t busy_bytes) : db_(db), busy_bytes_(busy_bytes) {}

  uint64_t owed() const { return submitted_ - drained_; }

  // Submit one Set and adjudicate the answer. Returns the verdict so a caller
  // can assert which direction it got rather than only that it did not diverge.
  BusyVerdict Put(const std::string& key, const std::string& value) {
    std::vector<SubmittedOp> ops(1);
    ops[0].kind = SubmittedOp::Kind::kSet;
    ops[0].key_bytes = key.size();
    ops[0].value_bytes = value.size();
    const uint64_t bytes = HarnessRecordBytes(ops);

    WriteBatch b;
    b.Set(Slice(key), Slice(value));
    SeqNum seq = 0;
    const Status s = db_->Write(b, &seq);

    const bool reported = s.code() == Status::Code::kBusy;
    const BusyVerdict v = AdjudicateBusy(owed() + bytes, busy_bytes_, reported);
    if (!reported) {
      // ACCEPTED WRITES ONLY. A refused write was not applied, so charging it
      // would make the rig's arithmetic drift from the engine's by exactly the
      // batches the engine correctly rejected -- and the drift would look like
      // a missing-busy divergence one write later.
      EXPECT_TRUE(s.ok()) << "unexpected engine error: " << CodeName(s.code());
      submitted_ += bytes;
    }
    return v;
  }

  // One poller cycle. The rig decides WHEN this happens; that is the whole of
  // what "drives" means.
  void Drain() {
    const uint64_t at_start = submitted_;
    SeqNum w = 0;
    ASSERT_TRUE(db_->Sync(&w).ok());
    // SNAPSHOTTED AT START, APPLIED AT COMPLETION. Anything submitted while the
    // Sync was in flight is NOT covered by it, and crediting it would hide the
    // exact window this rig exists to reach.
    drained_ = at_start;
  }

 private:
  DB* db_;
  uint64_t busy_bytes_;
  uint64_t submitted_ = 0;
  uint64_t drained_ = 0;
};

wal::Caps BusyCaps(uint64_t busy) {
  Caps c;
  c.busy_bytes = busy;
  return c;
}

// DIRECTION 2, CONSTRUCTED: the rig withholds the poller until backpressure is
// unambiguously owed, then checks that it was signalled. This is the direction
// a rig that only watched a poller could never reach, and it is the whole
// reason B1-Q11 ruled the rig a driver.
TEST(Poller, BackpressureIsOwedAndSignalledWhenThePollerIsWithheld) {
  TestEnvironment env;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(env.env(), "busy", BusyCaps(4096), &db).ok());
  DrivenPoller p(db.get(), 4096);

  bool ever_backpressured = false;
  for (int i = 0; i < 200; i++) {
    const BusyVerdict v = p.Put("key" + std::to_string(i), std::string(64, 'v'));
    ASSERT_FALSE(IsBusyDivergence(v))
        << "at write " << i << ": " << BusyVerdictName(v) << ", owed " << p.owed();
    if (v == BusyVerdict::kBackpressured) ever_backpressured = true;
  }
  ASSERT_TRUE(ever_backpressured)
      << "the threshold was never crossed, so this test asserted nothing -- "
         "GF-16: a workload that cannot reach its precondition is green for "
         "the wrong reason";

  // AND IT CLEARS. A policy that latches is not backpressure, it is a broken
  // database; nothing above would notice the difference.
  p.Drain();
  EXPECT_EQ(0u, p.owed());
  EXPECT_EQ(BusyVerdict::kNormal, p.Put("after", std::string(64, 'v')));
}

// THE ENGINE'S OWN COMPARISON, AT THE BOUNDARY VALUE, DRIVEN.
//
// Poller.TheBoundaryValueItselfIsNotOwed above asserts the same edge on
// AdjudicateBusy, and BM118 survived it -- correctly. That test exercises the
// HARNESS's arithmetic, and the mutant changes the ENGINE's. Two
// implementations of one comparison exist precisely so they can be checked
// against each other, and a test of one of them says nothing about the other:
//
//   A SECOND IMPLEMENTATION IS ONLY A CHECK WHEN SOMETHING RUNS BOTH.
//
// So this test picks a threshold that is an exact multiple of one write's cost
// and drives real writes onto it. Write k leaves the backlog at exactly
// busy_bytes: legal under `>`, refused under `>=`, and nothing else in the
// suite can tell those apart.
TEST(Poller, TheEngineAcceptsTheWriteThatLandsExactlyOnTheThreshold) {
  std::vector<SubmittedOp> one(1);
  one[0].kind = SubmittedOp::Kind::kSet;
  one[0].key_bytes = 8;    // "k0000000"
  one[0].value_bytes = 64;
  const uint64_t per_write = HarnessRecordBytes(one);
  const int k = 20;

  TestEnvironment env;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(env.env(), "edge", BusyCaps(per_write * k), &db).ok());
  DrivenPoller p(db.get(), per_write * k);

  char key[9];
  for (int i = 1; i <= k; i++) {
    std::snprintf(key, sizeof(key), "k%07d", i);
    const BusyVerdict v = p.Put(key, std::string(64, 'v'));
    ASSERT_EQ(BusyVerdict::kNormal, v)
        << "write " << i << " of " << k << " (" << BusyVerdictName(v)
        << "); the backlog reaches the threshold EXACTLY on the last one, which "
           "is legal -- the engine must compare with > and not >=";
  }
  ASSERT_EQ(per_write * k, p.owed()) << "the arithmetic did not land on the boundary, "
                                        "so this test asserted nothing";

  // AND ONE BYTE PAST IT IS REFUSED, which is what makes the line above an
  // assertion about the edge rather than about the threshold being large.
  std::snprintf(key, sizeof(key), "k%07d", k + 1);
  EXPECT_EQ(BusyVerdict::kBackpressured, p.Put(key, std::string(64, 'v')));
}

// THE IN-FLIGHT HALF, which is only reachable from INSIDE a Sync.
//
// Wal::Sync zeroes buffered_bytes_ at the swap and then does its I/O with the
// mutex released. For that whole window the bytes are resident, undrained, and
// counted by no buffer counter -- so an engine charging only `buffered` would
// answer kOk here while this rig's record says the backlog is owed.
//
// The promotion hook fires inside file_->Sync(), before Wal::Sync returns and
// before the in-flight charge is released. That is the window, and reaching it
// through the hook keeps this rig SINGLE-THREADED: a second thread would have
// been the obvious way and would have made the moment a race rather than a
// place.
struct InFlightProbe {
  DB* db = nullptr;
  DrivenPoller* poller = nullptr;
  int fired = 0;
  BusyVerdict verdict = BusyVerdict::kNormal;
  uint64_t owed_at_probe = 0;
};

void ProbeDuringSync(void* ctx, const testenv::DurableImage&) {
  auto* p = static_cast<InFlightProbe*>(ctx);
  if (p->fired++ != 0) return;  // the first promotion only
  p->owed_at_probe = p->poller->owed();
  p->verdict = p->poller->Put("during-sync", std::string(64, 'v'));
}

TEST(Poller, BackpressureIsOwedForBytesStillInFlightInsideASync) {
  TestEnvironment env;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(env.env(), "inflight", BusyCaps(4096), &db).ok());
  DrivenPoller p(db.get(), 4096);

  // Fill to just under the threshold, so the one write issued from inside the
  // Sync is what crosses it -- and it can only cross it if the bytes handed to
  // that very Sync are still being charged.
  int i = 0;
  while (p.owed() + 128 < 4096) {
    ASSERT_FALSE(IsBusyDivergence(p.Put("k" + std::to_string(i++), std::string(64, 'v'))));
  }
  const uint64_t before = p.owed();
  ASSERT_GT(before, 0u);

  InFlightProbe probe{db.get(), &p, 0, BusyVerdict::kNormal, 0};
  env.set_promotion_hook(&ProbeDuringSync, &probe);
  p.Drain();

  ASSERT_EQ(1, probe.fired) << "the hook never fired, so the window was never entered";
  EXPECT_EQ(before, probe.owed_at_probe)
      << "the rig's own record must still show the backlog owed while the Sync "
         "covering it is in flight; if this is 0 the RIG is wrong, not the engine";
  EXPECT_FALSE(IsBusyDivergence(probe.verdict))
      << "inside the Sync: " << BusyVerdictName(probe.verdict);
}

}  // namespace
}  // namespace rig
}  // namespace rift
