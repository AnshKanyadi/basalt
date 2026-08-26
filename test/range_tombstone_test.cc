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

}  // namespace
}  // namespace sst
}  // namespace rift
