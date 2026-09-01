// TestEnv: threshold 3. A lane can now fail for the right reason about
// durability, because there is finally an observer that distinguishes WRITTEN
// from DURABLE.
//
// EVERY ASSERTION BELOW IS AGAINST THE HARNESS'S OWN RECORD -- the ledger, the
// fault plan the test itself wrote, the census -- and never against anything
// the engine reports about itself. That is not stylistic. "An oracle that
// interrogates the engine believes the lie" is the recorded sentence, and the
// place it is easiest to violate is exactly here, where the observer and the
// observed are both ours.
#include "test_env.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "basalt/call_site.h"
#include "basalt/env.h"
#include "run_outcome.h"

namespace basalt {
namespace testenv {
namespace {

const std::string kDir = "db";
const std::string kLog = "db/000001.log";
const std::string kLog2 = "db/000002.log";

// Creates the file and makes its directory entry durable, so later assertions
// are about CONTENT durability rather than about the name.
void CreateAndPublish(Env* env, const std::string& path, WritableFilePtr* w) {
  ASSERT_TRUE(env->NewWritableFile(path, w).ok());
  DirectoryPtr d;
  ASSERT_TRUE(env->NewDirectory(kDir, &d).ok());
  ASSERT_TRUE(d->Sync().ok());
  ASSERT_TRUE(d->Close().ok());
}

// The ledger entry for the last call at `path` carrying `site`.
const LedgerEntry* LastEntryFor(const TestEnvironment& t, const std::string& path,
                                CallSite site) {
  const LedgerEntry* found = nullptr;
  for (const LedgerEntry& e : t.ledger()) {
    if (e.path == path && e.site == site) found = &e;
  }
  return found;
}

// The harness's own answer to "what was the last thing promised for this path",
// computed from the ledger and from nothing else.
uint64_t LastPromisedBytes(const TestEnvironment& t, const std::string& path) {
  uint64_t promised = 0;
  for (const LedgerEntry& e : t.ledger()) {
    if (e.path == path && e.promoted) promised = e.durable_bytes_after;
  }
  return promised;
}

// -------------------------------------------------------- the power model

TEST(TestEnvDurability, KillDiscardsEverythingSinceTheLastSyncThatReturned) {
  TestEnvironment t;
  Env* env = t.env();
  WritableFilePtr w;
  CreateAndPublish(env, kLog, &w);

  ASSERT_TRUE(w->Append(Slice("AAAA", 4)).ok());
  ASSERT_TRUE(w->Flush().ok());
  ASSERT_TRUE(w->Sync().ok());          // this one returned: 4 bytes promised

  // The other direction of the same flag: a Sync that DID promote must say so,
  // or "promoted" would degenerate into a constant and stop distinguishing
  // anything at all.
  {
    const LedgerEntry* e = LastEntryFor(t, kLog, CallSite::kWritableFileSync);
    ASSERT_NE(e, nullptr);
    EXPECT_TRUE(e->promoted);
    EXPECT_EQ(e->injection, Injection::kNone);
    EXPECT_EQ(e->durable_bytes_after, 4u);
  }

  ASSERT_TRUE(w->Append(Slice("BBBB", 4)).ok());
  ASSERT_TRUE(w->Flush().ok());         // flushed, visible, NOT durable

  const uint64_t promised = LastPromisedBytes(t, kLog);
  ASSERT_EQ(promised, 4u);

  EXPECT_EQ(t.ContentNow(kLog), "AAAABBBB") << "both flushes are visible now";

  t.Kill();

  EXPECT_EQ(t.ContentNow(kLog), "AAAA")
      << "a kill sets content = durable; the flushed-but-unsynced tail is gone "
         "from the reader's view as well as from the disk";

  const DurableImage image = t.Image();
  ASSERT_EQ(image.count(kLog), 1u);
  EXPECT_EQ(image.at(kLog), "AAAA")
      << "a kill left more or less than the last Sync that returned had covered";
  EXPECT_EQ(image.at(kLog).size(), promised)
      << "the surviving bytes disagree with the harness's own record of what "
         "was promised -- which is the only record this assertion may consult";
}

TEST(TestEnvDurability, BufferedBytesNeverReachContentBeforeFlush) {
  TestEnvironment t;
  Env* env = t.env();
  WritableFilePtr w;
  CreateAndPublish(env, kLog, &w);
  ASSERT_TRUE(w->Append(Slice("AAAA", 4)).ok());

  uint64_t size = 99;
  ASSERT_TRUE(env->GetFileSize(kLog, &size).ok());
  EXPECT_EQ(size, 0u) << "Append reached the device; section 3.3 says it must not, "
                         "and PosixWritableFile agrees with that or B4's "
                         "differential rig compares the wrong thing";
}

TEST(TestEnvDurability, ATornSyncPromotesAPrefixAndTheCallNeverReturns) {
  FaultPlan plan;
  TestEnvironment probe;  // count the ordinals the workload will use
  (void)probe;
  TestEnvironment t0;
  {
    Env* env = t0.env();
    WritableFilePtr w;
    CreateAndPublish(env, kLog, &w);
    ASSERT_TRUE(w->Append(Slice("AAAABBBB", 8)).ok());
    // ordinal of the coming Sync is one past what has been issued so far.
    plan.At(t0.ordinal() + 1, Injection::kTornSync, /*prefix_bytes=*/3);
  }

  TestEnvironment t(plan);
  Env* env = t.env();
  WritableFilePtr w;
  CreateAndPublish(env, kLog, &w);
  ASSERT_TRUE(w->Append(Slice("AAAABBBB", 8)).ok());
  const Status s = w->Sync();
  EXPECT_EQ(s.code(), Status::Code::kKilled) << "a torn Sync's call never returns";
  EXPECT_TRUE(t.dead());

  const DurableImage image = t.Image();
  ASSERT_EQ(image.count(kLog), 1u);
  EXPECT_EQ(image.at(kLog), "AAA") << "exactly the promoted prefix survives";
  EXPECT_FALSE(t.exactness_suspended())
      << "a prefix-granular torn Sync is the CONTRACT MODEL, not a suspension: "
         "section 7.4's two-element set is this case, and a run that exercises "
         "it must remain bankable as evidence";
}

// ------------------------------------------------- the directory entry pair

TEST(TestEnvDurability, AFileWhoseNameWasNeverSyncedVanishesEntirely) {
  TestEnvironment t;
  Env* env = t.env();
  WritableFilePtr w;
  ASSERT_TRUE(env->NewWritableFile(kLog, &w).ok());   // no directory sync
  ASSERT_TRUE(w->Append(Slice("AAAA", 4)).ok());
  ASSERT_TRUE(w->Sync().ok());                        // the BYTES are durable

  t.Kill();
  const DurableImage image = t.Image();
  EXPECT_EQ(image.count(kLog), 0u)
      << "the bytes survived and the name did not, which is the whole reason "
         "Directory::Sync is not decoration -- and section 7.2's gapless check "
         "is what turns this loss into a failed open instead of silence";
}

TEST(TestEnvDurability, AnUnlinkThatWasNeverSyncedComesBack) {
  TestEnvironment t;
  Env* env = t.env();
  {
    WritableFilePtr w;
    CreateAndPublish(env, kLog, &w);
    ASSERT_TRUE(w->Append(Slice("AAAA", 4)).ok());
    ASSERT_TRUE(w->Sync().ok());
  }
  ASSERT_TRUE(env->DeleteFile(kLog).ok());
  bool exists = true;
  ASSERT_TRUE(env->FileExists(kLog, &exists).ok());
  EXPECT_FALSE(exists) << "the unlink is visible now";

  t.Kill();
  const DurableImage image = t.Image();
  EXPECT_EQ(image.count(kLog), 1u) << "an unlink is a directory-entry change and "
                                      "is not durable until the directory is synced";
  EXPECT_EQ(image.at(kLog), "AAAA");
}

// --------------------------------------------- the suspending-injector registry

TEST(TestEnvRegistry, ALyingSyncReturnsOkPromotesNothingAndSuspendsExactness) {
  TestEnvironment t0;
  FaultPlan plan;
  {
    Env* env = t0.env();
    WritableFilePtr w;
    CreateAndPublish(env, kLog, &w);
    ASSERT_TRUE(w->Append(Slice("AAAA", 4)).ok());
    plan.At(t0.ordinal() + 1, Injection::kSyncLoss);
  }
  TestEnvironment t(plan);
  Env* env = t.env();
  WritableFilePtr w;
  CreateAndPublish(env, kLog, &w);
  ASSERT_TRUE(w->Append(Slice("AAAA", 4)).ok());

  EXPECT_TRUE(w->Sync().ok()) << "the device lied: the caller must be unable to tell";

  // ASSERTED ON THE FLAG DIRECTLY, not through LastPromisedBytes.
  //
  // The first version of this test checked only the promised byte count, and
  // mutant LEDGER-always-promoted SURVIVED it: with the promotion suppressed,
  // durable_bytes_after is 0 whether the entry claims to have promoted or not,
  // so both fields agreed at zero and the flag was never under test. That is
  // the first of the three things a surviving mutant can mean -- a checker that
  // cannot see it -- and the response to that one is to strengthen the checker,
  // not to delete anything.
  const LedgerEntry* e = LastEntryFor(t, kLog, CallSite::kWritableFileSync);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->injection, Injection::kSyncLoss);
  EXPECT_FALSE(e->promoted)
      << "the ledger recorded a promotion for a Sync that promoted nothing; an "
         "oracle reading this ledger would be reading the engine's account of "
         "itself through a harness-shaped hole";
  EXPECT_EQ(LastPromisedBytes(t, kLog), 0u)
      << "the ledger records what happened, never what was reported";

  t.Kill();
  EXPECT_EQ(t.Image().count(kLog), 1u);
  EXPECT_EQ(t.Image().at(kLog), "") << "nothing was promoted";

  EXPECT_TRUE(t.exactness_suspended());
  EXPECT_EQ(t.suspending_injector(), ExactnessSuspendingInjector::kLyingSync);
  EXPECT_EQ(rig::OutcomeFloor(t.exactness_suspended()),
            rig::RunOutcome::kCharacterizationOnly);
  EXPECT_FALSE(rig::CountsAsRecoveryEvidence(
      rig::OutcomeFloor(t.exactness_suspended())));
}

// The second registry member, through the SAME mechanism, with no second flag
// existing anywhere. One suppression mechanism, two injectors -- because two
// mechanisms that mean the same thing drift apart, and the one that drifts is
// the one nobody is looking at.
TEST(TestEnvRegistry, TheSectorSubsetTornSyncSuspendsThroughTheSameMechanism) {
  TestEnvironment t0;
  FaultPlan plan;
  {
    Env* env = t0.env();
    WritableFilePtr w;
    CreateAndPublish(env, kLog, &w);
    ASSERT_TRUE(w->Append(Slice("AAAABBBB", 8)).ok());
    plan.At(t0.ordinal() + 1, Injection::kSectorSubsetTornSync, 0);
  }
  TestEnvironment t(plan);
  Env* env = t.env();
  WritableFilePtr w;
  CreateAndPublish(env, kLog, &w);
  ASSERT_TRUE(w->Append(Slice("AAAABBBB", 8)).ok());
  (void)w->Sync();

  EXPECT_TRUE(t.exactness_suspended());
  EXPECT_EQ(t.suspending_injector(),
            ExactnessSuspendingInjector::kSectorSubsetTornSync);
  EXPECT_FALSE(rig::CountsAsRecoveryEvidence(
      rig::OutcomeFloor(t.exactness_suspended())));
}

TEST(TestEnvRegistry, EveryInjectorIsClassifiedAndOnlyRegistryMembersSuspend) {
  const Injection all[] = {Injection::kNone,     Injection::kIoError,
                           Injection::kDiskFull, Injection::kSyncLoss,
                           Injection::kTornSync,
                           Injection::kSectorSubsetTornSync,
                           Injection::kTornFlush, Injection::kKill};
  int suspending = 0;
  for (Injection i : all) {
    EXPECT_NE(InjectionName(i), nullptr);
    if (SuspendsExactness(i)) ++suspending;
  }
  EXPECT_EQ(suspending, 2) << "the registry has exactly two members; a third "
                              "appearing without a decision is what the closed "
                              "switch in SuspendsExactness exists to prevent";
  // AND THE PREFIX-GRANULAR TORN SYNC IS NOT ONE OF THEM. B1-D5 ruled prefix as
  // THE CONTRACT MODEL: section 7.4's two-element set is that exact case and
  // the engine is held to exactness under it. This assertion exists because the
  // classification was got wrong once, in the conservative direction -- bankable
  // runs marked characterization-only, which would have made the two-element
  // set untestable as evidence at B1.9a.
  EXPECT_FALSE(SuspendsExactness(Injection::kTornSync));
  EXPECT_TRUE(SuspendsExactness(Injection::kSectorSubsetTornSync));
  EXPECT_TRUE(SuspendsExactness(Injection::kSyncLoss));
}

// ------------------------------------------------------------- the kill

TEST(TestEnvKill, EveryCallAfterTheKillIsANoOpAndTheImageIsFrozen) {
  TestEnvironment t;
  Env* env = t.env();
  WritableFilePtr w;
  CreateAndPublish(env, kLog, &w);
  ASSERT_TRUE(w->Append(Slice("AAAA", 4)).ok());
  ASSERT_TRUE(w->Sync().ok());

  const DurableImage before = t.Image();
  t.Kill();

  EXPECT_EQ(w->Append(Slice("XXXX", 4)).code(), Status::Code::kKilled);
  EXPECT_EQ(w->Flush().code(), Status::Code::kKilled);
  EXPECT_EQ(w->Sync().code(), Status::Code::kKilled);
  EXPECT_EQ(env->CreateDir("other").code(), Status::Code::kKilled);

  EXPECT_EQ(t.Image(), before)
      << "code that ignores the Status must still only be able to touch a "
         "frozen Env -- that is the only dimension a crash has";
}

TEST(TestEnvKill, AFreshEnvironmentIsSeededOnlyFromTheFrozenImage) {
  TestEnvironment t;
  {
    Env* env = t.env();
    WritableFilePtr w;
    CreateAndPublish(env, kLog, &w);
    ASSERT_TRUE(w->Append(Slice("AAAA", 4)).ok());
    ASSERT_TRUE(w->Sync().ok());
    ASSERT_TRUE(w->Append(Slice("BBBB", 4)).ok());
    ASSERT_TRUE(w->Flush().ok());
  }
  t.Kill();

  std::unique_ptr<TestEnvironment> reopened =
      TestEnvironment::FromImage(t.Image(), FaultPlan());
  SequentialFilePtr s;
  ASSERT_TRUE(reopened->env()->NewSequentialFile(kLog, &s).ok());
  char scratch[32];
  Slice r;
  ASSERT_TRUE(s->Read(sizeof scratch, &r, scratch).ok());
  EXPECT_EQ(r.ToString(), "AAAA")
      << "the reopened environment saw bytes that were not in the frozen image";
  EXPECT_EQ(reopened->ordinal(), 2u) << "reopening issued exactly the two calls "
                                        "this test made, so the fresh "
                                        "environment inherited no history";
}

// ----------------------------------------------------- ordering and order

TEST(TestEnvOrdering, GetChildrenIsReverseSortedOnPurpose) {
  TestEnvironment t;
  Env* env = t.env();
  for (const std::string& p : {kLog, kLog2, std::string("db/000003.log")}) {
    WritableFilePtr w;
    ASSERT_TRUE(env->NewWritableFile(p, &w).ok());
  }
  std::vector<std::string> kids;
  ASSERT_TRUE(env->GetChildren(kDir, &kids).ok());
  ASSERT_EQ(kids.size(), 3u);
  EXPECT_EQ(kids[0], "000003.log");
  EXPECT_EQ(kids[2], "000001.log")
      << "TestEnv hands back the worst legal order so an engine that forgot to "
         "sort by parsed file number fails on the first test rather than on "
         "someone else's filesystem";
}

// ------------------------------------------------------------ the census

// Drives every CallSite exactly once. If a call is added to or removed from
// this function, the pinned counts below must change with it -- which is what
// makes "an Env call nobody swept" visible instead of invisible.
void DriveEveryCallSiteOnce(Env* env) {
  ASSERT_TRUE(env->CreateDir(kDir).ok());

  WritableFilePtr w;
  ASSERT_TRUE(env->NewWritableFile(kLog, &w).ok());
  ASSERT_TRUE(w->Append(Slice("hello", 5)).ok());
  ASSERT_TRUE(w->Flush().ok());
  ASSERT_TRUE(w->Sync().ok());
  ASSERT_TRUE(w->Close().ok());

  DirectoryPtr d;
  ASSERT_TRUE(env->NewDirectory(kDir, &d).ok());
  ASSERT_TRUE(d->Sync().ok());
  ASSERT_TRUE(d->Close().ok());

  std::vector<std::string> kids;
  ASSERT_TRUE(env->GetChildren(kDir, &kids).ok());
  uint64_t size = 0;
  ASSERT_TRUE(env->GetFileSize(kLog, &size).ok());
  bool exists = false;
  ASSERT_TRUE(env->FileExists(kLog, &exists).ok());

  char scratch[16];
  Slice r;
  SequentialFilePtr s;
  ASSERT_TRUE(env->NewSequentialFile(kLog, &s).ok());
  ASSERT_TRUE(s->Read(sizeof scratch, &r, scratch).ok());
  ASSERT_TRUE(s->Close().ok());

  RandomAccessFilePtr ra;
  ASSERT_TRUE(env->NewRandomAccessFile(kLog, &ra).ok());
  ASSERT_TRUE(ra->Read(0, sizeof scratch, &r, scratch).ok());
  ASSERT_TRUE(ra->Close().ok());

  FileLockPtr lock;
  ASSERT_TRUE(env->LockFile("db/LOCK", &lock).ok());
  ASSERT_TRUE(env->UnlockFile(std::move(lock)).ok());

  ASSERT_TRUE(env->RenameFile(kLog, kLog2).ok());
  ASSERT_TRUE(env->DeleteFile(kLog2).ok());
}

// EVERY CALLSITE IS REACHABLE. A CallSite that exists and is never reached is
// an injector nobody can fire, which is A0.7's fire-count argument at the seam
// instead of at the network.
TEST(TestEnvCensus, EveryCallSiteIsReachedByAWorkloadThatExercisesEveryOperation) {
  TestEnvironment t;
  DriveEveryCallSiteOnce(t.env());

  const std::vector<CallSite> missed = t.unvisited();
  std::string names;
  for (CallSite s : missed) { names += " "; names += CallSiteName(s); }
  EXPECT_TRUE(missed.empty()) << "unreachable call sites:" << names;
}

// THE KILL-POINT CENSUS. The counts are pinned, not printed: a number nobody
// asserts on is decoration that looks like evidence. Adding an Env call and not
// updating this table is exactly the change section 9.5 says must surface, and
// here it surfaces as a red test naming the call and both numbers.
TEST(TestEnvCensus, PerCallSiteCountsAreExactlyWhatTheWorkloadIssued) {
  TestEnvironment t;
  DriveEveryCallSiteOnce(t.env());

  for (const CallSite s : AllCallSites()) {
    EXPECT_EQ(t.observed(s), 1u)
        << CallSiteName(s) << " was visited " << t.observed(s)
        << " times; the census workload issues exactly one of each";
  }
  EXPECT_EQ(t.ordinal(), AllCallSites().size())
      << "the total number of kill points the workload offers has changed";
  EXPECT_EQ(AllCallSites().size(), 22u)
      << "the Env surface changed size";
}

}  // namespace
}  // namespace testenv
}  // namespace basalt
