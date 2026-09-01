#include "poller.h"

namespace basalt {
namespace rig {

const char* BusyVerdictName(BusyVerdict v) {
  switch (v) {  // NO default: arm
    case BusyVerdict::kNormal:        return "normal";
    case BusyVerdict::kBackpressured: return "backpressured";
    case BusyVerdict::kSpuriousBusy:  return "spurious-busy";
    case BusyVerdict::kMissingBusy:   return "missing-busy";
  }
  return "unknown";
}

bool IsBusyDivergence(BusyVerdict v) {
  switch (v) {  // NO default: arm
    case BusyVerdict::kSpuriousBusy:
    case BusyVerdict::kMissingBusy:
      return true;
    case BusyVerdict::kNormal:
    case BusyVerdict::kBackpressured:
      return false;
  }
  return false;
}

BusyVerdict AdjudicateBusy(uint64_t owed_plus_batch, uint64_t busy_bytes,
                           bool engine_reported) {
  // STRICTLY GREATER, matching the engine's `> busy_bytes` exactly. An
  // off-by-one between the two arithmetics would present as a divergence at
  // precisely one occupancy and nowhere else, which is the hardest possible
  // shape to attribute -- so the comparison is written the same way on both
  // sides and a test lands on the boundary value itself.
  const bool owed = busy_bytes != 0 && owed_plus_batch > busy_bytes;
  if (owed) {
    return engine_reported ? BusyVerdict::kBackpressured : BusyVerdict::kMissingBusy;
  }
  return engine_reported ? BusyVerdict::kSpuriousBusy : BusyVerdict::kNormal;
}

RunOutcome OutcomeForBusyVerdict(BusyVerdict v) {
  switch (v) {  // NO default: arm
    case BusyVerdict::kSpuriousBusy:
    case BusyVerdict::kMissingBusy:
      return RunOutcome::kContractViolation;
    case BusyVerdict::kNormal:
    case BusyVerdict::kBackpressured:
      // BANKABLE, BOTH OF THEM. See the header: a policy that fired correctly
      // is the rig working, not evidence being lost.
      return RunOutcome::kContractPass;
  }
  return RunOutcome::kContractViolation;
}

}  // namespace rig
}  // namespace basalt
