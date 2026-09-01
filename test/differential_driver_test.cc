// THE DRIVER: deterministic, and it reaches no verdict.
//
// What is asserted here is everything the JUDGE has to be able to rely on and
// cannot check for itself -- because by the time the judge sees an artifact,
// the run that produced it is over.
#include "differential_driver.h"

#include <gtest/gtest.h>

#include "differential_artifact.h"

namespace basalt {
namespace rig {
namespace {

DiffRunOptions Options(DiffRegime r = DiffRegime::kFlush, uint64_t seed = 1) {
  DiffRunOptions o;
  o.regime = r;
  o.seed = seed;
  o.ops = 60;
  o.engine_commit = "engine1";
  o.model_commit = "model1";
  return o;
}

// SAME OPTIONS, SAME BYTES. Without this the corpus promise is empty: an
// artifact that cannot be regenerated is a record of something that happened
// once.
TEST(DiffDriver, IsDeterministic) {
  const std::string a = EncodeDiffArtifact(RunDifferential(Options()));
  const std::string b = EncodeDiffArtifact(RunDifferential(Options()));
  EXPECT_EQ(a, b);
}

TEST(DiffDriver, DifferentSeedsProduceDifferentRuns) {
  const std::string a = EncodeDiffArtifact(RunDifferential(Options(DiffRegime::kFlush, 1)));
  const std::string b = EncodeDiffArtifact(RunDifferential(Options(DiffRegime::kFlush, 2)));
  EXPECT_NE(a, b);
}

// WHAT THE DRIVER WRITES IS WHAT THE CLASSIFIER ACCEPTS -- and it must be, or
// the judge never sees a run at all.
TEST(DiffDriver, EveryArtifactItWritesIsOneTheClassifierAccepts) {
  for (DiffRegime r : {DiffRegime::kDefault, DiffRegime::kFlush, DiffRegime::kCompact}) {
    for (uint64_t seed = 1; seed <= 3; ++seed) {
      const DiffArtifact made = RunDifferential(Options(r, seed));
      const std::string image = EncodeDiffArtifact(made);
      DiffArtifact parsed;
      const DiffCheck v = ParseDiffArtifact(Slice(image), &parsed);
      ASSERT_TRUE(v.ok()) << DiffRegimeName(r) << " seed " << seed << ": "
                          << DiffFaultName(v.fault) << " " << v.why;
      EXPECT_EQ(DiffRegimeName(r), parsed.provenance.regime);
      EXPECT_FALSE(parsed.submission.empty());
    }
  }
}

// IT REACHES NO VERDICT, and that is asserted rather than assumed: an artifact
// carrying a verdict this side invented would be an engine judging itself.
TEST(DiffDriver, ReachesNoVerdict) {
  const DiffArtifact a = RunDifferential(Options());
  EXPECT_EQ(DiffOutcome::kUnrun, a.outcome);
  EXPECT_FALSE(RequireJudged(a).ok());
}

// THE SUBMISSION LOG CARRIES THE SEQUENCES THE ENGINE ASSIGNED, which is what
// lets the model replay to the same watermark. An op that consumed no sequence
// carries 0.
TEST(DiffDriver, EveryWriteCarriesTheSequenceItWasAssigned) {
  const DiffArtifact a = RunDifferential(Options());
  bool saw_write = false;
  for (const DiffOp& op : a.submission) {
    switch (op.kind) {  // NO default: arm
      case DiffOpKind::kSet:
      case DiffOpKind::kDelete:
      case DiffOpKind::kDeleteRange:
        EXPECT_GT(op.seq, 0u) << "a write with no sequence cannot be replayed";
        saw_write = true;
        break;
      case DiffOpKind::kSync:
      case DiffOpKind::kSnapshotTake:
      case DiffOpKind::kSnapshotRelease:
        EXPECT_EQ(0u, op.seq) << "an op that consumes no sequence must carry 0";
        break;
    }
  }
  EXPECT_TRUE(saw_write);
}

// THE WORKLOAD REACHES DELETE_RANGE, INCLUDING ITS UNBOUNDED SHAPES. [A3]'s
// reason: the two engines implement it by entirely different mechanisms, so it
// is the strongest evidence the rig can produce -- and a workload that never
// issued one would make the whole differential weaker without saying so.
TEST(DiffDriver, TheWorkloadReachesEveryOperationKindIncludingUnboundedRanges) {
  bool set = false, del = false, range = false, sync = false, snap = false;
  bool unbounded_start = false, unbounded_end = false, bounded_range = false;
  for (uint64_t seed = 1; seed <= 6; ++seed) {
    for (const DiffOp& op : RunDifferential(Options(DiffRegime::kFlush, seed)).submission) {
      switch (op.kind) {  // NO default: arm
        case DiffOpKind::kSet: set = true; break;
        case DiffOpKind::kDelete: del = true; break;
        case DiffOpKind::kDeleteRange:
          range = true;
          if (!op.start_bounded) unbounded_start = true;
          if (!op.end_bounded) unbounded_end = true;
          if (op.start_bounded && op.end_bounded) bounded_range = true;
          break;
        case DiffOpKind::kSync: sync = true; break;
        case DiffOpKind::kSnapshotTake:
        case DiffOpKind::kSnapshotRelease: snap = true; break;
      }
    }
  }
  EXPECT_TRUE(set);
  EXPECT_TRUE(del);
  EXPECT_TRUE(range);
  EXPECT_TRUE(sync);
  EXPECT_TRUE(snap);
  EXPECT_TRUE(unbounded_start) << "no unbounded start in six seeds";
  EXPECT_TRUE(unbounded_end) << "no unbounded end in six seeds";
  EXPECT_TRUE(bounded_range) << "no finite range in six seeds";
}

// A KILL CHANGES THE RUN. Without this, a kill_ordinal that silently did
// nothing would produce a full corpus of clean runs reported as crash schedules.
TEST(DiffDriver, AKillProducesADifferentRunFromACleanOne) {
  DiffRunOptions clean = Options();
  const uint64_t ordinals = DifferentialOrdinalCount(clean);
  ASSERT_GT(ordinals, 20u);
  DiffRunOptions killed = clean;
  killed.kill_ordinal = ordinals / 2;
  const DiffArtifact a = RunDifferential(clean);
  const DiffArtifact b = RunDifferential(killed);
  EXPECT_NE(EncodeDiffArtifact(a), EncodeDiffArtifact(b))
      << "the kill point had no effect on the run";
}

}  // namespace
}  // namespace rig
}  // namespace basalt
