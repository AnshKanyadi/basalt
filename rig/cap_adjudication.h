// THE HARNESS DECIDES, NOT THE ENGINE. B1-D8, OVERRULED on adjudication.
//
// Revision 2 said the rig treats an over-cap run as void because the engine
// reported a tripwire. That is an escape hatch with the engine's hand on the
// lever: AN ENGINE THAT SPURIOUSLY TRIPS THE CAP WOULD DELETE THE EVIDENCE OF
// ITS OWN BUG, and the oracle would be believing the engine's account of
// itself -- the one thing ruling 4 exists to prevent.
//
// So the harness computes record_bytes ITSELF, from its own record of the batch
// it submitted, using section 5.3.4's frozen formula -- and for DeleteRange,
// expanding against ITS OWN reference key set, which it has because it is
// driving engine/model in parallel.
//
//   harness computes | engine reports    | verdict
//   -----------------|-------------------|------------------------------------
//   <= cap           | no error          | normal run; assertions proceed
//   <= cap           | kRecordTooLarge   | DIVERGENCE -- the run FAILS. The
//                    |                   | engine tripped on legal input.
//   >  cap           | no error          | DIVERGENCE -- the run FAILS. The
//                    |                   | engine accepted an over-cap record.
//   >  cap           | kRecordTooLarge   | kVoid: own column, never banked,
//                    |                   | rate tracked like inconclusive
//
// THE FORMULA IS REIMPLEMENTED HERE ON PURPOSE. Calling the engine's
// BatchRecordBytes would make this an oracle asking the engine whether the
// engine was right. The failure that opens -- harness and engine disagreeing
// about what the cap MEANS -- is closed by the formula being frozen in section
// 5.3.4 and by both divergence directions being asserted and induced (BM19,
// BM20). Section 0.1's middle row is exactly this trade.
#ifndef RIFT_RIG_CAP_ADJUDICATION_H_
#define RIFT_RIG_CAP_ADJUDICATION_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "run_outcome.h"
#include "status.h"

namespace rift {
namespace rig {

// An op AS THE HARNESS RECORDED SUBMITTING IT. A separate type from wal::Op
// deliberately: sharing one would let a change to the engine's op layout
// silently change the harness's arithmetic.
struct SubmittedOp {
  enum class Kind : uint8_t { kSet = 0, kDelete = 1, kDeleteRange = 2 };
  Kind kind = Kind::kSet;
  std::size_t key_bytes = 0;
  std::size_t value_bytes = 0;  // SET: value. DELETE_RANGE: end key.
};

// Section 5.3.4's frozen formula, implemented independently of the engine's.
uint64_t HarnessRecordBytes(const std::vector<SubmittedOp>& ops);

enum class CapVerdict : uint8_t {
  kNormal,              // under cap, no error: assertions proceed
  kVoid,                // over cap, correctly reported: never banked
  kSpuriousTripwire,    // under cap, error reported: DIVERGENCE, run fails
  kMissingTripwire,     // over cap, no error: DIVERGENCE, run fails
};
const char* CapVerdictName(CapVerdict v);

// True only for the two divergences. Both directions fail the run; neither
// voids it, because a void is a legitimate engine error and a divergence is a
// bug.
bool IsDivergence(CapVerdict v);

// `engine_reported` is whether the engine returned the cap's error code. It is
// the only engine input, and it is held to the harness's own computation rather
// than believed.
CapVerdict AdjudicateCap(uint64_t harness_bytes, uint64_t cap, bool engine_reported);

// The outcome a verdict forces. A divergence is a contract violation; a void is
// its own kind and is never banked.
RunOutcome OutcomeForCapVerdict(CapVerdict v);

}  // namespace rig
}  // namespace rift

#endif  // RIFT_RIG_CAP_ADJUDICATION_H_
