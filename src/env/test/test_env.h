// TestEnv: the power-loss model, the fault matrix, the ledger, and the kill.
//
// THIS IS THRESHOLD 3 (DESIGN-B1 section 14.1). Before the content/durable
// split there is no observer that distinguishes WRITTEN from DURABLE, so no
// durability test can fail correctly -- it can only fail loudly. After it, a
// lane can go red for the right reason about durability.
//
// B1-D2, RULED: the POWER-LOSS model, and it is the single model every contract
// in section 7 is stated against.
//
//   Rejected (a), the process-crash model where the page cache survives and
//   durable == content always. That is what kill -9 actually does and it is
//   useless here: under it an unsynced write is never lost, which makes the
//   frozen contract's entire unsynced window untestable -- and it is GREEN,
//   because an engine that never synced would pass every one of its tests.
//
//   Rejected (c), both models selectable. A second model buys fidelity we do
//   not need at the price of a qualifier on every safety sentence, and
//   qualified contracts are the ones people misremember.
//
// The model, per file:
//
//   buf[]      appended, not yet flushed -- nothing has reached the device
//   content[]  what a reader sees now
//   durable[]  what a kill would leave
//
//   Append          buf grows                          content, durable unchanged
//   Flush           content += buf; buf cleared        durable unchanged
//   Sync   (clean)  content += buf; durable = content  ledger records the extent
//   Sync   (loss)   returns OK, durable unchanged      ledger records "lied"  [suspends exactness]
//   Sync   (torn)   durable = content[0 : k)           ledger records k; the call never returns
//   kill            content = durable; buf cleared; all handles closed
//
// A DIRECTORY ENTRY HAS THE SAME TWO STATES, and that is not decoration. A WAL
// created, written and fsynced is still losable if the entry naming it was
// never made durable: the bytes survive and the name does not. Section 7.2's
// gapless-file-number check is what turns that loss into a failed open instead
// of silence.
//
// THE SYMMETRY WITH engine/model IS THE POINT. The model keeps `durable` plus
// an ordered `pending` list and reverts on Crash(); TestEnv keeps `durable` plus
// the unsynced tail and reverts on kill. Two implementations of one idea, which
// is what makes disagreement between them mean something.
#ifndef BASALT_ENV_TEST_TEST_ENV_H_
#define BASALT_ENV_TEST_TEST_ENV_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "basalt/call_site.h"
#include "basalt/env.h"
#include "basalt/fault_controller.h"
#include "basalt/status.h"

namespace basalt {
namespace testenv {

// What TestEnv can do to an Env call. Closed; -Werror=switch, no default: arm.
enum class Injection : uint8_t {
  kNone,
  kIoError,       // the call fails; the implementation does not run
  kDiskFull,      // ENOSPC, quota exhausted
  kSyncLoss,      // Sync returns OK and promotes nothing -- the device lied
  // A kill INSIDE Sync: a PREFIX of the newly covered extent is promoted.
  //
  // THIS IS THE CONTRACT MODEL AND IT DOES NOT SUSPEND EXACTNESS. B1-D5 ruled
  // (a) prefix as the model every contract in section 7 is stated against, and
  // section 7.4's two-element set -- R in {G_{k-1}, G_k} -- is precisely this
  // case. The engine is held to exactness under it.
  kTornSync,
  // A kill inside Sync that promotes an ARBITRARY SUBSET of 4 KiB sectors of
  // the newly covered extent, not a prefix.
  //
  // THIS ONE SUSPENDS. It can promote a GROUP_END while leaving an earlier
  // record in the same group torn, which is a device that violated fsync's own
  // ordering guarantee. Against such a device the engine cannot be held to
  // exactness, and holding it there anyway would REPORT THE ENGINE FOR THE
  // DISK'S CRIME. Its obligation is narrower and still real: detect and refuse,
  // which section 5.4(d) already does.
  kSectorSubsetTornSync,
  kTornFlush,     // a kill INSIDE Flush: a prefix of the flushed extent lands
  kKill,          // the plain kill point
  // The call SUCCEEDS -- the effect lands, durably -- and the process dies
  // before the answer reaches the caller.
  //
  // This is section 7.4's second element and it cannot be expressed by an
  // injector that runs before the effect. "A Sync can complete on the device
  // with the kill preempting its return: the bytes are durable, the caller
  // never learned it." Recovery then lands on G_k while the harness's own
  // record shows only G_{k-1} promised, which is the direction that makes the
  // recovery set a SET rather than a value.
  kKillAfterEffect,
};
const char* InjectionName(Injection injection);

// THE EXACTNESS-SUSPENDING INJECTOR REGISTRY (section 7.5).
//
// Exactly one list, and both entries live in it. Adding a member here is the
// ONLY way to suspend assertion (ii); there is no per-injector flag anywhere
// else. Ruled: the sector-subset torn Sync gets the IDENTICAL treatment as the
// lying Sync through the SAME mechanism -- one suppression mechanism, two
// injectors, not two mechanisms that can drift apart, because two mechanisms
// that mean the same thing drift, and the one that drifts is the one nobody is
// looking at.
enum class ExactnessSuspendingInjector : uint8_t {
  kLyingSync,
  kSectorSubsetTornSync,
};

// True exactly for injections that are registry members. Enabling one sets the
// run's outcome at the POINT OF ENABLING, not at the point of reporting, so a
// run cannot be enabled into characterization mode and then summarized as
// something else.
bool SuspendsExactness(Injection injection);  // BASALT_EVIDENCE_DECIDER

// How a kill point kills (B1-D12, ruled: (c) -- (b) for the sweep, (a) for a
// stated sample, so the blind spot is measured rather than assumed).
enum class KillMode : uint8_t {
  // The fault controller sets a dead flag; every subsequent Env call is a no-op
  // returning kKilled and TestEnv freezes its durable image. Code that ignores
  // the Status can still only touch a frozen Env, so it cannot affect what
  // recovery reads -- the only dimension a crash has.
  //
  // -fno-exceptions rules out the obvious mechanism, and throw would be wrong
  // regardless: unwinding runs destructors, and a destructor that flushed would
  // write after the crash.
  kInProcess,
  // A real _exit(2). Maximally faithful -- no destructor runs, no heap
  // survives -- and it costs a full workload re-run per point, which is why it
  // is a sample and not the sweep.
  kRealExit,
};

// A cap on post-kill Env calls, so a caller that ignores every Status cannot
// spin forever inside a dead Env. Exceeded is a harness failure, not an engine
// one: it means the workload never checked a Status at all.
constexpr int kMaxPostKillCalls = 1024;

// Exit status a kRealExit kill leaves behind, so the parent can tell an
// intentional kill from a crash.
constexpr int kRealExitStatus = 97;

// A sector, for kSectorSubsetTornSync. 4 KiB is the unit a device promotes.
inline constexpr uint64_t kSectorBytes = 4096;

struct PlannedFault {
  Injection injection = Injection::kNone;
  // kTornSync / kTornFlush: how many of the newly covered bytes survive.
  // kSectorSubsetTornSync: which sector of the newly covered extent does NOT.
  uint64_t prefix_bytes = 0;
};

// Deterministic and total: an ordinal nobody named injects nothing. There is no
// randomness here and there must never be -- a fault schedule is authored, and
// a rig whose faults it cannot state is a rig whose failures it cannot replay.
class FaultPlan {
 public:
  void At(uint64_t ordinal, Injection injection, uint64_t prefix_bytes = 0);
  PlannedFault Lookup(uint64_t ordinal) const;
  bool Empty() const { return at_.empty(); }
  KillMode kill_mode = KillMode::kInProcess;

 private:
  std::map<uint64_t, PlannedFault> at_;
};

// One row per Env call, in issue order. HARNESS-SIDE, and never derived from
// anything the engine reports: the crash rig's verdicts come from its own
// record, because an oracle that interrogates the engine believes the lie.
struct LedgerEntry {
  uint64_t ordinal = 0;
  CallSite site = CallSite::kEnvNewWritableFile;
  std::string path;
  Injection injection = Injection::kNone;
  // For a Sync that returned successfully, the size of the durable image of
  // `path` afterwards. This is the harness's own record of what was promised.
  uint64_t durable_bytes_after = 0;
  bool promoted = false;  // did this call advance any durable image?
  // BYTES THIS CALL WAS ASKED TO WRITE, for an Append and zero otherwise.
  //
  // It is NOT `durable_bytes_after`, and the difference is what B3.7b's first
  // instrument got wrong: `durable_bytes_after` is the SIZE OF THE FILE once a
  // Sync has promoted it, so for an Append it is left at zero. Summing it over
  // appends produced a write amplification of 0.00 -- a number that cannot be
  // true, which is the only reason the broken instrument announced itself.
  //
  // Recorded here because write amplification is BYTES WRITTEN over BYTES
  // SUBMITTED, and the harness must count the first from its own record of what
  // the engine asked for rather than from anything the engine reports.
  uint64_t append_bytes = 0;
};

// Exactly what a kill leaves on disk: for every path whose directory entry is
// durable, its durable bytes. A file created and synced whose entry was never
// synced is ABSENT, because after the kill its name is gone.
using DurableImage = std::map<std::string, std::string>;

class TestEnvironment {
 public:
  explicit TestEnvironment(FaultPlan plan);
  TestEnvironment();
  ~TestEnvironment();

  Env* env() { return env_.get(); }

  // ---- harness-side observation. None of this asks the engine anything.
  const std::vector<LedgerEntry>& ledger() const;
  uint64_t ordinal() const;
  uint64_t observed(CallSite site) const;
  std::vector<CallSite> unvisited() const;
  bool dead() const;
  int post_kill_calls() const;

  // TRUE once a registry injector has been enabled, and set at the POINT OF
  // ENABLING rather than at the point of reporting.
  //
  // TestEnv reports the FACT and does not own the POLICY: rig/run_outcome.h's
  // OutcomeFloor turns this into kCharacterizationOnly, and
  // CountsAsRecoveryEvidence is the single site that decides what may be
  // banked. src/ must not be able to reach rig/ -- the oracle must not be
  // reachable from the thing it judges -- so the dependency runs one way and
  // this bool is the seam.
  bool exactness_suspended() const;
  // Which registry member suspended it. Only meaningful when suspended.
  ExactnessSuspendingInjector suspending_injector() const;

  // Exactly what a kill would leave. Callable before or after the kill; after,
  // it is frozen.
  DurableImage Image() const;

  // What a reader would see NOW -- the content view, as distinct from the
  // durable one. Harness-side observation of the MODEL, not of the engine: it
  // exists so `content = durable` on a kill is a checkable property rather than
  // a defensive line nothing can reach. After a kill the Env is dead and can no
  // longer be read through, so without this accessor that half of B1-D2's table
  // would be unobservable, and an unobservable line of a model is a line that
  // can rot.
  std::string ContentNow(const std::string& path) const;

  // Kill outside any Env call, for tests that want the state transition without
  // an injected fault.
  void Kill();

  // Called whenever the durable image CHANGES, and therefore called for the
  // last time immediately before a kRealExit kill takes the process down.
  //
  // It exists because of an asymmetry that is easy to miss: an in-process kill
  // leaves the durable image in memory for the rig to read, and a real _exit
  // does not. Something has to carry the image out of the process, and it must
  // not be TestEnv -- writing a file here would mean a syscall in
  // engine-cpp/src outside env/posix/, which is precisely what the A5 scope
  // scan bans. So TestEnv offers the image and the RIG owns the persistence.
  using PromotionHook = void (*)(void* ctx, const DurableImage& image);
  void set_promotion_hook(PromotionHook hook, void* ctx);

  // Seed a FRESH TestEnvironment from a frozen durable image -- which is how
  // the rig reopens: not by reusing this object, so a stale pointer faults
  // under ASan rather than silently working.
  static std::unique_ptr<TestEnvironment> FromImage(const DurableImage& image,
                                                    FaultPlan plan);

  class Impl;

 private:
  // Destruction order matters and is the reason these are in this order:
  // env_ dies first, then the controller it calls, then the model both read.
  std::unique_ptr<Impl> impl_;
  std::unique_ptr<FaultController> controller_;
  std::unique_ptr<Env> env_;
};

}  // namespace testenv
}  // namespace basalt

#endif  // BASALT_ENV_TEST_TEST_ENV_H_
