// Runs at non-default caps NEVER aggregate with default-cap runs. Section 8.4.
//
// Ruled from Track A's ablation, which found that lowering a harness parameter
// did not weaken detection -- IT REMOVED THE BUG FROM EXISTENCE ENTIRELY, so
// results across parameter regimes were not comparable at all. The same hazard
// applies here and the mechanism is the same shape as section 7.5's.
//
// Run-time configurable caps exist precisely so the sweep can set them low and
// watch the tripwire fire -- a tripwire nobody has watched fire is the
// decoration this project rejects everywhere else -- and this file is what
// stops that convenience from contaminating the numbers it makes reachable.
//
// STATED SO NOBODY HAS TO INFER IT:
//
//   A tripwire observed firing at a lowered cap is evidence THAT THE TRIPWIRE
//   WORKS. It is NOT evidence about the 64 MiB or 256 MiB regime, and its run
//   may not be banked with runs that are.
#ifndef RIFT_RIG_REGIME_H_
#define RIFT_RIG_REGIME_H_

#include <cstdint>
#include <vector>

#include "caps.h"
#include "run_outcome.h"

namespace rift {
namespace rig {

enum class Regime : uint8_t {
  kDefault,     // both caps equal the named constants
  kNonDefault,  // either differs
};
const char* RegimeName(Regime r);

// Every run record carries THE ACTUAL CAP VALUES, not a flag someone set. The
// regime is computed from them, so a run cannot be mislabelled by forgetting.
struct RunRecord {
  uint64_t max_record_bytes = wal::kMaxRecordBytes;
  uint64_t wal_buffer_bytes = wal::kWalBufferBytes;
  RunOutcome outcome = RunOutcome::kContractPass;

  Regime regime() const;
};

struct Tally {
  Regime regime = Regime::kDefault;
  std::size_t pass = 0;
  std::size_t violation = 0;
  std::size_t characterization = 0;
  std::size_t inconclusive = 0;
  std::size_t voided = 0;
  std::size_t total() const {
    return pass + violation + characterization + inconclusive + voided;
  }
};

// AGGREGATION IS KEYED ON REGIME. Returns false -- and fills nothing -- if the
// rows span more than one regime. A summarizer that combined them would be
// producing a number about no regime at all.
bool AggregateRuns(const std::vector<RunRecord>& rows, Tally* out);

}  // namespace rig
}  // namespace rift

#endif  // RIFT_RIG_REGIME_H_
