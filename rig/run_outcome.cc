#include "run_outcome.h"

#include "check.h"

namespace rift {
namespace rig {

bool CountsAsRecoveryEvidence(RunOutcome outcome) {
  // NO `default:` ARM.
  switch (outcome) {
    case RunOutcome::kContractPass:
      return true;
    case RunOutcome::kContractViolation:      // a bug, with a kill point
    case RunOutcome::kCharacterizationOnly:   // the contract was not under test
    case RunOutcome::kInconclusive:           // the check did not complete
    case RunOutcome::kVoid:                   // adjudicated legitimate, never banked
      return false;
  }
  RIFT_UNREACHABLE("RunOutcome holds a value no enumerator names");
}

const char* RunOutcomeName(RunOutcome outcome) {
  switch (outcome) {
    case RunOutcome::kContractPass:         return "kContractPass";
    case RunOutcome::kContractViolation:    return "kContractViolation";
    case RunOutcome::kCharacterizationOnly: return "kCharacterizationOnly";
    case RunOutcome::kInconclusive:         return "kInconclusive";
    case RunOutcome::kVoid:                 return "kVoid";
  }
  RIFT_UNREACHABLE("RunOutcome holds a value no enumerator names");
}

const char* RunOutcomeLedgerColumn(RunOutcome outcome) {
  switch (outcome) {
    case RunOutcome::kContractPass:         return "pass";
    case RunOutcome::kContractViolation:    return "violation";
    case RunOutcome::kCharacterizationOnly: return "characterization (not evidence)";
    case RunOutcome::kInconclusive:         return "inconclusive (not evidence)";
    case RunOutcome::kVoid:                 return "void (not evidence)";
  }
  RIFT_UNREACHABLE("RunOutcome holds a value no enumerator names");
}

}  // namespace rig
}  // namespace rift
