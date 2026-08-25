// The cpp-tsan lane's harness.
//
// TSan is required regardless of the lock, because a locked structure with a
// WRONG lock is still a race. And "a TSan lane over single-threaded tests is a
// green lane that proves nothing" (DESIGN-B1 section 6.4) -- so this lane runs
// this harness and not the ordinary unit suite.
//
// WHAT IT ESTABLISHES IS EXACTLY kConcurrencyClaim AND NOTHING WIDER. One
// authored interleaving pattern. Not a search. TSan reports the races it
// observes, not the ones that exist. The sentence lives in one constant, is
// printed below, and is pinned by a test, so strengthening it requires failing
// that test -- and the rule is that the harness must be strengthened in the
// same diff that strengthens the claim.
//
// BM14 removes the lock from the write path and this harness must report a
// race. That is what proves the lane is not decoration; without it, green here
// is indistinguishable from a lane that is not instrumented at all, which is
// why sanitizer_lane_test.cc separately asserts at COMPILE TIME that TSan is
// on. Two independent checks, because one of them can be defeated by an edit
// and the other cannot.
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "concurrency_claim.h"
#include "memtable.h"
#include "slice.h"

namespace rift {
namespace {

constexpr int kWriters = 2;
constexpr int kReaders = 2;
constexpr int kOpsPerThread = 4000;

std::string KeyAt(int i) { return "key" + std::to_string(i % 512); }

// Concurrent Add against concurrent Get, which is the shape the frozen
// interface forces: Apply runs on the node loop while a separate thread owns
// the blocking Sync, so the engine IS called from two threads and must be
// internally synchronized. B1-D6c ruled that synchronization is a mutex.
TEST(TSanLane, ConcurrentAddAndGetAreRaceFreeAcrossThisInterleaving) {
  std::printf("  claim: %s\n", kConcurrencyClaim);

  MemTable m;
  std::atomic<uint64_t> next_seq{1};
  std::vector<std::thread> threads;
  threads.reserve(kWriters + kReaders);

  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&m, &next_seq, w] {
      for (int i = 0; i < kOpsPerThread; ++i) {
        const std::string k = KeyAt(w * 7919 + i);
        const std::string v = "v" + std::to_string(i);
        m.Add(next_seq.fetch_add(1), ValueType::kValue, Slice(k), Slice(v));
      }
    });
  }
  for (int r = 0; r < kReaders; ++r) {
    threads.emplace_back([&m, &next_seq, r] {
      std::string value;
      for (int i = 0; i < kOpsPerThread; ++i) {
        const std::string k = KeyAt(r * 104729 + i);
        (void)m.Get(Slice(k), next_seq.load(), &value);
      }
    });
  }
  for (std::thread& t : threads) t.join();

  // The count is the only thing asserted, and deliberately so: WHAT the readers
  // saw depends on the interleaving and asserting it would make this test
  // flaky, which is how a concurrency lane earns a retry loop and stops meaning
  // anything. TSan is the observer here; the assertion is only that the writers
  // all landed.
  EXPECT_EQ(m.Count(), static_cast<std::size_t>(kWriters) * kOpsPerThread);
}

}  // namespace
}  // namespace rift
