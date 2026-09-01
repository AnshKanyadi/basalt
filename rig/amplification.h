// SPACE, READ AND WRITE AMPLIFICATION -- B3's exit criterion, and B3-D3's
// deciding measurement.
//
// ---------------------------------------------------------------------------
// WHAT EACH NUMBER IS, AND WHY IT IS THAT ONE (B3-D8).
//
//   SPACE  bytes on disk / bytes of live data AS THE HARNESS COMPUTES IT. The
//          harness knows the live set exactly; asking the engine would be
//          asking the thing under test.
//   READ   TABLES CONSULTED per point read, MEASURED -- not derived from the
//          level structure. The bloom filter's whole purpose is to make the
//          measured number smaller than the structural one, so a derived
//          number would report the filter as having no effect.
//   WRITE  bytes written / bytes submitted. It is what decides whether (c) is
//          reopened, so it is measured even though the exit criterion does not
//          name it.
//
// ---------------------------------------------------------------------------
// THE THRESHOLD WAS FIXED BEFORE THIS FILE EXISTED, in DESIGN-B3 section 8.1,
// and it is repeated here so a reader of the numbers meets it beside them:
//
//     WA ~ 1 (WAL) + 1 (flush) + D / (K*F)      K = 4, F = the flush threshold
//
//   (b) crosses 10x write amplification at D / (K*F) > 8, which at the shipped
//   caps -- K*F = 16 MiB -- is D > 128 MiB OF LIVE DATA PER ENGINE.
//
// AND THE PRE-DECLARED OUTCOME, WHICH IS THE PART THAT MATTERS MOST:
//
//   IF THE MEASURED SIZES SIT BELOW THE CROSSING POINT, THE QUESTION IS NOT
//   DECIDABLE ON EVIDENCE, AND (b) WINS ON AMENDMENT A6's RULE RATHER THAN ON
//   A BENCHMARK. That is a RESULT and is recorded as one.
//
// IT IS NOT AN "INCONCLUSIVE", and Amendment A4's vocabulary settles it:
// inconclusive means THE INSTRUMENT DID NOT FINISH. A measurement that finished
// and returned a number below a threshold HAS CONCLUDED. Calling it
// inconclusive would be A4's own failure mode running in the opposite
// direction, and "run it bigger" is how a benchmark gets sized until it says
// something -- which is what fixing the number in advance exists to prevent.
#ifndef BASALT_RIG_AMPLIFICATION_H_
#define BASALT_RIG_AMPLIFICATION_H_

#include <cstdint>
#include <string>
#include <vector>

#include "basalt/caps.h"

namespace basalt {
namespace rig {

// The crossing point of DESIGN-B3 section 8.1, recomputed from the caps rather
// than written as a constant -- so a reader who changes the flush threshold
// gets the threshold that follows from it, not the one that followed from a
// setting they have just changed.
//
//   crossing_bytes = 8 * K * flush_bytes
//
// where 8 is `WA_limit - 2`: the 10x limit less the WAL copy and the flush copy,
// which are one each and unavoidable.
uint64_t CrossingPointBytes(const wal::Caps& caps);

struct AmpPoint {
  uint64_t live_bytes = 0;       // as the harness counts what it submitted
  uint64_t disk_bytes = 0;       // as the harness counts the directory
  uint64_t submitted_bytes = 0;
  uint64_t written_bytes = 0;    // every byte the engine appended, via TestEnv
  double space = 0.0;
  double write = 0.0;
  double read = 0.0;             // tables consulted per point read, MEASURED
  uint64_t tables = 0;
  // HOW MANY L0 FILES WERE STILL UNCOMPACTED WHEN THE WORKLOAD STOPPED.
  //
  // Reported because it is the condition the write number is true UNDER. A run
  // that ends with L0 partly full has not paid for those files' compaction yet,
  // so its write amplification is a SNAPSHOT MID-CYCLE and reads lower than the
  // steady state. A number whose conditions are not printed beside it invites
  // the reader to assume the best ones.
  uint64_t l0_at_end = 0;
};

struct AmpResult {
  wal::Caps caps;
  uint64_t crossing_bytes = 0;
  std::vector<AmpPoint> points;
  // The conclusion, in the vocabulary section 8.2b fixed in advance.
  bool above_crossing = false;
  std::string conclusion;
};

// Runs the fillrandom-shaped workload at each size in `live_targets`, in one
// TestEnvironment per size, and reports the three numbers per size.
AmpResult MeasureAmplification(const wal::Caps& caps,
                               const std::vector<uint64_t>& live_targets);

}  // namespace rig
}  // namespace basalt

#endif  // BASALT_RIG_AMPLIFICATION_H_
