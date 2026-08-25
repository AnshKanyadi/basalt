// The four lane canaries.
//
//   B1.1 is the ALIVE-canary moment: "required lane" becomes "lane that can
//   fail". Every green between B1.1 and B1.9 is uninterpretable without it.
//                                              -- DESIGN-B1 section 14.1
//
// Track A learned the expensive half of this the same week: a lane stayed green
// across five checklist steps while the machinery underneath ran at a sixth of
// its power, and nothing noticed, because a green lane reports the health of
// the lane and not the power of the harness behind it.
//
// Four lanes is four claims, and one red is not evidence for four. So each
// operation below is the CORRECT version of something exactly one lane can
// catch when it is wrong, and the patches in engine-cpp/mutants/ break one
// each. scripts/cpp-mutants.sh requires the named lane to go red AND -- for the
// three sanitizer canaries -- requires the uninstrumented cpp-test lane to stay
// green, which is what makes the red attributable to the sanitizer rather than
// to a patch that simply broke the build.
//
// These are not dead fixtures. Each function is exercised by a real assertion
// below; the patches change what they do, not whether they run.
#include <cstdint>

#include <gtest/gtest.h>

namespace {

// cpp-test's canary target: an ordinary assertion of ours.
int Doubled(int x) { return x * 2; }

// cpp-asan's canary target: a heap read that is IN BOUNDS. LANE-cpp-asan moves
// it one byte past the end -- which an uninstrumented build reads without
// complaint, and which neither UBSan nor TSan looks for.
//
// Two details are load-bearing and neither is incidental.
//
// The index goes through a volatile so the compiler cannot fold it and reject
// the patched version at compile time. That is not hiding the defect from the
// build; it is keeping the canary a RUNTIME question, which is the question
// ASan answers and the only question that distinguishes this lane.
//
// The RETURN VALUE does not depend on the byte read. If it did, the patched
// version would fail its assertion under cpp-test as well, cpp-test would go
// red, and the control that makes cpp-asan's red attributable would be gone --
// one red counted twice instead of two lanes reporting two things.
int ReadHeapBufferInBounds() {
  const int kSize = 8;
  char* buf = new char[kSize];
  for (int i = 0; i < kSize; ++i) buf[i] = static_cast<char>('a' + i);
  volatile int index = kSize - 1;
  volatile char sink = buf[index];
  (void)sink;
  delete[] buf;
  return kSize;
}

// cpp-ubsan's canary target: a signed addition that does NOT overflow.
// LANE-cpp-ubsan makes it overflow, which an uninstrumented build wraps
// silently and which neither ASan nor TSan looks for. The return value is
// independent of the sum for the same reason as above.
int AddSignedIntegers() {
  volatile int a = 2147483646;
  volatile int b = 1;
  volatile int sum = a + b;
  (void)sum;
  return 0;
}

TEST(LaneCanary, OurOwnAssertionsRun) { EXPECT_EQ(Doubled(21), 42); }

TEST(LaneCanary, HeapBufferIsReadInBounds) {
  EXPECT_EQ(ReadHeapBufferInBounds(), 8);
}

TEST(LaneCanary, SignedAdditionIsDefined) { EXPECT_EQ(AddSignedIntegers(), 0); }

}  // namespace
