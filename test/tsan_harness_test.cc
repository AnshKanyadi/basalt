// The cpp-tsan lane's harness.
//
// THIS MAKES NO CLAIM ABOUT ANY ENGINE CODE, because at B1.1 there is none.
// Its only claim is that the lane is instrumented and actually runs threads:
// "a TSan lane over single-threaded tests is a green lane that proves nothing"
// (DESIGN-B1 section 6.4), and a TSan lane over no threads at all is worse.
//
// The engine's real concurrency harness -- Apply/Get on one thread against
// Sync on another -- lands at B1.5 with the memtable, together with
// kConcurrencyClaim, the single sanctioned wording of what such a lane
// establishes. No wording of that kind is landed here, deliberately: there is
// nothing yet for it to be about, and a claim that outruns its subject is the
// thing kConcurrencyClaim exists to prevent.
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace {

constexpr int kThreads = 4;
constexpr int kIncrementsPerThread = 20000;

// Under a lock, so TSan must report nothing. The canary patch that removes the
// lock is what proves TSan would report something if there were something to
// report -- without it, this test's green is indistinguishable from a lane that
// is not instrumented at all, which is why the static_assert in
// sanitizer_lane_test.cc exists as well. Two independent checks, because one of
// them can be defeated by an edit and the other cannot.
TEST(TSanLane, IsInstrumentedAndActuallyRunsThreads) {
  std::mutex mu;
  long counter = 0;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&mu, &counter] {
      for (int i = 0; i < kIncrementsPerThread; ++i) {
        std::lock_guard<std::mutex> guard(mu);
        ++counter;
      }
    });
  }
  for (std::thread& th : threads) th.join();
  EXPECT_EQ(counter, static_cast<long>(kThreads) * kIncrementsPerThread);
}

}  // namespace
