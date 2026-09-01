// A PRECONDITION THAT SAYS "ONE CALLER AT A TIME", AND FAILS AT THE MISTAKE.
//
// It is a named type rather than three lines inside `Sync` for one reason: a
// guard nobody can construct in a test is a guard nobody has induced, and this
// repo's rule is that NO GATE COUNTS UNTIL ITS FAILURE HAS BEEN INDUCED AND
// OBSERVED. Two threads racing a real `Sync` would induce it only PROBABLY;
// constructing this twice induces it every time.
//
// WHY IT IS AN ABORT AND NOT A Status. The caller is a program, not a file. A
// second concurrent `Sync` is a bug in the code calling the engine, and the
// useful place to stop is at the call rather than at the next Open, which would
// be reading a manifest with two groups interleaved into one another and would
// report only that the bytes make no sense.
#ifndef BASALT_SINGLE_CALLER_H_
#define BASALT_SINGLE_CALLER_H_

#include <atomic>

#include "basalt/check.h"

namespace basalt {

// The flag lives with the object being protected; this is the scoped claim on
// it. Re-entrancy on ONE thread trips it too, and that is wanted: a re-entrant
// Sync would append to the manifest from inside its own append.
class SingleCaller {
 public:
  explicit SingleCaller(std::atomic<bool>* held) : held_(held) {
    BASALT_CHECK(!held_->exchange(true));
  }
  ~SingleCaller() { held_->store(false); }

  SingleCaller(const SingleCaller&) = delete;
  SingleCaller& operator=(const SingleCaller&) = delete;

 private:
  std::atomic<bool>* held_;
};

}  // namespace basalt

#endif  // BASALT_SINGLE_CALLER_H_
