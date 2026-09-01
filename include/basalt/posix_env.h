// PosixEnv: the production Env.
//
// READ SECTION 11 IDEALIZATION 9 BEFORE TRUSTING ANYTHING HERE.
//
//   PosixEnv is unverified by every lane in B1, and it is the component that
//   talks to the actual disk.
//
// Every B1 test runs on TestEnv, so the one piece of the engine whose behaviour
// B1 never checks is the piece every production durability claim runs THROUGH --
// and the piece B1's verification runs AROUND. The revert map lists B1.2b as the
// only unqualified leaf in the sequence, and that is not good news: it is a leaf
// precisely because nothing in B1 depends on it being right.
//
// WHAT ITS CORRECTNESS ACTUALLY RESTS ON, exactly and exhaustively:
//   (a) the thinness of this implementation -- each private Do* being a
//       mechanical mapping onto one syscall with no logic beyond a retry loop;
//   (b) the short-write, EINTR and zero-return unit tests at the raw-write seam;
//   and nothing else.
// There is no lane in B1 that would notice this file calling pwrite where it
// promised write, syncing the wrong descriptor, or dropping a flag.
//
// (a) IS ASSERTED HERE AND CHECKED AT B1.4, AND THE RULE IS STATED NOW.
//
// B1-Q12 was ruled as recommended: PosixEnv thinness becomes a checked scan
// rule, stated at B1.2b and enforced at B1.4, and B1.8's semantics suite runs
// against PosixEnv on a real filesystem -- the latter scoped explicitly as NOT
// evidence, injecting no faults, making no durability claim and never entering
// the recovery ledger, which is the condition it was approved under.
//
// (Section 13 of DESIGN-B1 revision 4 still prints B1-Q12 as open. The ruling
// postdates that revision and the document has not been revised since; the
// binding text is the ruling.)
//
// THE RULE, so that B1.4 enforces something written down rather than something
// remembered. Each private Do* below is a mechanical mapping onto ONE syscall:
//   * no branching beyond a documented retry loop;
//   * no state beyond the descriptor and the path;
//   * no arithmetic on offsets or lengths beyond what the public wrapper
//     passed;
//   * a per-method statement cap.
// Until B1.4 lands the scan rule, thinness is a property of this file that a
// reader can check and a lane cannot. That is a gap with a date on it, not an
// open question.
//
// The division of labour is still correct -- TestEnv is where fault injection
// belongs, and a fault-injecting production Env would be a second
// implementation of the thing under test. This is an idealization to state, not
// a defect to fix here. Under B1-Q12's ruling the first real evidence now
// arrives at B1.8 rather than at B5, still fault-free and still not evidence
// for the recovery contract; the first ADVERSARIAL evidence is I2's chaos lane.
#ifndef BASALT_POSIX_ENV_H_
#define BASALT_POSIX_ENV_H_

#include <memory>
#include <string>
#include <vector>

#include "basalt/env.h"
#include "basalt/fault_controller.h"
#include "basalt/posix_raw.h"

namespace basalt {

// The production Env. `faults` is NoFaultController() in production; the
// parameter exists because the kill mechanism is a property of the Env seam
// rather than of TestEnv, not because PosixEnv is ever meant to inject.
std::unique_ptr<Env> NewPosixEnv(FaultController* faults = NoFaultController(),
                                 posix::RawWriteFn raw_write = posix::RawWrite,
                                 posix::RawReadDirFn raw_readdir = posix::RawReadDir);

}  // namespace basalt

#endif  // BASALT_POSIX_ENV_H_
