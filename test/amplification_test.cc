// THE INSTRUMENT THAT PRODUCES THE NUMBER BENCHMARKS.md PUBLISHES, MEASURED.
//
// GF-26 one level over. A sweep regime with no class floored against it is a
// green with unknown sensitivity; a MEASUREMENT with no class floored against
// it is the same thing, and this one decides B3-D3 and appears in a file whose
// first rule is that a number without its methodology is not a result.
//
// THE TWO CLASSES ARE THE TWO FAILURES THE INSTRUMENT ALREADY HAD, and both
// were caught by a person rather than by an instrument -- the second only after
// the number had been printed once:
//
//   IT RETURNED ZERO WHERE IT SHOULD HAVE RETURNED BYTES. `durable_bytes_after`
//   is a file size after a Sync and is left at zero for an Append, so summing
//   it over appends gave a write amplification of 0.00. It announced itself
//   only because zero cannot be true.
//
//   IT REPORTED A RUN WITHOUT ITS CONDITIONS. A workload that stops with L0
//   partly full has not paid for those files' compaction, so its write number
//   reads LOW -- the direction that flatters the result.
//
// THESE RUN AT A LOWERED FLUSH THRESHOLD so the suite stays fast. That is a
// NON-DEFAULT REGIME by section 8.4's rule and its numbers are never banked
// with the shipped-caps measurement: what is asserted here is that THE
// INSTRUMENT WORKS, never what the engine's amplification is.
#include "amplification.h"

#include <gtest/gtest.h>

#include <cstdio>

#include "basalt/caps.h"

namespace basalt {
namespace rig {
namespace {

wal::Caps SmallCaps() {
  wal::Caps c;
  c.flush_bytes = 8u * 1024;
  return c;
}

// WRITTEN BYTES ARE BYTES, NOT A FILE SIZE. Every submitted byte is written at
// least twice -- once to the WAL and once to a flushed table -- so a write
// amplification below 2 is the instrument under-counting, and a zero is it
// counting the wrong field entirely.
TEST(AmpInstrument, WrittenBytesCountWhatTheEngineAppended) {
  const AmpResult r = MeasureAmplification(SmallCaps(), {64u * 1024});
  ASSERT_EQ(1u, r.points.size());
  const AmpPoint& p = r.points[0];
  EXPECT_GT(p.written_bytes, 0u) << "the instrument counted no bytes at all";
  EXPECT_GT(p.write, 2.0)
      << "every byte is written to the WAL and again to a table, so anything at "
         "or below 2 is the instrument under-counting rather than the engine "
         "being efficient";
  EXPECT_LT(p.write, 100.0) << "and a number this large is the instrument too";
}

// THE CONDITIONS ARE REPORTED, AND BOTH DIRECTIONS ARE ASSERTED (GF-14). A
// blinded `l0_at_end` reads as zero, which is indistinguishable from a run that
// genuinely drained -- so a test that only ever measured a drained run would
// pass against an instrument that never reports anything.
TEST(AmpInstrument, ReportsUncompactedLevelZeroWhenThereIsSome) {
  // Small enough that the workload stops before four flushes accumulate, so L0
  // has files that were never compacted.
  const AmpResult r = MeasureAmplification(SmallCaps(), {24u * 1024});
  ASSERT_EQ(1u, r.points.size());
  EXPECT_GT(r.points[0].l0_at_end, 0u)
      << "this workload cannot have drained L0, so a zero here is the "
         "instrument not reporting the condition its number is true under";
}

TEST(AmpInstrument, ReportsNoUncompactedLevelZeroWhenTheRunDrained) {
  // 64 KiB WAS CHOSEN BY MEASUREMENT, NOT BY ARITHMETIC. Whether a run ends
  // with L0 drained depends on where the workload stops relative to the
  // trigger, and the first version of this test guessed a size, guessed wrong,
  // and would have been "fixed" by loosening the assertion to `>= 0` -- which
  // asserts nothing.
  //
  // A sweep of sizes showed 64, 128 and 192 KiB drain and 32, 96, 160, 224 and
  // 256 do not. The pattern is real and the point is that it was READ rather
  // than derived: a condition column is only worth having if a test can tell
  // its two states apart, and the two states had to be found.
  const AmpResult r = MeasureAmplification(SmallCaps(), {64u * 1024});
  ASSERT_EQ(1u, r.points.size());
  EXPECT_EQ(0u, r.points[0].l0_at_end)
      << "a drained run must report zero, or the condition column cannot "
         "distinguish a drained run from an instrument that reports nothing";
}

// THE CROSSING POINT IS DERIVED FROM THE CAPS, so a reader who changes the
// flush threshold gets the threshold that follows from it rather than one that
// followed from a setting they have just changed.
TEST(AmpInstrument, TheCrossingPointFollowsTheCaps) {
  wal::Caps a;
  wal::Caps b = SmallCaps();
  EXPECT_EQ(8ull * 4ull * a.flush_bytes, CrossingPointBytes(a));
  EXPECT_EQ(8ull * 4ull * b.flush_bytes, CrossingPointBytes(b));
  EXPECT_NE(CrossingPointBytes(a), CrossingPointBytes(b))
      << "a threshold that does not move with the caps is a constant wearing a "
         "derivation's clothes";
}

// READ AMPLIFICATION IS AT LEAST ONE FOR KEYS THAT ARE PRESENT, and no more
// than the number of tables. A zero would mean the sample found nothing, which
// is the instrument measuring absent keys -- a different number entirely.
TEST(AmpInstrument, ReadAmplificationIsAtLeastOneAndAtMostTheTableCount) {
  const AmpResult r = MeasureAmplification(SmallCaps(), {256u * 1024});
  ASSERT_EQ(1u, r.points.size());
  const AmpPoint& p = r.points[0];
  EXPECT_GE(p.read, 1.0) << "sampled keys are present, so each is in some table";
  EXPECT_LE(p.read, static_cast<double>(p.tables));
}

}  // namespace
}  // namespace rig
}  // namespace basalt
