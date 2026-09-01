// The bloom filter: a pure function, checked as a pure function. No table, no
// Env, no rig -- B2-D9 puts it here precisely because nothing else has to exist
// for it to be checkable.
//
// THE TWO PROPERTIES ARE ASSERTED DIFFERENTLY, AND THAT IS THE POINT OF THE
// FILE. No false negatives is SAFETY and is asserted EXACTLY, over every key of
// every set. The false-positive rate is PERFORMANCE and is asserted as a
// CEILING with the measured number printed -- section 10.3's shape. Pinning the
// rate exactly would assert noise; asserting nothing about it would let the
// filter silently degrade to "maybe" for everything, which is why the ceiling
// has a FLOOR beside it.
#include "bloom.h"

#include <cstdio>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "crc32c.h"
#include "sha256.h"
#include "basalt/slice.h"

namespace basalt {
namespace sst {
namespace {

std::vector<std::string> Keys(const char* prefix, int n) {
  std::vector<std::string> out;
  out.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) out.push_back(std::string(prefix) + std::to_string(i));
  return out;
}

std::string BuildOver(const std::vector<std::string>& keys) {
  FilterBuilder b;
  for (const std::string& k : keys) b.AddUserKey(Slice(k));
  return b.Finish();
}

// A FilterReader holds a Slice into the block it parsed, so binding one to a
// temporary dangles at the end of the statement -- and reads exactly like the
// safe form beside it. The deleted rvalue overload is the same move slice.h
// made after ASan caught that class in the mutant lane's baseline gate; the
// lesson transfers because the shape does.
FilterReader ParseOrDie(const std::string& block) {
  FilterReader r;
  std::string why;
  EXPECT_TRUE(FilterReader::Parse(Slice(block), &r, &why)) << why;
  return r;
}
FilterReader ParseOrDie(std::string&&) = delete;

void RestampFilterCrc(std::string* block) {
  const uint32_t crc = wal::Crc32c(block->data(), block->size() - 4);
  for (int i = 0; i < 4; ++i) {
    (*block)[block->size() - 4 + i] = static_cast<char>((crc >> (8 * i)) & 0xff);
  }
}

// A filter block built BY HAND with an arbitrary probe count, using the same
// probe walk the builder uses. This is B2.0's rule carried into B2.1: the
// fixture and the reader share one encoder, or the fixture is testing itself.
std::string HandBuiltFilter(const std::vector<std::string>& keys, uint32_t probes,
                            std::size_t nbytes) {
  std::string out(nbytes, '\0');
  const uint64_t bits = static_cast<uint64_t>(nbytes) * 8;
  for (const std::string& k : keys) {
    ProbeWalk walk(BloomHash(Slice(k)), bits);
    for (uint32_t i = 0; i < probes; ++i) {
      const uint64_t pos = walk.Next();
      out[pos / 8] = static_cast<char>(static_cast<unsigned char>(out[pos / 8]) |
                                       (1u << (pos % 8)));
    }
  }
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((probes >> (8 * i)) & 0xff));
  const uint32_t n32 = static_cast<uint32_t>(nbytes);
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((n32 >> (8 * i)) & 0xff));
  const uint32_t crc = wal::Crc32c(out.data(), out.size());
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((crc >> (8 * i)) & 0xff));
  return out;
}

// ------------------------------------------------------------ the constants

TEST(Bloom, TheFrozenConstantsArePinned) {
  // These are not tuning knobs; they are the format. A build that changes
  // either one writes files whose rate is different from every file already on
  // disk, and the arithmetic that produced kProbes -- ln2 x 10 = 6.93 -> 7 --
  // was done by hand at the definition site because A5 forbids doing it here.
  EXPECT_EQ(10u, kBitsPerKey);
  EXPECT_EQ(7u, kProbes);
  EXPECT_EQ(8u, kMinFilterBytes);
  EXPECT_EQ(12u, kFilterTrailerBytes);
}

TEST(Bloom, TheProbeStepIsNeverZero) {
  // ASSERTED ON THE ARITHMETIC, because it cannot be asserted anywhere above
  // it. A zero step puts all k probes on one bit: no false negative, no failing
  // lookup, no symptom at all -- just a 7-probe filter quietly behaving like a
  // 1-probe one. `h2 | 1` is what prevents it, and h = 0 is the input that
  // proves the guard is doing something.
  for (uint64_t bits : {64u, 512u, 4096u, 12345u}) {
    EXPECT_NE(0u, ProbeWalk(0, bits).step()) << "bits=" << bits;
    for (uint64_t high = 0; high < 8; ++high) {
      const uint64_t h = (high << 32) | 0x5A5A5A5Aull;
      EXPECT_NE(0u, ProbeWalk(h, bits).step()) << "bits=" << bits << " high=" << high;
    }
  }
  // And a hash whose high half is an exact multiple of the bit count, which is
  // the other way a step reaches zero.
  EXPECT_NE(0u, ProbeWalk(static_cast<uint64_t>(512) << 32, 512).step());
}

TEST(Bloom, TheProbeWalkStaysInsideTheBitArray) {
  const uint64_t bits = 4096;
  ProbeWalk walk(0xDEADBEEFCAFEF00Dull, bits);
  for (int i = 0; i < 1000; ++i) EXPECT_LT(walk.Next(), bits);
}

// ---------------------------------------------------- safety: no false negatives

TEST(Bloom, NoFalseNegativesAtEverySize) {
  // EXACT, over the whole key set, at sizes that cross the minimum-size floor
  // and the byte-rounding boundary in both directions.
  for (int n : {0, 1, 2, 3, 7, 8, 16, 17, 100, 1000, 5000}) {
    const std::vector<std::string> keys = Keys("key", n);
    const std::string block = BuildOver(keys);
    const FilterReader r = ParseOrDie(block);
    for (const std::string& k : keys) {
      EXPECT_TRUE(r.MayContain(Slice(k))) << "FALSE NEGATIVE at n=" << n << " for " << k;
    }
  }
}

TEST(Bloom, NoFalseNegativesForAdversarialKeyShapes) {
  // The shapes a real key set has and a generated one does not: an empty key,
  // embedded NULs, keys differing in one byte, keys sharing a long prefix --
  // which is exactly what A5's MVCC encoding produces once it is layered on top.
  std::vector<std::string> keys;
  keys.push_back("");
  keys.push_back(std::string("a\0b", 3));
  keys.push_back(std::string("a\0c", 3));
  keys.push_back(std::string(1, '\xff'));
  keys.push_back(std::string(4096, 'z'));
  const std::string shared(200, 'p');
  for (int i = 0; i < 64; ++i) keys.push_back(shared + std::string(1, static_cast<char>(i)));

  const std::string block = BuildOver(keys);
  const FilterReader r = ParseOrDie(block);
  for (const std::string& k : keys) {
    EXPECT_TRUE(r.MayContain(Slice(k))) << "FALSE NEGATIVE for a key of size " << k.size();
  }
}

TEST(Bloom, AnEmptyFilterDeniesEveryKey) {
  // Zero keys means zero bits set, so every probe fails and every lookup is a
  // definite no. That is correct and it is worth asserting: a filter that
  // answered "maybe" for an empty table would make an empty SSTable cost a seek.
  const std::string block = BuildOver({});
  const FilterReader r = ParseOrDie(block);
  for (const std::string& k : Keys("anything", 50)) {
    EXPECT_FALSE(r.MayContain(Slice(k)));
  }
}

// ------------------------------------------- performance: the rate, measured

TEST(Bloom, FalsePositiveRateIsUnderItsCeilingAndOverItsFloor) {
  const std::vector<std::string> members = Keys("key:", 10000);
  const std::vector<std::string> strangers = Keys("miss:", 100000);
  const std::string block = BuildOver(members);
  const FilterReader r = ParseOrDie(block);

  std::size_t hits = 0;
  for (const std::string& k : strangers) {
    if (r.MayContain(Slice(k))) ++hits;
  }
  const std::size_t per_mille = hits * 1000 / strangers.size();

  // MEASURED, NOT GUESSED, and printed on every run so the number is visible
  // rather than merely bounded. The prediction at 10 bits/key and k=7 is about
  // 8 per mille; the measurement here is what the ceiling is set against.
  std::printf("  bloom: false positives %zu/%zu = %zu per mille (predicted ~8)\n",
              hits, strangers.size(), per_mille);

  // A CEILING, loose enough that a key-shape change does not redden the lane
  // and tight enough that a filter degrading toward "maybe for everything"
  // cannot hide. A lane that reddens on benign change gets its bound raised
  // rather than its code fixed, which is how a bound stops meaning anything.
  EXPECT_LE(per_mille, 30u);

  // AND A FLOOR, because a rate of zero over a hundred thousand strangers does
  // not mean the filter is perfect -- it means the measurement did not happen.
  // GF-1: a lane verifying a rate must run where the rate could be non-zero.
  EXPECT_GT(hits, 0u);
}

// ------------------------------------------------- the block, and its bytes

TEST(Bloom, TheFilterIsIndependentOfInsertionOrder) {
  // Bits are OR'ed, so the same key set in any order must produce byte-identical
  // output. If it did not, the byte digest of a flush would depend on the order
  // the skiplist happened to be walked in, and a pinned digest would be pinning
  // a traversal rather than a format.
  const std::vector<std::string> keys = Keys("k", 500);
  std::vector<std::string> reversed(keys.rbegin(), keys.rend());
  std::vector<std::string> interleaved;
  for (std::size_t i = 0; i < keys.size(); i += 2) interleaved.push_back(keys[i]);
  for (std::size_t i = 1; i < keys.size(); i += 2) interleaved.push_back(keys[i]);

  const std::string a = BuildOver(keys);
  EXPECT_EQ(a, BuildOver(reversed));
  EXPECT_EQ(a, BuildOver(interleaved));
}

TEST(Bloom, TheBytesArePinned) {
  // The format is frozen, so its bytes are a golden vector. This catches three
  // things at once, the way the WAL digest does: ambient randomness, an
  // uninitialized byte reaching the block, and any float that reached the
  // sizing arithmetic.
  const std::string block = BuildOver(Keys("pinned-", 128));
  // 128 keys x 10 bits = 1280 bits = 160 bytes, plus the twelve-byte trailer.
  EXPECT_EQ(128u * kBitsPerKey / 8 + kFilterTrailerBytes, block.size());
  EXPECT_EQ("673aab9c1f1716fa79fd6cff2b016a4f9e66826545c14c0141539a9030d82f41",
            wal::Sha256Hex(block.data(), block.size()));
}

TEST(Bloom, TheProbeCountIsReadFromTheFileAndNotFromTheConstant) {
  // Self-description, asserted rather than assumed: a build that pinned a
  // different k must still read this build's files correctly. Rewriting the
  // stored count to 3 must change how many bits are TESTED, which is only
  // observable if the reader is using the file's value.
  std::string block = BuildOver(Keys("k", 200));
  const std::size_t probes_at = block.size() - kFilterTrailerBytes;
  block[probes_at] = 3;
  for (int i = 1; i < 4; ++i) block[probes_at + i] = 0;
  RestampFilterCrc(&block);

  const FilterReader r = ParseOrDie(block);
  EXPECT_EQ(3u, r.probes());
  // Fewer probes can only ever accept MORE keys, so every member is still
  // present -- no false negative is introduced by reading the file's number.
  for (const std::string& k : Keys("k", 200)) EXPECT_TRUE(r.MayContain(Slice(k)));
}

TEST(Bloom, AFilterWrittenWithADifferentProbeCountIsStillReadCorrectly) {
  // THE DIRECTION THAT MATTERS. A reader using its OWN constant instead of the
  // file's would test four bits a k=3 writer never set, and every member of the
  // table would probe absent -- a false negative produced by version skew
  // rather than by corruption, and therefore invisible to every checksum in the
  // file.
  const std::vector<std::string> keys = Keys("older-writer-", 300);
  const std::string block = HandBuiltFilter(keys, 3, 512);
  const FilterReader r = ParseOrDie(block);
  EXPECT_EQ(3u, r.probes());
  for (const std::string& k : keys) {
    EXPECT_TRUE(r.MayContain(Slice(k))) << "FALSE NEGATIVE from a k=3 file: " << k;
  }
}

// ---------------------------------------------------------- what it refuses

TEST(Bloom, RejectsABlockTooSmallToHoldItsTrailer) {
  FilterReader r;
  std::string why;
  const std::string tiny(kFilterTrailerBytes + kMinFilterBytes - 1, '\0');
  EXPECT_FALSE(FilterReader::Parse(Slice(tiny), &r, &why));
  const std::string nothing;
  EXPECT_FALSE(FilterReader::Parse(Slice(nothing), &r, &why));
}

TEST(Bloom, RejectsABadChecksum) {
  std::string block = BuildOver(Keys("k", 40));
  block[3] ^= 0x01;
  FilterReader r;
  std::string why;
  EXPECT_FALSE(FilterReader::Parse(Slice(block), &r, &why));
  EXPECT_NE(std::string::npos, why.find("checksum")) << why;
}

TEST(Bloom, RejectsALengthThatDisagreesWithItsBlock) {
  // A TRUNCATED FILTER MUST ANNOUNCE ITSELF AS A FILTER PROBLEM. Without the
  // stored length, a filter cut short by a torn write would be read as a
  // smaller filter -- one that answers "no" to keys the table holds, which is
  // the false negative this whole file exists to make impossible.
  std::string block = BuildOver(Keys("k", 40));
  block.erase(block.size() - kFilterTrailerBytes - 1, 1);
  RestampFilterCrc(&block);
  FilterReader r;
  std::string why;
  EXPECT_FALSE(FilterReader::Parse(Slice(block), &r, &why));
  EXPECT_NE(std::string::npos, why.find("disagrees")) << why;
}

TEST(Bloom, RejectsZeroProbes) {
  std::string block = BuildOver(Keys("k", 40));
  const std::size_t probes_at = block.size() - kFilterTrailerBytes;
  for (int i = 0; i < 4; ++i) block[probes_at + i] = 0;
  RestampFilterCrc(&block);
  FilterReader r;
  std::string why;
  EXPECT_FALSE(FilterReader::Parse(Slice(block), &r, &why));
  EXPECT_NE(std::string::npos, why.find("zero probes")) << why;
}

TEST(Bloom, RejectsAProbeCountNoWriterCouldHaveProduced) {
  // A count off disk drives a loop. Four billion probes is not a wrong answer,
  // it is an unanswerable file, and the bound is where it stops being a lookup.
  std::string block = BuildOver(Keys("k", 40));
  const std::size_t probes_at = block.size() - kFilterTrailerBytes;
  const uint32_t absurd = 0xFFFFFFFFu;
  for (int i = 0; i < 4; ++i) {
    block[probes_at + i] = static_cast<char>((absurd >> (8 * i)) & 0xff);
  }
  RestampFilterCrc(&block);
  FilterReader r;
  std::string why;
  EXPECT_FALSE(FilterReader::Parse(Slice(block), &r, &why));
  EXPECT_NE(std::string::npos, why.find("probes")) << why;
}

}  // namespace
}  // namespace sst
}  // namespace basalt
