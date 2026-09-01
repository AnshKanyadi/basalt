// HandleId, in its own header so fault_controller.h can name it without
// including the whole Env surface -- and so env.h can include fault_controller.h
// without a cycle.
#ifndef RIFT_ENV_ENV_HANDLE_H_
#define RIFT_ENV_ENV_HANDLE_H_

#include <cstdint>

namespace rift {

// The identity of an Env handle, for handle-scoped fault injection and for the
// ledger.
//
// IT IS AN INTEGER AND NOT AN ADDRESS, and that is section 6.1's rule rather
// than a preference: "nothing may depend on an address -- no pointer-keyed
// containers, no address-ordered anything", the C++ restatement of the
// map-iteration rule. A controller that mapped `this` to a path would be a
// pointer-keyed container, and the day someone iterated it the fault schedule
// would depend on the allocator.
//
// It is also a better bug report. "kill 47 = Sync(handle 3, 000001.log)" is a
// sentence; "kill 47 = Sync(0x7f9c4a005e10)" is a number that means nothing on
// the second run.
struct HandleId {
  uint64_t value = 0;
};
inline bool operator==(HandleId a, HandleId b) { return a.value == b.value; }
inline bool operator<(HandleId a, HandleId b) { return a.value < b.value; }

}  // namespace rift

#endif  // RIFT_ENV_ENV_HANDLE_H_
