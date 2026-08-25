// The cap adjudication and the regime key: the harness deciding, not the engine.
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cap_adjudication.h"
#include "caps.h"
#include "format.h"
#include "regime.h"
#include "run_outcome.h"

namespace rift {
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
  EXPECT_EQ(OutcomeForCapVerdict(CapVerdict::kSpuriousTripwire),
            RunOutcome::kContractViolation);
  EXPECT_EQ(OutcomeForCapVerdict(CapVerdict::kMissingTripwire),
            RunOutcome::kContractViolation);
  EXPECT_EQ(OutcomeForCapVerdict(CapVerdict::kVoid), RunOutcome::kVoid);
  EXPECT_FALSE(CountsAsRecoveryEvidence(RunOutcome::kVoid));
}

// ------------------------------------------------------------- the regime

TEST(Regime, IsComputedFromTheActualCapValues) {
  RunRecord r;
  EXPECT_EQ(r.regime(), Regime::kDefault);
  r.max_record_bytes = 1024;
  EXPECT_EQ(r.regime(), Regime::kNonDefault);
  r.max_record_bytes = wal::kMaxRecordBytes;
  r.wal_buffer_bytes = 4096;
  EXPECT_EQ(r.regime(), Regime::kNonDefault);
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
  rows[1].max_record_bytes = 1024;
  rows[1].wal_buffer_bytes = 4096;
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
}  // namespace rift
