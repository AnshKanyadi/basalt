// RunOutcome: what a rig run may and may not be counted as.
//
// This is harness code, not engine code. It lives outside src/ on purpose: src
// is what the A5 scope scan (DESIGN-B1 section 9.4) polices and what ships as
// the library archive, and the oracle must never be reachable from the thing it
// judges.
#ifndef BASALT_RIG_RUN_OUTCOME_H_
#define BASALT_RIG_RUN_OUTCOME_H_

#include <cstdint>

namespace basalt {
namespace rig {

// CLOSED. No `default:` arm anywhere, enforced by -Werror=switch, which is the
// C++ compiler implementing A0.6's exhaustive rule for free. DESIGN-B1 7.5.
enum class RunOutcome : uint8_t {
  kContractPass,          // (i) and (ii) both asserted, both held
  kContractViolation,     // (i) or (ii) failed -- a bug
  kCharacterizationOnly,  // an exactness-suspending injector ran: (ii) SUSPENDED
  kInconclusive,          // the checks did not complete
  kVoid,                  // an engine error whose HARNESS-SIDE predicate held
};

// THE ONLY PLACE THIS POLICY LIVES.
//
// Adding a kind forces a decision here rather than defaulting to "sure, count
// it" at whichever summarizer forgot. That is the whole design: a suppression
// that is remembered is a suppression that lapses, and the one it would lapse
// on is kCharacterizationOnly -- a run in which assertion (ii) was suspended
// and the recovery contract was therefore NOT under test. Counting such a run
// as evidence is not a rounding error; it is citing a run that did not check
// the thing being claimed.
bool CountsAsRecoveryEvidence(RunOutcome outcome);  // BASALT_EVIDENCE_DECIDER

// THE MECHANICAL LINK between an enabled exactness-suspending injector and the
// outcome it forces (DESIGN-B1 section 7.5).
//
// TestEnv records the FACT -- that a registry member was enabled, at the point
// of enabling. This function is where that fact becomes a policy, and it is a
// FLOOR: the rig may narrow the outcome further, to a violation or to
// inconclusive, but it may never widen it back to kContractPass. Without this
// function the suppression would be a thing somebody remembers to do, and the
// run it would be forgotten on is the one where assertion (ii) was suspended
// and the recovery contract was therefore not under test at all.
RunOutcome OutcomeFloor(bool exactness_suspended);  // BASALT_EVIDENCE_DECIDER

// Total over RunOutcome. Same closed-switch discipline as CodeName.
const char* RunOutcomeName(RunOutcome outcome);

// The ledger heading a run of this kind is reported under. Two of them say
// "(not evidence)" in the heading itself, which cannot be misread by someone
// skimming a table -- Amendment A4's shape, one language over.
const char* RunOutcomeLedgerColumn(RunOutcome outcome);

}  // namespace rig
}  // namespace basalt

#endif  // BASALT_RIG_RUN_OUTCOME_H_
