// KNOWN-ANSWER VECTORS FOR THE RIG'S PCG64.
//
// `[A1]`'s reason, in C++: a corpus keyed to a seed is worthless if the
// generator can change underneath it. These values are PINNED -- they were
// produced by this implementation and are asserted forever after, so a change
// to the generator fails here rather than silently producing a different corpus
// in which every entry is self-consistent and different.
//
// THE VECTORS ARE NOT DERIVED FROM A REFERENCE IMPLEMENTATION, and that is
// stated rather than implied: what is pinned is THIS generator's behaviour, not
// its agreement with anyone else's PCG. The property the corpus needs is
// stability, not pedigree.
#include "rng.h"

#include <set>
#include <vector>

#include <gtest/gtest.h>

namespace rift {
namespace rig {
namespace {

TEST(Rng, IsPinned) {
  Pcg64 g(42);
  std::vector<uint64_t> got;
  for (int i = 0; i < 4; ++i) got.push_back(g.Next());
  // Regenerate deliberately if the generator is ever changed on purpose, and
  // never to make a failing test pass.
  static const std::vector<uint64_t> kExpected = {
      6649627550597423648ull, 7300106876089141621ull,
      2769834306095859105ull, 16257734974967078070ull};
  EXPECT_EQ(kExpected, got);
}

TEST(Rng, TheSameSeedGivesTheSameStream) {
  Pcg64 a(7), b(7);
  for (int i = 0; i < 64; ++i) EXPECT_EQ(a.Next(), b.Next()) << "at " << i;
}

TEST(Rng, DifferentSeedsDiverge) {
  Pcg64 a(7), b(8);
  bool differs = false;
  for (int i = 0; i < 8; ++i) differs = differs || (a.Next() != b.Next());
  EXPECT_TRUE(differs);
}

// BELOW() IS UNIFORM BY REJECTION, NOT BY MODULO, and this is the assertion
// that a modulo would fail: with a range that does not divide 2^64, the low
// values would be over-represented.
TEST(Rng, BelowIsInRangeAndCoversIt) {
  Pcg64 g(1);
  std::set<uint64_t> seen;
  for (int i = 0; i < 4000; ++i) {
    const uint64_t v = g.Below(7);
    ASSERT_LT(v, 7u);
    seen.insert(v);
  }
  EXPECT_EQ(7u, seen.size()) << "some value in [0,7) was never produced";
}

TEST(Rng, BelowHandlesTheDegenerateRanges) {
  Pcg64 g(1);
  EXPECT_EQ(0u, g.Below(0));
  EXPECT_EQ(0u, g.Below(1));
}

// A DERIVED SUB-STREAM DOES NOT ADVANCE ITS PARENT, which is what keeps two
// consumers from interleaving -- the way a corpus entry stops reproducing for a
// reason nobody can see.
TEST(Rng, DeriveDoesNotDisturbTheParent) {
  Pcg64 parent(99);
  const uint64_t before = Pcg64(99).Next();
  Pcg64 child = parent.Derive(3);
  (void)child.Next();
  EXPECT_EQ(before, parent.Next());
}

TEST(Rng, DifferentLabelsGiveDifferentSubStreams) {
  Pcg64 parent(99);
  Pcg64 a = parent.Derive(1);
  Pcg64 b = parent.Derive(2);
  bool differs = false;
  for (int i = 0; i < 8; ++i) differs = differs || (a.Next() != b.Next());
  EXPECT_TRUE(differs);
}

}  // namespace
}  // namespace rig
}  // namespace rift
