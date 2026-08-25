// FaultController: the thing every Env call passes through.
//
// It lives in its own header, OUTSIDE env.h's marked surface region, and that
// placement is deliberate: the region is scanned by a rule that rejects any
// public virtual (scripts/cpp-scan.sh), and Intercept is exactly that. Keeping
// it here means the rule can stay absolute rather than acquiring an exception,
// and an exception in an enforcement rule is where enforcement goes to die.
#ifndef RIFT_ENV_FAULT_CONTROLLER_H_
#define RIFT_ENV_FAULT_CONTROLLER_H_

#include "call_site.h"
#include "env_handle.h"
#include "status.h"

namespace rift {

class FaultController {
 public:
  virtual ~FaultController();

  // Called at EVERY Env call, before the implementation runs, from the
  // non-virtual public wrapper -- so an implementation physically cannot skip
  // it. A non-ok return means the implementation is NOT invoked and the caller
  // gets that Status instead.
  //
  // After a kill, this returns Status::kKilled for every subsequent call and
  // TestEnv freezes its durable image: code that ignores the Status can still
  // only touch a frozen Env, so it cannot affect what recovery reads -- the
  // only dimension a crash has (DESIGN-B1 section 9.5).
  //
  // `handle` identifies the file handle for handle-scoped injection. It is an
  // INTEGER and not a pointer, for two reasons that both matter.
  //
  // This interface must not be able to reach into the object it is
  // intercepting -- an integer cannot. And section 6.1 bans anything that
  // depends on an address: a controller keeping a pointer-to-path map is a
  // pointer-keyed container, and the day someone iterates it the fault schedule
  // starts depending on the allocator. Ids are assigned sequentially by the Env
  // that created the handle, so the same workload produces the same ids on
  // every run and on every machine.
  virtual Status Intercept(CallSite site, HandleId handle) = 0;
};

// The controller that injects nothing. PosixEnv uses it; TestEnv (B1.3) does
// not. It is stateless, so it is a singleton without being a variable anyone
// can write to.
FaultController* NoFaultController();

}  // namespace rift

#endif  // RIFT_ENV_FAULT_CONTROLLER_H_
