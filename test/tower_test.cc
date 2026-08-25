// The golden height vectors.
//
// The memtable's shape is a pure function of the key set, which makes the
// mapping from hash bits to tower height ON-DISK-ADJACENT BEHAVIOUR: change it
// and every skiplist this engine has ever built has a different structure. So
// any change to it must FAIL A VECTOR to happen, exactly the way NextTick is
// pinned on the Go side.
//
// Per A0's rule about signed packages, THESE VECTORS NEVER CHANGE IN THE SAME
// COMMIT AS THE CODE THEY PIN. A diff that touches both is a diff that has
// silently re-specified the structure.
//
// The vectors were computed offline and cover, per the ruling's condition:
// every reachable height 1..12, both sides of a tower boundary, both sides of
// the CAP boundary, the empty key, and a key whose hash has all low bits set.
#include "tower.h"

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "slice.h"

namespace rift {
namespace {

struct Vector {
  const char* key;
  uint64_t hash;
  int ntz;
  int height;
};

// EVERY REACHABLE HEIGHT. Finding a key for height 12 took a search of nine
// million; that it is expensive to reach is the point, and pinning it here is
// what stops the cap from silently becoming unreachable.
constexpr Vector kHeights[] = {
    {"k0",        0x08be0e07b562230eULL,  1,  1},
    {"k2",        0x08be0c07b5621fa8ULL,  3,  2},
    {"k11",       0x3d187a1935c4c3d0ULL,  4,  3},
    {"k64",       0x3d1b7d1935c6fec0ULL,  6,  4},
    {"k136",      0x9561ded65d5e2500ULL,  8,  5},
    {"k367",      0x83a58dd65334a400ULL, 10,  6},
    {"k990",      0x4f1a90d635489000ULL, 12,  7},
    {"k119201",   0xa1929b9870b44000ULL, 14,  8},
    {"k27005",    0x49a3097973c20000ULL, 17,  9},
    {"k930208",   0xf70901603d080000ULL, 19, 10},
    {"k1008469",  0xb97655da5e700000ULL, 20, 11},
    {"k8987398",  0x58b14e69e0400000ULL, 22, 12},
};

// Both sides of a tower boundary, and both sides of the CAP. ntz 21 -> 11 and
// ntz 22 -> 12 is the last real step; ntz 24 must still be 12, because the
// mapping is capped and a cap that stopped capping would be invisible in a
// distribution and obvious here.
constexpr Vector kBoundaries[] = {
    {"k0",        0x08be0e07b562230eULL,  1,  1},
    {"k6",        0x08be1007b5622674ULL,  2,  2},
    {"k2",        0x08be0c07b5621fa8ULL,  3,  2},
    {"k11",       0x3d187a1935c4c3d0ULL,  4,  3},
    {"k7423961",  0x7948131cd8a00000ULL, 21, 11},
    {"k8987398",  0x58b14e69e0400000ULL, 22, 12},
    {"k31164007", 0x23dc535671000000ULL, 24, 12},
};

// The empty key hashes to the FNV offset basis with no mixing at all, and a key
// whose low byte is all ones has ntz 0. Both are the edges an implementation is
// most likely to get wrong by special-casing.
constexpr Vector kEdges[] = {
    {"",    0xcbf29ce484222325ULL, 0, 1},
    {"z99", 0xcf5dc319885655ffULL, 0, 1},
};

void CheckVector(const Vector& v) {
  const Slice key(v.key, std::string(v.key).size());
  EXPECT_EQ(Fnv1a64(key), v.hash)
      << "fnv1a64(\"" << v.key << "\") changed; the hash is the input to every "
         "height in the engine";
  EXPECT_EQ(TowerHeight(key), v.height)
      << "the tower mapping changed for \"" << v.key << "\" (ntz " << v.ntz
      << "). Every skiplist this engine has ever built now has a different "
         "shape. If that is intended, the vectors change in their OWN commit.";
}

TEST(TestHeightVectors, EveryReachableHeight) {
  for (const Vector& v : kHeights) CheckVector(v);
}

TEST(TestHeightVectors, TowerAndCapBoundaries) {
  for (const Vector& v : kBoundaries) CheckVector(v);
}

TEST(TestHeightVectors, EdgeKeys) {
  for (const Vector& v : kEdges) CheckVector(v);
}

TEST(TestHeightVectors, AllTwelveHeightsAreCovered) {
  bool seen[kMaxHeight + 1] = {false};
  for (const Vector& v : kHeights) seen[v.height] = true;
  for (int h = 1; h <= kMaxHeight; ++h) {
    EXPECT_TRUE(seen[h]) << "height " << h << " has no golden vector, so a "
                            "mapping change that only affects it would not fail "
                            "a vector";
  }
}

TEST(TestHeightVectors, HeightIsAlwaysInRange) {
  for (int i = 0; i < 5000; ++i) {
    const std::string k = "key" + std::to_string(i);
    const int h = TowerHeight(Slice(k));
    EXPECT_GE(h, 1);
    EXPECT_LE(h, kMaxHeight);
  }
}

}  // namespace
}  // namespace rift
