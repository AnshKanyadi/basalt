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
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "concurrency_claim.h"
#include "basalt/db.h"
#include "memtable.h"
#include "basalt/posix_env.h"
#include "basalt/slice.h"

namespace basalt {
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

// THE SECOND PATTERN, AND THE ONE B2 EARNED. The flush REPLACES the WAL and the
// memtable underneath whatever else is running: a Write that reads the old WAL
// and then applies to the new memtable puts the record in a log about to be
// deleted and the data in a memtable the flushed table does not contain. A LOST
// WRITE, with no corruption anywhere and nothing to see afterwards.
//
// It runs against the PRODUCTION Env, not TestEnv, because TestEnv's fault
// controller keeps a single ordinal counter and is not built for concurrent
// callers -- driving it from two threads would be testing the harness. The
// directory is created and removed by the test.
//
// One writer and one syncer, matching the shape the frozen interface forces:
// Apply runs on the node loop while a separate thread owns the blocking Sync.
// Not more, because more would be a claim the contract does not make.
TEST(TSanLane, ConcurrentWriteAndSyncAcrossAFlushAreRaceFreeAcrossThisInterleaving) {
  const std::string dir = "tsan-flush-db";
  std::unique_ptr<Env> env = NewPosixEnv();
  (void)env->CreateDir(dir);
  {
    std::vector<std::string> stale;
    if (env->GetChildren(dir, &stale).ok()) {
      for (const std::string& c : stale) (void)env->DeleteFile(dir + "/" + c);
    }
  }

  wal::Caps caps;
  caps.flush_bytes = 16u * 1024;  // low enough that this workload flushes often
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(env.get(), dir, caps, &db).ok());

  std::atomic<bool> done{false};
  std::atomic<int> written{0};
  const std::string filler(256, 'y');

  std::thread writer([&db, &done, &written, &filler] {
    for (int i = 0; i < 3000; ++i) {
      char key[24];
      std::snprintf(key, sizeof key, "k%06d", i);
      WriteBatch b;
      const std::string k(key);
      b.Set(Slice(k), Slice(filler));
      wal::SeqNum s = 0;
      if (!db->Write(b, &s).ok()) break;
      written.fetch_add(1);
    }
    done.store(true);
  });
  std::thread syncer([&db, &done] {
    while (!done.load()) {
      wal::SeqNum mark = 0;
      if (!db->Sync(&mark).ok()) break;
    }
    wal::SeqNum mark = 0;
    (void)db->Sync(&mark);
  });
  writer.join();
  syncer.join();

  // EVERY WRITE THE WRITER ACKNOWLEDGED IS STILL READABLE. That is the
  // assertion the lost-write race would break, and it is deterministic --
  // unlike anything about WHICH interleaving occurred, which is TSan's job to
  // observe and not this test's to assert.
  const int n = written.load();
  ASSERT_GT(n, 0);
  for (int i = 0; i < n; ++i) {
    char key[24];
    std::snprintf(key, sizeof key, "k%06d", i);
    const std::string k(key);
    std::string v;
    EXPECT_TRUE(db->Get(Slice(k), &v).ok()) << "acknowledged write lost: " << k;
  }
  ASSERT_TRUE(db->Close().ok());
  db.reset();
  std::vector<std::string> children;
  if (env->GetChildren(dir, &children).ok()) {
    for (const std::string& c : children) (void)env->DeleteFile(dir + "/" + c);
  }
}

}  // namespace
}  // namespace basalt
