// The raw-write seam, driven with the three things a real write(2) does that a
// local filesystem almost never does on a developer's machine.
//
// This is the ENTIRE verified surface of PosixEnv (section 11 idealization 9,
// clause (b)). Everything else about the production Env rests on the thinness
// of the implementation, which is currently asserted rather than checked --
// B1-Q12, open. So these three tests are not a formality; they are one of the
// two legs the whole production durability story stands on.
//
// THE HARNESS BOUNDS THE LOOP, NOT THE SUBJECT. FakeWriter refuses to be called
// more than kHarnessCallCap times and records that it refused. A WriteFully
// that fails to terminate therefore fails a test instead of hanging a lane --
// which matters because "must not spin" is a liveness property, and a lane that
// hangs while checking a liveness property reports nothing at all.
#include "posix_raw.h"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "status.h"

namespace rift {
namespace posix {
namespace {

constexpr int kHarnessCallCap = 1000;

// A deterministic write(2). No randomness anywhere: A5's no-ambient-randomness
// rule is a property of the harness as much as of the engine, and a seam test
// that failed one run in fifty would be worse than no seam test.
//
// schedule[i] is what call i does:  >0 write at most that many bytes,
//                                   -1 fail with EINTR,
//                                    0 return zero without an error.
// When the schedule runs out its LAST entry repeats, which is what lets a
// one-element schedule express "forever".
struct FakeWriter {
  std::vector<int> schedule;
  std::string written;
  int calls = 0;
  bool overran = false;
};

FakeWriter* g_fake = nullptr;

ssize_t FakeWrite(int, const void* buf, std::size_t n) {
  FakeWriter* f = g_fake;
  if (f->calls >= kHarnessCallCap) {
    f->overran = true;
    errno = EIO;
    return -1;
  }
  const std::size_t i = static_cast<std::size_t>(f->calls);
  const int action = f->schedule[i < f->schedule.size() ? i : f->schedule.size() - 1];
  f->calls++;
  if (action < 0) { errno = EINTR; return -1; }
  if (action == 0) return 0;
  const std::size_t k = n < static_cast<std::size_t>(action) ? n : static_cast<std::size_t>(action);
  f->written.append(static_cast<const char*>(buf), k);
  return static_cast<ssize_t>(k);
}

std::string Payload(std::size_t n) {
  std::string s;
  for (std::size_t i = 0; i < n; ++i) s.push_back(static_cast<char>('A' + (i % 26)));
  return s;
}

class RawWriteTest : public ::testing::Test {
 protected:
  void SetUp() override { g_fake = &fake_; }
  void TearDown() override { g_fake = nullptr; }
  FakeWriter fake_;
};

// ------------------------------------------------------------ short writes
//
// The defect this catches: a loop that adds n instead of the returned count, or
// does not loop at all. On a local filesystem write(2) essentially always
// returns the full count, so such a loop is correct on every machine it is ever
// run on until the day it is not -- and the bytes it silently drops are WAL
// bytes that were reported durable.

TEST_F(RawWriteTest, ResumesFromTheReturnedCountOneByteAtATime) {
  const std::string data = Payload(8);
  fake_.schedule = {1};  // one byte per call, forever
  Status s = WriteFully(0, data.data(), data.size(), FakeWrite);
  ASSERT_TRUE(s.ok()) << s.ToString();
  EXPECT_FALSE(fake_.overran);
  EXPECT_EQ(fake_.written, data) << "bytes were dropped, duplicated or reordered";
  EXPECT_EQ(fake_.calls, 8) << "eight one-byte writes is eight calls; a different "
                               "number means the loop is not resuming from the "
                               "count the syscall returned";
}

TEST_F(RawWriteTest, ResumesCorrectlyAcrossVaryingShortCounts) {
  const std::string data = Payload(8);
  fake_.schedule = {1, 3, 2};  // then 2 repeats: 1 + 3 + 2 + 2 = 8
  Status s = WriteFully(0, data.data(), data.size(), FakeWrite);
  ASSERT_TRUE(s.ok()) << s.ToString();
  EXPECT_EQ(fake_.written, data);
  EXPECT_EQ(fake_.calls, 4);
}

// ------------------------------------------------------------------ EINTR
//
// The defect this catches: treating an interrupted call as a failure. EINTR
// means nothing happened and the call should be repeated; reporting it as an
// IO error turns a signal into a lost write, and a signal is not rare in a
// process that has a poller thread in it (B5).

TEST_F(RawWriteTest, RetriesOnEintrWithoutLosingBytes) {
  const std::string data = Payload(8);
  fake_.schedule = {-1, -1, 4, -1, 4};
  Status s = WriteFully(0, data.data(), data.size(), FakeWrite);
  ASSERT_TRUE(s.ok()) << s.ToString();
  EXPECT_FALSE(fake_.overran);
  EXPECT_EQ(fake_.written, data);
  EXPECT_EQ(fake_.calls, 5) << "an EINTR must cost a retry and nothing else";
}

// ------------------------------------------------------------ zero returns
//
// The defect this catches is a liveness one: a write(2) that returns 0 without
// an error wrote nothing and gave no reason, and retrying it forever is a
// process that is not wrong, not finished, and not making progress. In a
// kill-point sweep the only symptom would be a run that never ends.

TEST_F(RawWriteTest, GivesUpOnRepeatedZeroReturnsInsteadOfSpinning) {
  const std::string data = Payload(8);
  fake_.schedule = {0};  // zero, forever
  Status s = WriteFully(0, data.data(), data.size(), FakeWrite);
  EXPECT_FALSE(s.ok()) << "a write that never writes must be reported, not awaited";
  EXPECT_EQ(s.code(), Status::Code::kIoError);
  EXPECT_FALSE(fake_.overran) << "WriteFully did not terminate on its own; the "
                                 "harness had to stop it";
  EXPECT_EQ(fake_.calls, kMaxConsecutiveZeroWrites);
}

// And the converse, which is the half that is easy to get wrong in the other
// direction: a zero return followed by progress is not an error at all, so the
// counter has to reset. A loop that counted zeros cumulatively would fail a
// long, slow, entirely healthy write.
TEST_F(RawWriteTest, ZeroReturnsThatMakeProgressAreNotFatal) {
  const std::string data = Payload(8);
  fake_.schedule = {0, 0, 4, 0, 0, 4};
  Status s = WriteFully(0, data.data(), data.size(), FakeWrite);
  ASSERT_TRUE(s.ok()) << s.ToString();
  EXPECT_EQ(fake_.written, data);
  EXPECT_EQ(fake_.calls, 6);
}

}  // namespace
}  // namespace posix
}  // namespace rift
