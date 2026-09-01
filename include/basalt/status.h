// Status: the engine's only error channel.
#ifndef BASALT_STATUS_H_
#define BASALT_STATUS_H_

#include <cstdint>
#include <string>
#include <utility>

#include "basalt/check.h"

namespace basalt {

// [[nodiscard]] IS THE ANSWER TO A COST WE TOOK ON DELIBERATELY.
//
// Wal::Open refuses an unordered cap pair with a Status rather than an abort,
// because section 10.2's induced failure has to be OBSERVED and a path whose
// only outcome is abort() is observable in this suite only through a death test
// -- which forks and behaves differently under three of our four sanitizer
// lanes. Choosing inducibility over conventional strictness is the same trade
// Track A made when it refused checkers it could not see fail.
//
// THE COST, STATED: a Status can be ignored by a caller where an abort cannot,
// so the thing that proves no caller ignores it is now load-bearing. This
// attribute is that thing, and it is a COMPILE ERROR under -Werror rather than
// a lane: a dropped Status stops being a code review question.
//
// Deliberately discarding one is spelled `(void)` at the call site, which is a
// visible, greppable act rather than an omission.
class [[nodiscard]] Status {
 public:
  // THE CLOSED ENUM (DESIGN-B1 section 7.6 clause 1).
  //
  // engine/model never errors. Every code here is therefore a place where the
  // two engines can legally differ, and every one must be adjudicated
  // harness-side: each carries a predicate computed from the harness's own
  // submission log, its reference state and TestEnv's ledger, NEVER from the
  // engine's report. An engine error whose predicate is not satisfied is a
  // divergence; a satisfied predicate with no error is also a divergence.
  //
  // Clause 6, which is the acceptance test for adding a code at all: the
  // predicate must be statable in BOTH directions. A code whose predicate is
  // one-directional makes clause 4 vacuous for that code and reopens the
  // escape hatch under a new name, so it does not get added.
  //
  // kBusy LANDED AT B5.3, under section 7.6.1's precondition and not around it.
  // Its predicate is stated in both directions below and BOTH ARE INDUCED; the
  // rig that asserts it DRIVES the poller rather than observing one, because a
  // rig that can only observe can never construct the negative direction. That
  // was known to be strictly more work when B1 ruled it and it was paid: the
  // alternative is an oracle asking the engine whether it was justified.
  //
  // NO `default:` ARM MAY EVER SWITCH OVER THIS TYPE. -Werror=switch is what
  // makes adding an enumerator a build failure until somebody classifies it.
  enum class Code : uint8_t {
    kOk = 0,
    // A normal result, not a divergence: engine/model produces it too, so it
    // is not a place the engines can legally differ and it carries no
    // predicate. It is the frozen interface's ErrNotFound.
    kNotFound,
    kRecordTooLarge,    // predicate: record_bytes(submitted) > max_record_bytes
    kWalBufferFull,     // predicate: sum since last Sync start > wal_buffer_bytes
    kIoError,           // predicate: TestEnv's ledger shows an injected IO error
    kDiskFull,          // predicate: TestEnv's quota ledger shows exhaustion
    kCorruption,        // predicate: harness planted corruption in a region read
    kKilled,            // predicate: the fault controller's dead flag is set
    kInvalidArgument,   // predicate: harness submitted outside the frozen contract
    // BOTH DIRECTIONS, AND THE QUANTITY IS THE HARNESS'S OWN, never ours:
    //   owed = (bytes the harness submitted) - (bytes its poller has drained)
    //   spurious: engine returned kBusy while owed + this batch <= busy_bytes
    //   missing : owed + this batch > busy_bytes and the engine returned kOk
    kBusy,              // predicate: owed + record_bytes(submitted) > busy_bytes
  };

  Status() = default;  // kOk, and no allocation

  static Status Ok() { return Status(); }

  // Make refuses kOk: an "error" carrying kOk is a caller who has not decided
  // what happened, and it would read as success at every call site.
  static Status Make(Code code, std::string message) {
    BASALT_CHECK(code != Code::kOk);
    return Status(code, std::move(message));
  }

  static Status NotFound(std::string m)        { return Make(Code::kNotFound, std::move(m)); }
  static Status RecordTooLarge(std::string m)  { return Make(Code::kRecordTooLarge, std::move(m)); }
  static Status WalBufferFull(std::string m)   { return Make(Code::kWalBufferFull, std::move(m)); }
  static Status Busy(std::string m)            { return Make(Code::kBusy, std::move(m)); }
  static Status IoError(std::string m)         { return Make(Code::kIoError, std::move(m)); }
  static Status DiskFull(std::string m)        { return Make(Code::kDiskFull, std::move(m)); }
  static Status Corruption(std::string m)      { return Make(Code::kCorruption, std::move(m)); }
  static Status Killed(std::string m)          { return Make(Code::kKilled, std::move(m)); }
  static Status InvalidArgument(std::string m) { return Make(Code::kInvalidArgument, std::move(m)); }

  bool ok() const { return code_ == Code::kOk; }
  Code code() const { return code_; }
  const std::string& message() const { return message_; }

  std::string ToString() const;

 private:
  Status(Code code, std::string message)
      : code_(code), message_(std::move(message)) {}

  Code code_ = Code::kOk;
  std::string message_;
};

// Total over Status::Code by construction: the switch inside has no `default:`
// arm, so a new enumerator is a build failure here before it is anything else.
const char* CodeName(Status::Code code);

}  // namespace basalt

#endif  // BASALT_STATUS_H_
