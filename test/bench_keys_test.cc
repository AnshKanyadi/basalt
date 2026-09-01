// The benchmark's key stream, pinned on the C++ side.
//
// An embedder that wants a comparable column pins the same three values on its
// own side. Neither assertion alone is worth much: what makes a table's columns
// comparable is that BOTH sides refuse to build a workload the other would not
// have built, and a pinned vector asserted on only one side is a promise the
// other never made.
#include "bench_keys.h"

#include <cstdint>

#include <gtest/gtest.h>

namespace basalt {
namespace {

TEST(BenchKeys, TheStreamIsPinnedInBothLanguages) {
  EXPECT_EQ(0x910A2DEC89025CC1ull, Bench64(1, 0));
  EXPECT_EQ(0xBEEB8DA1658EEC67ull, Bench64(1, 1));
  EXPECT_EQ(0xF893A2EEFB32555Eull, Bench64(1, 2));
}

TEST(BenchKeys, KeysAreFixedWidthSoKeyLengthIsAConstantOfTheWorkload) {
  for (uint64_t i = 0; i < 64; i++) {
    EXPECT_EQ(16u, BenchKey(1, i, 16).size()) << "at i=" << i;
  }
}

}  // namespace
}  // namespace basalt
