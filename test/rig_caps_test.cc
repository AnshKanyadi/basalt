// The cap adjudication and the regime key: the harness deciding, not the engine.
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cap_adjudication.h"
#include "basalt/caps.h"
#include "basalt/format.h"
#include "regime.h"
#include "run_outcome.h"

namespace basalt {
namespace rig {
namespace {

std::vector<SubmittedOp> Sets(int n, std::size_t key_bytes) {
  std::vector<SubmittedOp> ops;
  for (int i = 0; i < n; ++i) {
    SubmittedOp op;
    op.kind = SubmittedOp::Kind::kSet;
    op.key_bytes = key_bytes;
    op.value_bytes = 1;
    ops.push_back(op);
  }
  return ops;
}

// The harness's formula and the engine's must agree on the frozen formula, and
// they are DIFFERENT IMPLEMENTATIONS on purpose. This test is the only place
// they are ever compared -- everywhere else the harness uses its own, because
// an oracle that called the engine's would be asking the engine whether the
// engine was right.
TEST(CapAdjudication, TheTwoIndependentFormulasAgree) {
  for (int n : {0, 1, 5, 100}) {
    for (std::size_t k : {std::size_t(0), std::size_t(1), std::size_t(50)}) {
      std::vector<wal::Op> engine_ops;
      const std::string key(k, 'k');
      const std::string value(1, 'v');
      for (int i = 0; i < n; ++i) {
        wal::Op op;
        op.kind = wal::OpKind::kSet;
        op.key = Slice(key);
        op.value = Slice(value);
        engine_ops.push_back(op);
      }
      EXPECT_EQ(HarnessRecordBytes(Sets(n, k)), wal::BatchRecordBytes(engine_ops))
          << "harness and engine disagree about what the cap MEANS, at n=" << n
          << " keylen=" << k;
    }
  }
}

// The derivation in caps.h, checked rather than believed: roughly 1.22 million
// 50-byte point deletes fit under the default record cap.
TEST(CapAdjudication, TheRecordCapDerivationHolds) {
  std::vector<SubmittedOp> deletes;
  SubmittedOp d;
  d.kind = SubmittedOp::Kind::kDelete;
  d.key_bytes = 50;
  deletes.assign(1220000, d);
  EXPECT_LE(HarnessRecordBytes(deletes), wal::kMaxRecordBytes);
  deletes.assign(1230000, d);
  EXPECT_GT(HarnessRecordBytes(deletes), wal::kMaxRecordBytes);
}

TEST(CapAdjudication, AllFourCellsOfTheTable) {
  EXPECT_EQ(AdjudicateCap(50, 100, false), CapVerdict::kNormal);
  EXPECT_EQ(AdjudicateCap(50, 100, true), CapVerdict::kSpuriousTripwire);
  EXPECT_EQ(AdjudicateCap(150, 100, false), CapVerdict::kMissingTripwire);
  EXPECT_EQ(AdjudicateCap(150, 100, true), CapVerdict::kVoid);
}

// BOTH DIVERGENCE DIRECTIONS FAIL THE RUN. Neither voids it: a void is a
// legitimate engine error, a divergence is a bug, and collapsing them is
// exactly the escape hatch B1-D8 was overruled for.
TEST(CapAdjudication, BothDivergencesFailTheRunAndNeitherVoidsIt) {
  EXPECT_TRUE(IsDivergence(CapVerdict::kSpuriousTripwire));
  EXPECT_TRUE(IsDivergence(CapVerdict::kMissingTripwire));
  // AND THE OTHER DIRECTION, which the audit after HARNESS-006 found missing.
  // A classifier that decides whether a run counts must be asserted BOTH ways:
  // the safe-looking direction is the one no other assertion notices, because
  // its failure mode is a gate nothing can satisfy rather than a test that goes
  // red.
  EXPECT_FALSE(IsDivergence(CapVerdict::kNormal));
  EXPECT_FALSE(IsDivergence(CapVerdict::kVoid));
  EXPECT_EQ(OutcomeForCapVerdict(CapVerdict::kNormal), RunOutcome::kContractPass)
      << "a normal run that is not banked as a pass is a run that can never be "
         "banked at all, and nothing else here would say so";
  EXPECT_TRUE(CountsAsRecoveryEvidence(OutcomeForCapVerdict(CapVerdict::kNormal)));
  EXPECT_EQ(OutcomeForCapVerdict(CapVerdict::kSpuriousTripwire),
            RunOutcome::kContractViolation);
  EXPECT_EQ(OutcomeForCapVerdict(CapVerdict::kMissingTripwire),
            RunOutcome::kContractViolation);
  EXPECT_EQ(OutcomeForCapVerdict(CapVerdict::kVoid), RunOutcome::kVoid);
  EXPECT_FALSE(CountsAsRecoveryEvidence(RunOutcome::kVoid));
}

// ------------------------------------------------------------- the regime

// EVERY CAP, AND THAT IS THE POINT OF THE LOOP RATHER THAN THREE LINES. This
// test named two fields while there were two; B2 added a third and the test
// would have kept passing while a whole regime aggregated with the wrong one.
// A field added to Caps without a line here fails at the count below.
TEST(Regime, IsComputedFromTheActualCapValues) {
  RunRecord r;
  EXPECT_EQ(r.regime(), Regime::kDefault);

  int fields = 0;
  {
    RunRecord x;
    x.caps.max_record_bytes = 1024;
    EXPECT_EQ(x.regime(), Regime::kNonDefault) << "max_record_bytes";
    ++fields;
  }
  {
    RunRecord x;
    x.caps.wal_buffer_bytes = 4096;
    EXPECT_EQ(x.regime(), Regime::kNonDefault) << "wal_buffer_bytes";
    ++fields;
  }
  {
    RunRecord x;
    x.caps.flush_bytes = 8192;
    EXPECT_EQ(x.regime(), Regime::kNonDefault) << "flush_bytes";
    ++fields;
  }
  {
    // B5.3's backpressure threshold. Zero is DISABLED and is a regime in its
    // own right -- the WAL tripwire's tests run there, because with the policy
    // on the tripwire cannot be reached -- so a run with it off must never
    // aggregate with one that had it on.
    RunRecord x;
    x.caps.busy_bytes = 0;
    EXPECT_EQ(x.regime(), Regime::kNonDefault) << "busy_bytes";
    ++fields;
  }
  // sizeof is the crudest possible proxy and it is deliberate: it is the one
  // thing that changes when a field is added and cannot be kept in step by
  // accident. A new cap makes this fail, and the failure names the reason.
  EXPECT_EQ(sizeof(wal::Caps), static_cast<std::size_t>(fields) * sizeof(uint64_t))
      << "a cap was added to wal::Caps and this test still checks " << fields
      << " of them; a regime that ignores a cap aggregates two regimes silently";
}

TEST(Regime, RunsOfOneRegimeAggregate) {
  std::vector<RunRecord> rows(3);
  rows[1].outcome = RunOutcome::kVoid;
  rows[2].outcome = RunOutcome::kCharacterizationOnly;
  Tally t;
  ASSERT_TRUE(AggregateRuns(rows, &t));
  EXPECT_EQ(t.regime, Regime::kDefault);
  EXPECT_EQ(t.pass, 1u);
  EXPECT_EQ(t.voided, 1u);
  EXPECT_EQ(t.characterization, 1u);
  EXPECT_EQ(t.total(), 3u);
}

// REGIMES NEVER AGGREGATE. A tripwire observed firing at a lowered cap is
// evidence that the tripwire works. It is not evidence about the 64 MiB regime,
// and its run may not be banked with runs that are.
TEST(Regime, RunsOfDifferentRegimesRefuseToAggregate) {
  std::vector<RunRecord> rows(2);
  rows[1].caps.max_record_bytes = 1024;
  rows[1].caps.wal_buffer_bytes = 4096;
  ASSERT_EQ(rows[0].regime(), Regime::kDefault);
  ASSERT_EQ(rows[1].regime(), Regime::kNonDefault);
  Tally t;
  EXPECT_FALSE(AggregateRuns(rows, &t))
      << "a lowered-cap run was banked with default-cap runs, producing a "
         "number about no regime at all -- Track A's ablation found that "
         "lowering a harness parameter can remove the bug from existence "
         "entirely, so the two sets are not comparable";
}

}  // namespace
}  // namespace rig
}  // namespace basalt
