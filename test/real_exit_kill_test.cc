// The sampled real-_exit kill, and the measurement of the blind spot it exists
// to bound.
//
// B1-D12 was ruled (c): the in-process dead flag for the SWEEP, and a real
// _exit for a stated SAMPLE. (b) sweeps thousands of points per second and has
// one specific blind spot -- the engine keeps running, so a bug in which
// "recovery" reads live memory instead of disk could be masked. (a) is
// maximally faithful and costs a full workload re-run per point.
//
// Section 11 idealization 3 records the gap and says the sampled lane BOUNDS
// it. A branch that is never taken bounds nothing, so this test takes it: it
// runs one workload twice, once each way, and requires the durable images to be
// identical. That comparison is the measurement. Without it, KillMode::kRealExit
// would be unreachable code -- and unreachable code is the one of the three
// meanings of a surviving mutant whose correct response is deletion.
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <string>

#include <gtest/gtest.h>

#include "durable_mirror.h"
#include "basalt/env.h"
#include "test_env.h"

namespace basalt {
namespace {

using testenv::DurableImage;
using testenv::FaultPlan;
using testenv::Injection;
using testenv::KillMode;
using testenv::TestEnvironment;

const std::string kDir = "db";
const std::string kLog = "db/000001.log";

// Two syncs, then unsynced bytes, then the kill. The durable image must hold
// exactly what the SECOND Sync covered, and nothing after it.
void Workload(Env* env) {
  WritableFilePtr w;
  if (!env->NewWritableFile(kLog, &w).ok()) return;
  DirectoryPtr d;
  if (!env->NewDirectory(kDir, &d).ok()) return;
  if (!d->Sync().ok()) return;

  if (!w->Append(Slice("AAAA", 4)).ok()) return;
  if (!w->Sync().ok()) return;
  if (!w->Append(Slice("BBBB", 4)).ok()) return;
  if (!w->Sync().ok()) return;
  if (!w->Append(Slice("CCCC", 4)).ok()) return;
  if (!w->Flush().ok()) return;
  // The kill lands on the Env call that follows.
  bool exists = false;
  (void)env->FileExists(kLog, &exists);
}

// The ordinal of the FileExists at the end of Workload, discovered by running
// it once with no faults rather than counted by hand -- a hand-counted ordinal
// is a number that goes stale the first time the workload changes and takes the
// test's meaning with it.
uint64_t KillOrdinal() {
  TestEnvironment probe;
  Workload(probe.env());
  return probe.ordinal();
}

DurableImage RunInProcess(uint64_t kill_ordinal) {
  FaultPlan plan;
  plan.kill_mode = KillMode::kInProcess;
  plan.At(kill_ordinal, Injection::kKill);
  TestEnvironment t(plan);
  Workload(t.env());
  EXPECT_TRUE(t.dead());
  return t.Image();
}

TEST(RealExitKill, MatchesTheInProcessKillOnTheSameWorkloadAndKillPoint) {
  const uint64_t kill_ordinal = KillOrdinal();
  ASSERT_GT(kill_ordinal, 0u);

  const DurableImage in_process = RunInProcess(kill_ordinal);
  ASSERT_EQ(in_process.count(kLog), 1u);
  ASSERT_EQ(in_process.at(kLog), "AAAABBBB")
      << "the in-process kill is the reference for this comparison, so it has "
         "to be right before the comparison means anything";

  const std::string mirror = std::string(::getenv("TMPDIR") ? ::getenv("TMPDIR") : "/tmp") +
                             "/basalt-real-exit-mirror";
  ::unlink(mirror.c_str());

  const pid_t pid = ::fork();
  ASSERT_GE(pid, 0) << "fork failed";
  if (pid == 0) {
    // CHILD. Nothing here may return: a child that falls out of this block
    // would run the rest of the test binary a second time.
    FaultPlan plan;
    plan.kill_mode = KillMode::kRealExit;
    plan.At(kill_ordinal, Injection::kKill);
    TestEnvironment t(plan);
    std::string path = mirror;
    t.set_promotion_hook(&rig::MirrorHook, &path);
    Workload(t.env());
    // Only reachable if the kill point was never hit.
    ::_exit(1);
  }

  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status)) << "the child did not exit normally";
  ASSERT_EQ(WEXITSTATUS(status), testenv::kRealExitStatus)
      << "the child exited " << WEXITSTATUS(status)
      << "; the kill point was not reached, so nothing below is about a kill";

  DurableImage after_exit;
  ASSERT_TRUE(rig::ReadDurableMirror(mirror, &after_exit))
      << "no durable image survived the _exit";
  ::unlink(mirror.c_str());

  EXPECT_EQ(after_exit, in_process)
      << "a real _exit and the in-process dead flag left different durable "
         "images for the same workload at the same kill point. The sweep runs "
         "on the in-process kill, so this difference is the size of the blind "
         "spot section 11 idealization 3 records -- and it is supposed to be "
         "zero";
}

}  // namespace
}  // namespace basalt
