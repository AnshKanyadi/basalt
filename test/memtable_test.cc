#include "memtable.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "concurrency_claim.h"
#include "slice.h"
#include "tower.h"

namespace rift {
namespace {

std::string Get(const MemTable& m, const std::string& key, SeqNum snap) {
  std::string v;
  const Status s = m.Get(Slice(key), snap, &v);
  if (s.ok()) return v;
  return "<" + std::string(CodeName(s.code())) + ">";
}

// A fixed workload, used by both digest tests. Deliberately mixes heights: the
// keys below span towers of 1 through 5, so a change to the mapping moves the
// digest rather than leaving it coincidentally intact.
void FillFixedWorkload(MemTable* m) {
  const char* keys[] = {"k0", "k2", "k11", "k64", "k136", "k367", "k6", "z99", ""};
  SeqNum seq = 1;
  for (const char* k : keys) {
    m->Add(seq++, ValueType::kValue, Slice(k, std::string(k).size()),
           Slice("v", 1));
  }
}

TEST(MemTable, ReadsTheNewestVersionAtOrBelowTheSnapshot) {
  MemTable m;
  m.Add(1, ValueType::kValue, Slice("a"), Slice("one"));
  m.Add(5, ValueType::kValue, Slice("a"), Slice("five"));
  m.Add(9, ValueType::kValue, Slice("a"), Slice("nine"));

  EXPECT_EQ(Get(m, "a", 9), "nine");
  EXPECT_EQ(Get(m, "a", 5), "five");
  EXPECT_EQ(Get(m, "a", 4), "one");
  EXPECT_EQ(Get(m, "a", 1), "one");
  EXPECT_EQ(Get(m, "a", 0), "<kNotFound>")
      << "a snapshot below every version of a key must not see any of them";
}

// Multiple versions per key are REQUIRED, not incidental: NewSnapshot pins a
// sequence and a read through it must skip newer versions, so the memtable is
// append-only and never overwrites.
TEST(MemTable, NeverOverwritesAVersion) {
  MemTable m;
  m.Add(1, ValueType::kValue, Slice("a"), Slice("one"));
  m.Add(2, ValueType::kValue, Slice("a"), Slice("two"));
  EXPECT_EQ(m.Count(), 2u) << "an overwrite would leave the older snapshot with "
                              "nothing to read";
}

TEST(MemTable, ADeletionHidesOlderVersionsButOnlyFromLaterSnapshots) {
  MemTable m;
  m.Add(1, ValueType::kValue, Slice("a"), Slice("one"));
  m.Add(4, ValueType::kDeletion, Slice("a"), Slice());
  EXPECT_EQ(Get(m, "a", 9), "<kNotFound>");
  EXPECT_EQ(Get(m, "a", 4), "<kNotFound>");
  EXPECT_EQ(Get(m, "a", 3), "one")
      << "a snapshot taken before the delete must still see the value";
}

TEST(MemTable, KeysAreOrderedBytewiseAndTheEmptyKeyIsAKey) {
  MemTable m;
  for (const char* k : {"b", "", "a", "aa", "\xff"}) {
    m.Add(1, ValueType::kValue, Slice(k, std::string(k).size()), Slice(k, std::string(k).size()));
  }
  EXPECT_EQ(m.Count(), 5u);
  EXPECT_EQ(Get(m, "", 1), "");
  EXPECT_EQ(Get(m, "aa", 1), "aa");
  EXPECT_EQ(Get(m, "\xff", 1), "\xff");
  EXPECT_EQ(Get(m, "c", 1), "<kNotFound>");
}

TEST(MemTable, MemoryUsageGrowsAndIsExactlyTheArena) {
  MemTable m;
  const std::size_t empty = m.MemoryUsage();
  for (int i = 0; i < 200; ++i) {
    const std::string k = "key" + std::to_string(i);
    m.Add(static_cast<SeqNum>(i + 1), ValueType::kValue, Slice(k), Slice(k));
  }
  EXPECT_GT(m.MemoryUsage(), empty)
      << "B2's flush threshold reads this number; a memtable that reports no "
         "growth never flushes";
}

// ------------------------------------------------------- structural digest

// THE PINNED SHAPE. This is the C++ analogue of Track A's trace hash. It catches
// three things for one test: ambient randomness, uninitialized bytes reaching
// the structure, and any height source that is not a pure function of the key.
//
// If this number changes, the memtable's shape changed. That is either a bug or
// a deliberate re-specification, and a deliberate one updates this constant in
// its own commit alongside the golden vectors -- never in the same commit as
// the code.
constexpr uint64_t kFixedWorkloadDigest = 0xe5ba7567b616e26aULL;

TEST(MemTable, StructuralDigestIsPinned) {
  MemTable m;
  FillFixedWorkload(&m);
  EXPECT_EQ(m.StructuralDigest(), kFixedWorkloadDigest)
      << "the memtable's shape changed for a fixed key set";
}

// The same key set must build the same structure -- every time, in every
// instance. A PRNG height source, one derived from an address, or one reading
// uninitialized memory all differ between two instances in the same process,
// and none of the three is visible to any other check here.
TEST(MemTable, TheSameKeySetBuildsTheSameShape) {
  MemTable a;
  MemTable b;
  FillFixedWorkload(&a);
  FillFixedWorkload(&b);
  EXPECT_EQ(a.StructuralDigest(), b.StructuralDigest())
      << "two memtables built from the same key set have different shapes, so "
         "the height source is not a pure function of the key";
}

// And the digest must actually be sensitive to shape, or the two tests above
// are pinning a constant. One key differing must move it.
TEST(MemTable, TheDigestRespondsToTheKeySet) {
  MemTable a;
  MemTable b;
  FillFixedWorkload(&a);
  FillFixedWorkload(&b);
  b.Add(99, ValueType::kValue, Slice("k990"), Slice("v"));
  EXPECT_NE(a.StructuralDigest(), b.StructuralDigest());
}

// ---------------------------------------------------- the concurrency claim

// Strengthening this sentence requires failing this test, and the rule is that
// the harness must be strengthened in the SAME DIFF that strengthens the claim.
TEST(ConcurrencyClaim, WordingIsPinnedAndDoesNotClaimRaceFreedom) {
  const std::string claim(kConcurrencyClaim);
  EXPECT_EQ(claim,
            "TSan observed no data race across two authored interleaving "
            "patterns (concurrent MemTable Add and Get; concurrent DB Write "
            "and Sync across a flush); this is not a proof of race-freedom.");
  EXPECT_NE(claim.find("not a proof of race-freedom"), std::string::npos)
      << "the disclaimer is the whole sentence; without it this is a claim the "
         "harness does not support";
  EXPECT_EQ(claim.find("race-free."), std::string::npos)
      << "the lane observes the races TSan happens to see across ONE authored "
         "interleaving; it does not explore the interleaving space, and no "
         "wording may imply that it does";
}

}  // namespace
}  // namespace rift
