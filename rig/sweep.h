// The kill-point sweep. Section 9.5, section 7.6, and section 7.4 condition 3's
// sweep-level half.
//
// ONE WORKLOAD, EVERY KILL POINT. The identity of a point is a GLOBAL Env-CALL
// ORDINAL: complete by construction, nothing to annotate and therefore nothing
// to forget. Each point is visited twice, once with a kill BEFORE the effect and
// once AFTER it, because those are the two elements of the recovery set and a
// sweep that only ever kills before the effect can never observe the second one.
//
// EVERY OUTCOME IS ADJUDICATED HARNESS-SIDE. engine/model never errors, so every
// error the C++ engine can return is a place the two can legally differ, and
// every such place is closed by a predicate computed from the harness's own
// record -- never from the engine's report. An engine error whose predicate is
// not satisfied is a divergence and fails the run; a satisfied predicate with no
// error is also a divergence.
#ifndef RIFT_RIG_SWEEP_H_
#define RIFT_RIG_SWEEP_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "caps.h"
#include "exactness_oracle.h"
#include "run_outcome.h"

namespace rift {
namespace rig {

struct SweepPoint {
  uint64_t ordinal = 0;
  std::string call_site;
  bool after_effect = false;
  RunOutcome outcome = RunOutcome::kContractPass;
  MatchedElement matched = MatchedElement::kNone;
  std::string why;
};

struct SweepResult {
  std::size_t points_visited = 0;
  std::map<std::string, std::size_t> census;  // per call kind
  std::size_t pass = 0;
  std::size_t violation = 0;
  std::size_t characterization = 0;
  std::size_t inconclusive = 0;
  std::size_t voided = 0;
  std::size_t matched_previous = 0;
  std::size_t matched_in_flight = 0;
  std::size_t real_exit_samples = 0;
  std::size_t real_exit_agreements = 0;
  std::vector<SweepPoint> failures;

  bool BothElementsObserved() const {
    return matched_previous > 0 && matched_in_flight > 0;
  }
};

// TWO REGIMES, RUN SEPARATELY AND NEVER AGGREGATED. Section 8.4: runs at
// non-default caps never mix with default-cap runs, and B2 gives that rule its
// first real work.
//
//   kDefault  the caps as shipped. No flush occurs -- the threshold is four
//             megabytes and this workload writes six keys -- so this regime is
//             the DIRECT SUCCESSOR of the sweep B1 measured its floors against,
//             and its numbers are comparable to them.
//
//   kFlush    a low flush threshold and a workload that crosses it. This is the
//             regime in which the flush path has kill points at all, and it is
//             where CF-1's obligation is discharged: BM2's accidental defence
//             expires exactly when a flush writes the memtable out, so the two
//             regimes together say whether the accident was the whole
//             suppression or only part of it.
//
// Comparing a number from one against a floor from the other would be the
// aggregation section 8.4 forbids, which is why the regime is named in the
// output and in FLOORS.txt rather than left to whoever reads the log.
enum class SweepRegime : uint8_t { kDefault, kFlush };
const char* SweepRegimeName(SweepRegime r);
wal::Caps CapsFor(SweepRegime r);

// Runs the full sweep over the fixed workload for one regime. Deterministic:
// same binary, same regime, same result, every time.
SweepResult RunSweep(SweepRegime regime);

// How many Env calls the workload makes in this regime, discovered by running
// it once with no faults. The sweep's upper bound.
uint64_t WorkloadOrdinalCount(SweepRegime regime);

}  // namespace rig
}  // namespace rift

#endif  // RIFT_RIG_SWEEP_H_
