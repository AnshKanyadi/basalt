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

// Runs the full sweep over the fixed workload. Deterministic: same binary, same
// result, every time.
SweepResult RunSweep();

// How many Env calls the workload makes, discovered by running it once with no
// faults. The sweep's upper bound.
uint64_t WorkloadOrdinalCount();

}  // namespace rig
}  // namespace rift

#endif  // RIFT_RIG_SWEEP_H_
