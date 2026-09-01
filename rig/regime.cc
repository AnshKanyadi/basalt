#include "regime.h"

#include "basalt/check.h"

namespace basalt {
namespace rig {

const char* RegimeName(Regime r) {
  switch (r) {  // NO default: arm
    case Regime::kDefault:    return "default";
    case Regime::kNonDefault: return "non-default (never banked with default)";
  }
  BASALT_UNREACHABLE("Regime holds a value no enumerator names");
}

Regime RunRecord::regime() const {
  return caps.IsDefault() ? Regime::kDefault : Regime::kNonDefault;
}

bool AggregateRuns(const std::vector<RunRecord>& rows, Tally* out) {
  if (rows.empty()) return false;
  const Regime key = rows.front().regime();
  for (const RunRecord& r : rows) {
    // THE AGGREGATION KEY. BM18 removes this check.
    if (r.regime() != key) return false;
  }
  Tally t;
  t.regime = key;
  for (const RunRecord& r : rows) {
    switch (r.outcome) {  // NO default: arm
      case RunOutcome::kContractPass:         ++t.pass; break;
      case RunOutcome::kContractViolation:    ++t.violation; break;
      case RunOutcome::kCharacterizationOnly: ++t.characterization; break;
      case RunOutcome::kInconclusive:         ++t.inconclusive; break;
      case RunOutcome::kVoid:                 ++t.voided; break;
    }
  }
  *out = t;
  return true;
}

}  // namespace rig
}  // namespace basalt
