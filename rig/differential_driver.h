// THE C++ HALF OF THE DIFFERENTIAL: run a seeded workload, kill, reopen, and
// emit an artifact. It reaches NO VERDICT -- the judge is Go, and this side
// cannot compute what the model would have held.
//
// ---------------------------------------------------------------------------
// RULING 4 IN A WORLD WITH TWO ENGINES, AND THIS IS THE ENTRY POINT SO IT IS
// STATED HERE RATHER THAN ONLY IN THE DESIGN DOC:
//
//   THE OP LOG IS THE SHARED INPUT. NEITHER ENGINE IS A WITNESS ABOUT THE
//   OTHER. `w` IS CAPTURED BEFORE THE KILL.
//
// The last clause is the load-bearing one. If the rig recovered the engine and
// then asked IT which watermark to compare at, the engine would be answering a
// question about itself and the comparison would agree with whatever it said --
// A RIG THAT ASKS THE SURVIVOR WHAT WAS PROMISED CANNOT CATCH A BROKEN PROMISE.
//
// So the order is fixed and it is not an implementation detail:
//
//   1. the submission log is authored HERE, before either engine sees it;
//   2. the workload runs, and `w = DurableSeq()` is read from the LIVE process;
//   3. the kill lands;
//   4. the reopen produces the recovered state;
//   5. the artifact carries all four, and the judge compares (2) against (4).
//
// `w` from step 2 is the engine reporting about ITSELF, which is admissible for
// B1's reason: it is A PROMISE, and the whole comparison is "did you keep the
// promise you made?" An engine that lies about `w` fails against the model,
// because the model is held to the lie.
#ifndef BASALT_RIG_DIFFERENTIAL_DRIVER_H_
#define BASALT_RIG_DIFFERENTIAL_DRIVER_H_

#include <cstdint>
#include <string>

#include "differential_artifact.h"

namespace basalt {
namespace rig {

struct DiffRunOptions {
  DiffRegime regime = DiffRegime::kFlush;
  uint64_t seed = 1;
  // How many operations the workload issues before the kill.
  uint32_t ops = 200;
  // Which Env call to kill at. 0 means "do not kill" -- a clean run, which is
  // the control: an engine that recovers nothing after a clean close is broken
  // in a way no crash schedule is needed to find.
  uint64_t kill_ordinal = 0;
  std::string engine_commit = "unknown";
  std::string model_commit = "unknown";
};

// Runs one schedule and returns the artifact, UNJUDGED. Deterministic: same
// options, same bytes.
DiffArtifact RunDifferential(const DiffRunOptions& o);

// How many Env calls the workload makes, so a caller can sweep kill points.
uint64_t DifferentialOrdinalCount(const DiffRunOptions& o);

}  // namespace rig
}  // namespace basalt

#endif  // BASALT_RIG_DIFFERENTIAL_DRIVER_H_
