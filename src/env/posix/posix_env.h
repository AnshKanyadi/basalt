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
// (a) IS CURRENTLY ASSERTED RATHER THAN CHECKED, and it is the load-bearing
// half. B1-Q12 asks whether making thinness a scan rule is cheap enough to land
// at B1; the recommendation is yes, enforced at B1.4. It is OPEN, and adding it
// would add rows to section 10.2, which is not this session's to extend
// unruled. So the code below is written thin ON PURPOSE and that property is
// unenforced ON PURPOSE, and this paragraph is the record of which it is.
//
// The division of labour is still correct -- TestEnv is where fault injection
// belongs, and a fault-injecting production Env would be a second
// implementation of the thing under test. This is an idealization to state, not
// a defect to fix here. First real evidence arrives at B5 on a real
// filesystem; first ADVERSARIAL evidence at I2's chaos lane.
#ifndef RIFT_ENV_POSIX_POSIX_ENV_H_
#define RIFT_ENV_POSIX_POSIX_ENV_H_

#include <memory>
#include <string>
#include <vector>

#include "env.h"
#include "fault_controller.h"
#include "posix_raw.h"

namespace rift {

// The production Env. `faults` is NoFaultController() in production; the
// parameter exists because the kill mechanism is a property of the Env seam
// rather than of TestEnv, not because PosixEnv is ever meant to inject.
std::unique_ptr<Env> NewPosixEnv(FaultController* faults = NoFaultController(),
                                 posix::RawWriteFn raw = posix::RawWrite);

}  // namespace rift

#endif  // RIFT_ENV_POSIX_POSIX_ENV_H_
