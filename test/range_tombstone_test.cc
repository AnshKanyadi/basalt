// The range-tombstone classifier, from fixture bytes, WITH NO WRITER IN THE
// TREE AND NO COMPACTION TO PRODUCE ONE.
//
// Fourth use of B2-D6's ordering and the strongest version of it: the format
// this file judges DOES NOT EXIST YET anywhere else. There is no encoder to
// agree with, so every refusal below is a decision about what the format MEANS
// -- a specification the writer will be checked against, fixed in DESIGN-B3
// section 6.1 before either file was written.
#include "range_tombstone.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "internal_key.h"
#include "table_format.h"

namespace rift {
namespace sst {
namespace {

uint64_t DelTag(SeqNum seq) { return MakeTag(seq, ValueType::kDeletion); }

struct Fixture {
  std::string start, end;
  uint64_t tag;
  std::string value;  // must be empty for a legal tombstone
};

// Builds a range block by hand, through the SAME encoder the reader validates.
std::string Block(const std::vector<Fixture>& in) {
  BlockBuilder b;
  std::vector<std::string> keys;
  keys.reserve(in.size());
  for (const Fixture& f : in) {
    std::string k;
    EncodeRangeTombstone(Slice(f.start), Slice(f.end), f.tag, &k);
    keys.push_back(k);
  }
  for (std::size_t i = 0; i < in.size(); ++i) {
    b.Add(Slice(keys[i]), Slice(in[i].value));
  }
  return b.Finish();
}

RangeCheck Check(const std::string& block, std::vector<RangeTombstone>* out) {
  return ParseRangeBlock(Slice(block), out);
}

// A block holding one UNBOUNDED-end tombstone, through the same encoder.
std::string UnboundedBlock(const std::string& start, uint64_t tag,
                           const std::string& extra_end_bytes = "") {
  BlockBuilder b;
  std::string k;
  EncodeUnboundedRangeTombstone(Slice(start), tag, &k);
  if (!extra_end_bytes.empty()) {
    // Splice the bytes in where a finite end would live: after the sentinel
    // length and before the tag. That is exactly the shape the refusal is for --
    // a record claiming the sentinel AND carrying an end.
    const std::size_t tag_at = k.size() - 8;
    k.insert(tag_at, extra_end_bytes);
  }
  b.Add(Slice(k), Slice());
  return b.Finish();
}

// ------------------------------------------------------------ the legal shape

TEST(RangeTombstone, AcceptsASortedBlockAndReportsWhatItHolds) {
  std::vector<RangeTombstone> t;
  const std::string block = Block({{"a", "c", DelTag(5), ""},
                                   {"m", "q", DelTag(9), ""}});
  const RangeCheck v = Check(block, &t);
  ASSERT_TRUE(v.ok()) << RangeFaultName(v.fault) << ": " << v.why;
  ASSERT_EQ(2u, t.size());
  EXPECT_EQ(2u, v.count);
  EXPECT_EQ("a", t[0].start.ToString());
  EXPECT_EQ("c", t[0].end.ToString());
  EXPECT_EQ(5u, t[0].seq());
}

TEST(RangeTombstone, TheBoundsAreHalfOpen) {
  // [start, end), agreeing with engine.InRange by construction. The end key is
  // the one a reader gets wrong, so it is asserted rather than assumed.
  std::vector<RangeTombstone> t;
  const std::string block = Block({{"b", "d", DelTag(5), ""}});
  ASSERT_TRUE(Check(block, &t).ok());
  ASSERT_EQ(1u, t.size());
  EXPECT_FALSE(t[0].Covers(Slice("a")));
  EXPECT_TRUE(t[0].Covers(Slice("b")));   // start INCLUSIVE
  EXPECT_TRUE(t[0].Covers(Slice("c")));
  EXPECT_FALSE(t[0].Covers(Slice("d")));  // end EXCLUSIVE
  EXPECT_FALSE(t[0].Covers(Slice("e")));
}

TEST(RangeTombstone, TheEmptyUserKeyIsAValidBound) {
  // THE OPPOSITE OF THE POINT-ENTRY RULE, and stated because a reader who has
  // internalised that one will assume it carries over. Point entries are
  // refused when too short to carry a tag; range bounds are USER keys and the
  // tag is a separate field, so the empty key is legal and means "from the
  // beginning".
  std::vector<RangeTombstone> t;
  const std::string block = Block({{"", "m", DelTag(5), ""}});
  const RangeCheck v = Check(block, &t);
  ASSERT_TRUE(v.ok()) << RangeFaultName(v.fault) << ": " << v.why;
  EXPECT_TRUE(t[0].Covers(Slice("")));
  EXPECT_TRUE(t[0].Covers(Slice("a")));
  EXPECT_FALSE(t[0].Covers(Slice("m")));
}

TEST(RangeTombstone, TwoTombstonesMayShareAStartWhenTheirTagsDiffer) {
  // Two ranges from the same start at different sequences is what a key deleted
  // twice produces, and it is legal.
  std::vector<RangeTombstone> t;
  const std::string block = Block({{"a", "c", DelTag(9), ""},
                                   {"a", "z", DelTag(4), ""}});
  const RangeCheck v = Check(block, &t);
  ASSERT_TRUE(v.ok()) << RangeFaultName(v.fault) << ": " << v.why;
  EXPECT_EQ(2u, t.size());
}

// ------------------------------------------------------------ what it refuses

TEST(RangeTombstone, RefusesAnEmptyRange) {
  std::vector<RangeTombstone> t;
  const RangeCheck v = Check(Block({{"m", "m", DelTag(5), ""}}), &t);
  EXPECT_EQ(RangeFault::kEmptyOrInvertedRange, v.fault) << v.why;
}

TEST(RangeTombstone, RefusesAnInvertedRange) {
  // Accepting it would make "covers nothing" and "covers everything below the
  // start" indistinguishable at the reader.
  std::vector<RangeTombstone> t;
  const RangeCheck v = Check(Block({{"q", "b", DelTag(5), ""}}), &t);
  EXPECT_EQ(RangeFault::kEmptyOrInvertedRange, v.fault) << v.why;
}

TEST(RangeTombstone, RefusesTombstonesThatDoNotAscendByStart) {
  // THE BLOCK IS BINARY-SEARCHED. An unsorted block does not fail -- it returns
  // the wrong answer, which is a resurrected key or a wrongly hidden one.
  std::vector<RangeTombstone> t;
  const RangeCheck v = Check(Block({{"m", "q", DelTag(5), ""},
                                    {"a", "c", DelTag(9), ""}}), &t);
  EXPECT_EQ(RangeFault::kNotAscending, v.fault) << v.why;
}

TEST(RangeTombstone, RefusesADuplicateStartAndTag) {
  std::vector<RangeTombstone> t;
  const RangeCheck v = Check(Block({{"a", "c", DelTag(5), ""},
                                    {"a", "z", DelTag(5), ""}}), &t);
  EXPECT_EQ(RangeFault::kDuplicate, v.fault) << v.why;
}

TEST(RangeTombstone, RefusesATagThatIsNotADeletion) {
  // A point version in the range block is data that exists and is unreachable:
  // the point path will never look here for it.
  std::vector<RangeTombstone> t;
  const RangeCheck v = Check(Block({{"a", "c", MakeTag(5, ValueType::kValue), ""}}), &t);
  EXPECT_EQ(RangeFault::kNotADeleteRangeTag, v.fault) << v.why;
}

TEST(RangeTombstone, RefusesATombstoneCarryingAValue) {
  std::vector<RangeTombstone> t;
  const RangeCheck v = Check(Block({{"a", "c", DelTag(5), "unexpected"}}), &t);
  EXPECT_EQ(RangeFault::kMalformedBlock, v.fault) << v.why;
  EXPECT_NE(std::string::npos, v.why.find("no value")) << v.why;
}

TEST(RangeTombstone, RefusesAnEntryThatIsNotExactlyATombstone) {
  // Trailing bytes after a complete tombstone mean the writer meant something
  // this format does not express, and guessing which is how a format acquires a
  // second interpretation.
  BlockBuilder b;
  std::string k;
  EncodeRangeTombstone(Slice("a"), Slice("c"), DelTag(5), &k);
  k.push_back('x');
  b.Add(Slice(k), Slice(""));
  const std::string block = b.Finish();
  std::vector<RangeTombstone> t;
  const RangeCheck v = Check(block, &t);
  EXPECT_EQ(RangeFault::kMalformedBlock, v.fault) << v.why;
}

TEST(RangeTombstone, RefusesABlockWithABadChecksum) {
  // Inherited from ParseBlock rather than re-implemented -- ONE block decoder,
  // and this asserts the inheritance is real rather than assumed.
  std::string block = Block({{"a", "c", DelTag(5), ""}});
  block[2] ^= 0x01;
  std::vector<RangeTombstone> t;
  const RangeCheck v = Check(block, &t);
  EXPECT_EQ(RangeFault::kMalformedBlock, v.fault);
  EXPECT_NE(std::string::npos, v.why.find("checksum")) << v.why;
}

TEST(RangeTombstone, EveryFaultHasADistinctName) {
  const RangeFault all[] = {RangeFault::kNone, RangeFault::kMalformedBlock,
                            RangeFault::kEmptyOrInvertedRange, RangeFault::kNotAscending,
                            RangeFault::kDuplicate, RangeFault::kNotADeleteRangeTag};
  std::vector<std::string> names;
  for (RangeFault f : all) {
    const std::string n = RangeFaultName(f);
    EXPECT_FALSE(n.empty());
    for (const std::string& seen : names) EXPECT_NE(seen, n);
    names.push_back(n);
  }
}

// ------------------------------------- B3-Q4: the unbounded end, in the bytes
//
// A NEW SHAPE THE FROZEN REFUSALS HAVE NEVER SEEN. Every rule §6.1 fixed was
// induced against FINITE ends, so the sentinel gets its own fixtures rather
// than inheriting confidence from theirs.

TEST(RangeTombstone, AcceptsAnUnboundedEnd) {
  std::vector<RangeTombstone> t;
  // BOUND TO A LOCAL, NOT PASSED AS A TEMPORARY. `RangeTombstone::start` is a
  // Slice INTO the block, so a temporary block dies at the end of the
  // expression and every field reads freed memory -- HARNESS-007's class,
  // reached the one way the deleted `Slice(std::string&&)` cannot catch:
  // through a `const std::string&` parameter. The first version of this test
  // read "e" for a start of "m" and still passed its ok() assertion.
  const std::string block = UnboundedBlock("m", DelTag(9));
  const RangeCheck v = Check(block, &t);
  ASSERT_TRUE(v.ok()) << RangeFaultName(v.fault) << ": " << v.why;
  ASSERT_EQ(1u, t.size());
  EXPECT_TRUE(t[0].end_unbounded);
  EXPECT_EQ("m", t[0].start.ToString());
  EXPECT_EQ(9u, t[0].seq());
}

// IT COVERS EVERYTHING AT OR ABOVE ITS START AND NOTHING BELOW. Both halves,
// because "unbounded" implemented as "covers everything" passes the first.
TEST(RangeTombstone, AnUnboundedEndCoversEverythingAboveItsStartAndNothingBelow) {
  std::vector<RangeTombstone> t;
  const std::string block = UnboundedBlock("m", DelTag(9));
  ASSERT_TRUE(Check(block, &t).ok());
  ASSERT_EQ(1u, t.size());
  EXPECT_FALSE(t[0].Covers(Slice("a")));
  EXPECT_FALSE(t[0].Covers(Slice("l")));
  EXPECT_TRUE(t[0].Covers(Slice("m")));      // start is INCLUSIVE
  EXPECT_TRUE(t[0].Covers(Slice("zzzzzz")));
}

// THE TWO WAYS OF SAYING UNBOUNDED MUST NOT BOTH EXIST. A record claiming the
// sentinel while carrying end bytes is refused, so "no upper bound" has exactly
// one encoding and there is no second one to disagree with it.
TEST(RangeTombstone, RefusesAnUnboundedEndThatCarriesEndBytes) {
  std::vector<RangeTombstone> t;
  const std::string block = UnboundedBlock("m", DelTag(9), "zzz");
  const RangeCheck v = Check(block, &t);
  ASSERT_FALSE(v.ok());
  EXPECT_EQ(RangeFault::kUnboundedEndWithBytes, v.fault) << v.why;
}

// AND `end > start` IS STILL A RULE ABOUT FINITE ENDS. The sentinel is not an
// exception to it; an inverted FINITE range beside an unbounded one is still
// refused, which is what distinguishes "a different case" from "a hole".
TEST(RangeTombstone, TheFiniteRuleStillBindsBesideAnUnboundedOne) {
  BlockBuilder b;
  std::string ok_key;
  EncodeUnboundedRangeTombstone(Slice("a"), DelTag(9), &ok_key);
  b.Add(Slice(ok_key), Slice());
  std::string bad_key;
  EncodeRangeTombstone(Slice("m"), Slice("b"), DelTag(7), &bad_key);  // inverted
  b.Add(Slice(bad_key), Slice());
  const std::string block = b.Finish();
  std::vector<RangeTombstone> t;
  const RangeCheck v = ParseRangeBlock(Slice(block), &t);
  ASSERT_FALSE(v.ok());
  EXPECT_EQ(RangeFault::kEmptyOrInvertedRange, v.fault) << v.why;
}

}  // namespace
}  // namespace sst
}  // namespace rift
