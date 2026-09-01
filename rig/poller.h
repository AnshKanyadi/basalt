// THE RIG DRIVES THE POLLER; IT DOES NOT OBSERVE ONE. B1-Q11, ruled at B5.
//
// Section 7.6.1 ruled that Status::kBusy lands only with a predicate statable
// in BOTH directions, and gave the mechanical reason a passive rig cannot
// deliver one:
//
//   Direction 2 is the one that forces the design. To assert it, the harness
//   must be able to PUT the engine into a state where backpressure is
//   unambiguously owed and then observe that it was not signalled. A rig that
//   only watches the poller can never construct that state on purpose.
//
// So this file owns the record and the pacing. The quantity is
//
//     owed = (bytes this rig submitted and the engine ACCEPTED) - (bytes its
//            own Sync calls have drained)
//
// computed from what the rig did, never read out of the engine. That identity
// is why B5-D4 could rule the poller harness-side at all: a poller inside the
// engine is a thread of control the engine schedules, and a rig that cannot
// schedule it cannot construct direction 2.
//
// THE CONSEQUENCE, so no benchmark number is misread: a production embedder
// supplies its own poller, and nothing in BENCHMARKS.md is a claim about a
// poller we ship. The pacing in any measurement is a harness input.
#ifndef BASALT_RIG_POLLER_H_
#define BASALT_RIG_POLLER_H_

#include <cstdint>

#include "run_outcome.h"

namespace basalt {
namespace rig {

enum class BusyVerdict : uint8_t {
  kNormal,         // under the threshold, accepted: assertions proceed
  kBackpressured,  // over the threshold, correctly refused: the policy working
  kSpuriousBusy,   // under the threshold, refused: DIVERGENCE, run fails
  kMissingBusy,    // over the threshold, accepted: DIVERGENCE, run fails
};
const char* BusyVerdictName(BusyVerdict v);

// True only for the two divergences.
//
// kBackpressured IS NOT A DIVERGENCE AND IS NOT A VOID EITHER, which is where
// this differs from the cap. A tripwire firing means the run went somewhere it
// should not have and its evidence is not bankable; a policy firing means the
// policy WORKED, and a run in which the engine correctly applied backpressure
// is a run whose every other assertion still counts. Voiding it would make the
// rig unable to bank exactly the runs it exists to produce.
bool IsBusyDivergence(BusyVerdict v);  // BASALT_EVIDENCE_DECIDER

// `engine_reported` is whether the engine returned kBusy. It is the ONLY engine
// input, and it is held to the harness's arithmetic rather than believed.
//
// `busy_bytes == 0` is backpressure DISABLED, and it is a real regime: with the
// policy on, the WAL buffer tripwire is unreachable through Apply, so the
// tripwire's own tests run with the policy off. Under it, kBusy is never owed
// and any kBusy at all is spurious.
BusyVerdict AdjudicateBusy(uint64_t owed_plus_batch, uint64_t busy_bytes,
                           bool engine_reported);

// The outcome a verdict forces.
RunOutcome OutcomeForBusyVerdict(BusyVerdict v);  // BASALT_EVIDENCE_DECIDER

}  // namespace rig
}  // namespace basalt

#endif  // BASALT_RIG_POLLER_H_
