#include "cap_adjudication.h"

#include "check.h"

namespace rift {
namespace rig {

uint64_t HarnessRecordBytes(const std::vector<SubmittedOp>& ops) {
  uint64_t n = 1 + 8 + 4;  // kind, seq, op_count
  for (const SubmittedOp& op : ops) {
    n += 1 + 4 + op.key_bytes;
    switch (op.kind) {  // NO default: arm
      case SubmittedOp::Kind::kSet:
      case SubmittedOp::Kind::kDeleteRange:
        n += 4 + op.value_bytes;
        break;
      case SubmittedOp::Kind::kDelete:
        break;
    }
  }
  return n;
}

const char* CapVerdictName(CapVerdict v) {
  switch (v) {  // NO default: arm
    case CapVerdict::kNormal:           return "normal";
    case CapVerdict::kVoid:             return "void (not evidence)";
    case CapVerdict::kSpuriousTripwire: return "divergence: tripped on legal input";
    case CapVerdict::kMissingTripwire:  return "divergence: accepted an over-cap record";
  }
  RIFT_UNREACHABLE("CapVerdict holds a value no enumerator names");
}

bool IsDivergence(CapVerdict v) {
  switch (v) {  // NO default: arm
    case CapVerdict::kSpuriousTripwire:
    case CapVerdict::kMissingTripwire:
      return true;
    case CapVerdict::kNormal:
    case CapVerdict::kVoid:
      return false;
  }
  RIFT_UNREACHABLE("CapVerdict holds a value no enumerator names");
}

CapVerdict AdjudicateCap(uint64_t harness_bytes, uint64_t cap, bool engine_reported) {
  const bool over = harness_bytes > cap;
  if (over) return engine_reported ? CapVerdict::kVoid : CapVerdict::kMissingTripwire;
  return engine_reported ? CapVerdict::kSpuriousTripwire : CapVerdict::kNormal;
}

RunOutcome OutcomeForCapVerdict(CapVerdict v) {
  switch (v) {  // NO default: arm
    case CapVerdict::kNormal:           return RunOutcome::kContractPass;
    case CapVerdict::kVoid:             return RunOutcome::kVoid;
    case CapVerdict::kSpuriousTripwire:
    case CapVerdict::kMissingTripwire:
      return RunOutcome::kContractViolation;
  }
  RIFT_UNREACHABLE("CapVerdict holds a value no enumerator names");
}

}  // namespace rig
}  // namespace rift
