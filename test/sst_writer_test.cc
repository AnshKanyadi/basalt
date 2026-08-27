// The SSTable writer, checked against rules that were already seen to reject
// every illegal shape. Not against a decoder written to agree with it.
//
// Every table this file produces goes through ValidateTable -- the same
// function, unchanged, that sst_format_test.cc induced on hand-built bytes.
// That is the whole value of B2-D6's ordering: the writer's output is judged by
// a classifier that has never seen it.
#include "table_builder.h"

#include <cstdio>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include "bloom.h"
#include "env.h"
#include "internal_key.h"
#include "memtable.h"
#include "sha256.h"
#include "crc32c.h"
#include "range_tombstone.h"
#include "table_check.h"
#include "test_env.h"

namespace rift {
namespace sst {
namespace {

using testenv::FaultPlan;
using testenv::Injection;
using testenv::TestEnvironment;

const std::string kDir = "db";
const std::string kTable = "db/000007.sst";

std::string IKey(const std::string& user, SeqNum seq,
                 ValueType type = ValueType::kValue) {
  std::string out;
  AppendInternalKey(&out, Slice(user), MakeTag(seq, type));
  return out;
}

// Writes one table through the Env and returns its bytes. The caller supplies
// the entries in table order; the writer RIFT_CHECKs that they are.
struct Written {
  std::string bytes;
  uint64_t entries = 0;
  std::string smallest;
  std::string largest;
  SeqNum largest_seq = 0;
  Status status;
};

Written WriteTable(TestEnvironment* t,
                   const std::vector<std::pair<std::string, std::string>>& kvs) {
  Written w;
  WritableFilePtr f;
  EXPECT_TRUE(t->env()->CreateDir(kDir).ok());
  EXPECT_TRUE(t->env()->NewWritableFile(kTable, &f).ok());
  {
    TableBuilder b(f.get());
    for (const auto& kv : kvs) b.Add(Slice(kv.first), Slice(kv.second));
    w.status = b.Finish();
    w.entries = b.entries();
    w.smallest = b.smallest().ToString();
    w.largest = b.largest().ToString();
    w.largest_seq = b.largest_seq();
  }
  if (w.status.ok()) {
    EXPECT_TRUE(f->Sync().ok());
    EXPECT_TRUE(f->Close().ok());
  }
  w.bytes = t->ContentNow(kTable);
  return w;
}

std::vector<std::pair<std::string, std::string>> Sequential(int n, int value_bytes) {
  std::vector<std::pair<std::string, std::string>> out;
  for (int i = 0; i < n; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "key%06d", i);
    out.emplace_back(IKey(buf, 1), std::string(static_cast<std::size_t>(value_bytes), 'v'));
  }
  return out;
}

// ----------------------------------------------------------- the writer

TEST(SstWriter, WhatItWritesTheClassifierAccepts) {
  TestEnvironment t;
  const Written w = WriteTable(&t, {{IKey("a", 1), "1"},
                                    {IKey("b", 1), "2"},
                                    {IKey("c", 1), "3"}});
  ASSERT_TRUE(w.status.ok()) << w.status.ToString();
  const TableCheck v = ValidateTable(Slice(w.bytes));
  ASSERT_TRUE(v.ok()) << TableFaultName(v.fault) << ": " << v.why;
  EXPECT_EQ(1u, v.data_blocks);
  EXPECT_EQ(3u, v.entries);
  EXPECT_EQ(3u, w.entries);
}

TEST(SstWriter, ManyBlocksAreStillOneLegalTable) {
  // Enough data to cut several blocks, so the index has more than one entry and
  // the separator rule is exercised on a table the writer actually cut.
  TestEnvironment t;
  const Written w = WriteTable(&t, Sequential(400, 64));
  ASSERT_TRUE(w.status.ok()) << w.status.ToString();
  const TableCheck v = ValidateTable(Slice(w.bytes));
  ASSERT_TRUE(v.ok()) << TableFaultName(v.fault) << ": " << v.why;
  EXPECT_GT(v.data_blocks, 5u) << "the block size never cut a block";
  EXPECT_EQ(400u, v.entries);
}

TEST(SstWriter, VersionsOfOneKeyAreWrittenNewestFirst) {
  // The order the memtable emits, written out and validated. A writer that
  // reordered these would be caught by the classifier, and a classifier
  // comparing bytes would refuse this legal table -- the pair is what makes
  // either statement mean anything.
  TestEnvironment t;
  const Written w = WriteTable(&t, {{IKey("a", 9), "new"},
                                    {IKey("a", 4), "old"},
                                    {IKey("b", 2, ValueType::kDeletion), ""}});
  ASSERT_TRUE(w.status.ok()) << w.status.ToString();
  const TableCheck v = ValidateTable(Slice(w.bytes));
  ASSERT_TRUE(v.ok()) << TableFaultName(v.fault) << ": " << v.why;
  EXPECT_EQ(3u, v.entries);
  EXPECT_EQ(IKey("a", 9), w.smallest);
  EXPECT_EQ(IKey("b", 2, ValueType::kDeletion), w.largest);
  // The largest SEQUENCE is not the sequence of the largest KEY. It is what
  // recovery re-derives and holds the manifest to (B2-D4 section 5.1 point 2),
  // so a writer that reported the last key's sequence would put a number in the
  // manifest that recovery then disagrees with.
  EXPECT_EQ(9u, w.largest_seq);
}

TEST(SstWriter, TheFilterHoldsEveryUserKeyAndNotTheInternalOnes) {
  // A filter built over INTERNAL keys is the defect that makes every lookup
  // miss: a reader probes a user key, the filter holds user_key||tag, and the
  // answer is "definitely not" for keys the table certainly contains.
  TestEnvironment t;
  std::vector<std::pair<std::string, std::string>> kvs;
  std::vector<std::string> users;
  for (int i = 0; i < 200; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "user%04d", i);
    users.emplace_back(buf);
    kvs.emplace_back(IKey(buf, 3), "v");
  }
  const Written w = WriteTable(&t, kvs);
  ASSERT_TRUE(w.status.ok()) << w.status.ToString();

  Footer footer;
  std::string why;
  ASSERT_TRUE(DecodeFooter(Slice(w.bytes), &footer, &why)) << why;
  ASSERT_GT(footer.filter.size, 0u);
  const std::string filter_block =
      w.bytes.substr(footer.filter.offset, footer.filter.size);
  FilterReader r;
  ASSERT_TRUE(FilterReader::Parse(Slice(filter_block), &r, &why)) << why;
  for (const std::string& u : users) {
    EXPECT_TRUE(r.MayContain(Slice(u))) << "the filter denies a key the table holds: " << u;
  }
}

TEST(SstWriter, TheBytesArePinned) {
  // Same workload, same bytes, forever. Catches ambient randomness, an
  // uninitialized byte reaching the file, and any float that reached the sizing
  // arithmetic -- the WAL digest's three-for-one, on the second format.
  TestEnvironment t;
  const Written w = WriteTable(&t, Sequential(64, 16));
  ASSERT_TRUE(w.status.ok()) << w.status.ToString();
  EXPECT_EQ("5ac666d3fcbf1b1c92e2423cf56dd6081b284b682c07d5df96a2bf9099be5fae",
            wal::Sha256Hex(w.bytes.data(), w.bytes.size()));
}

TEST(SstWriter, AMemtableWrittenOutIsALegalTable) {
  // B2.4's PRECONDITION, END TO END, before the flush path exists: whatever the
  // memtable emits, in the order it emits it, is a table the classifier accepts.
  MemTable m;
  for (SeqNum seq = 1; seq <= 4; ++seq) {
    for (int i = 0; i < 50; ++i) {
      char buf[32];
      std::snprintf(buf, sizeof buf, "k%04d", i);
      const std::string user(buf);
      const std::string value = user + ":" + std::to_string(seq);
      m.Add(seq, seq == 3 ? ValueType::kDeletion : ValueType::kValue,
            Slice(user), Slice(value));
    }
  }
  std::vector<std::pair<std::string, std::string>> kvs;
  MemTable::Iter it(&m);
  for (it.SeekToFirst(); it.Valid(); it.Next()) {
    std::string ikey;
    AppendInternalKey(&ikey, it.user_key(), it.tag());
    kvs.emplace_back(ikey, it.value().ToString());
  }
  ASSERT_EQ(200u, kvs.size());

  TestEnvironment t;
  const Written w = WriteTable(&t, kvs);
  ASSERT_TRUE(w.status.ok()) << w.status.ToString();
  const TableCheck v = ValidateTable(Slice(w.bytes));
  ASSERT_TRUE(v.ok()) << TableFaultName(v.fault) << ": " << v.why;
  EXPECT_EQ(200u, v.entries);
}

// -------------------------------------------------------- and when I/O fails

TEST(SstWriter, AFailedAppendIsReportedAndNotSwallowed) {
  // A writer that returned ok after a failed write would produce a file the
  // flush path then names in the manifest -- which is B2-D5's ordering giving
  // a guarantee about a table that was never written.
  //
  // The ordinal is found by RUNNING the workload once and reading the harness's
  // own ledger, rather than by counting Env calls in one's head: a hand-counted
  // ordinal is a fixture that silently stops pointing at the call it named.
  TestEnvironment probe;
  const Written clean = WriteTable(&probe, Sequential(400, 64));
  ASSERT_TRUE(clean.status.ok());
  uint64_t third_append = 0;
  int seen = 0;
  for (const auto& e : probe.ledger()) {
    if (e.site == CallSite::kWritableFileAppend && ++seen == 3) {
      third_append = e.ordinal;
      break;
    }
  }
  ASSERT_GT(third_append, 0u) << "the workload never appended three times";

  FaultPlan plan;
  plan.At(third_append, Injection::kIoError);
  TestEnvironment t(plan);
  const Written w = WriteTable(&t, Sequential(400, 64));
  EXPECT_FALSE(w.status.ok());
  // And the writer stopped: nothing after the failed append reached the file.
  EXPECT_LT(w.bytes.size(), clean.bytes.size());
}

// ------------------------------------------------------- range tombstones
//
// THE WRITER, CHECKED AGAINST RULES FIXED BEFORE IT EXISTED. B3.2 landed the
// range-block format and its seven refusals with no writer in the tree; these
// assert that what this writer emits is what that classifier accepts, and that
// the two facts the format DERIVES rather than stores are true of real output.

uint64_t RangeTag(SeqNum seq) { return MakeTag(seq, ValueType::kDeletion); }

struct RangeBounds {
  uint64_t file_bytes = 0;
  std::string smallest;
  std::string largest;
  uint64_t largest_seq = 0;
};

std::string BuiltWithRanges(
    const std::vector<std::pair<std::string, std::string>>& points,
    const std::vector<std::tuple<std::string, std::string, SeqNum>>& ranges,
    RangeBounds* meta) {
  testenv::TestEnvironment t;
  EXPECT_TRUE(t.env()->CreateDir("w").ok());
  const std::string path = "w/000001.sst";
  WritableFilePtr f;
  EXPECT_TRUE(t.env()->NewWritableFile(path, &f).ok());
  TableBuilder b(f.get());
  for (const auto& kv : points) b.Add(Slice(kv.first), Slice(kv.second));
  for (const auto& r : ranges) {
    b.AddRangeTombstone(Slice(std::get<0>(r)), Slice(std::get<1>(r)),
                        RangeTag(std::get<2>(r)));
  }
  EXPECT_TRUE(b.Finish().ok());
  if (meta != nullptr) {
    meta->file_bytes = b.file_size();
    meta->smallest = b.smallest().ToString();
    meta->largest = b.largest().ToString();
    meta->largest_seq = b.largest_seq();
  }
  EXPECT_TRUE(f->Sync().ok());
  EXPECT_TRUE(f->Close().ok());
  return t.ContentNow(path);
}

TEST(SstWriter, ATableWithRangeTombstonesIsOneTheClassifierAccepts) {
  RangeBounds meta;
  const std::string image = BuiltWithRanges(
      {{IKey("a", 1), "1"}, {IKey("m", 2), "2"}, {IKey("z", 3), "3"}},
      {{"a", "n", 7}, {"n", "z", 8}}, &meta);
  const TableCheck v = ValidateTable(Slice(image));
  ASSERT_TRUE(v.ok()) << TableFaultName(v.fault) << ": " << v.why;
  EXPECT_EQ(2u, v.range_tombstones);
  EXPECT_EQ(3u, v.entries);
  // The tombstones' sequences count toward the table's largest, because the
  // manifest's number is re-derived from the file and a tombstone is a version.
  EXPECT_EQ(8u, v.largest_seq);
}

TEST(SstWriter, ATableWithoutThemLeavesTheRangeOffsetZero) {
  const std::string image =
      BuiltWithRanges({{IKey("a", 1), "1"}}, {}, nullptr);
  Footer footer;
  std::string why;
  ASSERT_TRUE(DecodeFooter(Slice(image), &footer, &why)) << why;
  EXPECT_EQ(0u, footer.range_offset)
      << "zero is what makes a B2-era table decode as having no range block";
}

// THE LAYOUT RULE, ASSERTED FROM THE BYTES. The size is DERIVED as
// `file_size - kFooterBytes - range_offset`, so the block must end exactly
// where the footer begins. A writer that emitted anything after it would
// corrupt that derivation for every reader; `BM85` is the mutant.
TEST(SstWriter, TheRangeBlockEndsExactlyWhereTheFooterBegins) {
  const std::string image = BuiltWithRanges(
      {{IKey("a", 1), "1"}, {IKey("z", 2), "2"}}, {{"a", "z", 5}}, nullptr);
  Footer footer;
  std::string why;
  ASSERT_TRUE(DecodeFooter(Slice(image), &footer, &why)) << why;
  ASSERT_NE(0u, footer.range_offset);
  const uint64_t footer_at = image.size() - kFooterBytes;
  const uint64_t derived = footer_at - footer.range_offset;
  std::vector<RangeTombstone> out;
  const RangeCheck rc = ParseRangeBlock(
      Slice(image.data() + footer.range_offset, static_cast<std::size_t>(derived)),
      &out);
  ASSERT_TRUE(rc.ok()) << RangeFaultName(rc.fault) << ": " << rc.why;
  EXPECT_EQ(1u, out.size());
  EXPECT_EQ("a", out[0].start.ToString());
  EXPECT_EQ("z", out[0].end.ToString());
}

// AND A WRONG DERIVATION IS LOUD, NOT WRONG. Shifting the recorded offset by one
// byte makes the derived size one larger; the block's crc32c covers exactly the
// bytes the size names, so the disagreement fails the checksum rather than
// producing a tombstone that covers something else. B1's CRC-covering-the-length
// property, one format over.
TEST(SstWriter, AShiftedRangeOffsetFailsTheChecksumRatherThanReadingWrong) {
  std::string image = BuiltWithRanges(
      {{IKey("a", 1), "1"}, {IKey("z", 2), "2"}}, {{"a", "z", 5}}, nullptr);
  Footer footer;
  std::string why;
  ASSERT_TRUE(DecodeFooter(Slice(image), &footer, &why)) << why;
  // FORWARD BY ONE, and the direction matters. Backwards, the offset lands
  // inside the index block and the OVERLAP check refuses it -- also correct,
  // also loud, and a different guard. Forwards leaves the offset legal and
  // makes the derived SIZE one byte short, which is the case this test is for.
  const std::size_t at = image.size() - 20;  // the range offset's first byte
  uint64_t shifted = footer.range_offset + 1;
  for (int i = 0; i < 8; ++i) {
    image[at + i] = static_cast<char>((shifted >> (8 * i)) & 0xff);
  }
  {  // the footer's own CRC must still pass, or this tests the wrong refusal
    char* f = &image[image.size() - kFooterBytes];
    const uint32_t crc = wal::Crc32c(f, kFooterCrcCovers);
    for (int i = 0; i < 4; ++i) {
      f[kFooterCrcCovers + i] = static_cast<char>((crc >> (8 * i)) & 0xff);
    }
  }
  const TableCheck v = ValidateTable(Slice(image));
  EXPECT_FALSE(v.ok());
  EXPECT_EQ(TableFault::kBadRangeBlock, v.fault) << v.why;
}

// THE BOUNDS THE MANIFEST RECORDS MUST ADMIT THE TOMBSTONE, and the writer
// widens them to make it so. Compaction chooses inputs by these bounds, and
// clause 2 of the drop claim is only sound if the inputs hold every version of
// every key they contain.
TEST(SstWriter, ARangeTombstoneWidensTheTablesRecordedBounds) {
  RangeBounds meta;
  BuiltWithRanges({{IKey("m", 1), "1"}}, {{"a", "z", 9}}, &meta);
  EXPECT_EQ("a", ExtractUserKey(Slice(meta.smallest)).ToString());
  EXPECT_EQ("z", ExtractUserKey(Slice(meta.largest)).ToString())
      << "the end bound is exclusive and is widened anyway: over-covering costs "
         "a file that did not need reading, under-covering resurrects data";
}

// B3-Q4 THROUGH THE WRITER. The clear-everything case Amendment [A3] put
// DeleteRange in the interface for, written as one tombstone rather than as one
// point delete per live key.
TEST(SstWriter, AnUnboundedTombstoneIsWrittenAndTheTableSaysSo) {
  testenv::TestEnvironment t;
  ASSERT_TRUE(t.env()->CreateDir("w").ok());
  const std::string path = "w/000001.sst";
  WritableFilePtr f;
  ASSERT_TRUE(t.env()->NewWritableFile(path, &f).ok());
  std::string k1 = IKey("m", 1);
  {
    TableBuilder b(f.get());
    b.Add(Slice(k1), Slice("1"));
    b.AddUnboundedRangeTombstone(Slice("a"), RangeTag(9));
    ASSERT_TRUE(b.Finish().ok());
    // NO FINITE KEY DESCRIBES THE UPPER END, so `largest` stays at the data key
    // and the FLAG is what says the range runs to infinity.
    EXPECT_EQ("a", ExtractUserKey(b.smallest()).ToString());
    EXPECT_EQ("m", ExtractUserKey(b.largest()).ToString());
  }
  ASSERT_TRUE(f->Sync().ok());
  ASSERT_TRUE(f->Close().ok());
  const std::string image = t.ContentNow(path);
  const TableCheck v = ValidateTable(Slice(image));
  ASSERT_TRUE(v.ok()) << TableFaultName(v.fault) << ": " << v.why;
  EXPECT_EQ(1u, v.range_tombstones);
  EXPECT_TRUE(v.unbounded_end);
  EXPECT_EQ("a", ExtractUserKey(Slice(v.smallest_key)).ToString());
  EXPECT_EQ("m", ExtractUserKey(Slice(v.largest_key)).ToString());
}

// GF-14: THE OTHER HALF. A finite tombstone must NOT set the flag, or "the
// table's range runs to infinity" would be true of every table that has one.
TEST(SstWriter, AFiniteTombstoneLeavesTheUnboundedFlagClear) {
  const std::string image = BuiltWithRanges(
      {{IKey("a", 1), "1"}, {IKey("z", 2), "2"}}, {{"a", "z", 5}}, nullptr);
  const TableCheck v = ValidateTable(Slice(image));
  ASSERT_TRUE(v.ok()) << TableFaultName(v.fault) << ": " << v.why;
  EXPECT_FALSE(v.unbounded_end);
}

// MINIMAL REPRODUCTION of the divergence the B4 differential rig found on its
// first outing: the WRITER records bounds the CLASSIFIER does not derive, so
// `VerifyTables` refuses the Open.
//
// THE SHAPE: a range tombstone whose END USER KEY EQUALS the table's largest
// data user key, at a LOWER sequence.
//
//   internal order is user key ascending, TAG DESCENDING -- so at one user key
//   a smaller tag sorts LATER. The writer compares INTERNAL keys, sees the
//   tombstone's end as "greater", and widens `largest_` to it. The classifier
//   compares USER KEYS -- which section 6.1a says is correct, because the bound
//   is a statement about user keys -- sees them equal, and does not widen.
//
// The manifest then records the writer's bound and is held to the classifier's,
// and every Open fails with "key bounds disagree with the manifest".
TEST(SstWriter, ATombstoneEndingAtTheLargestDataKeyDoesNotMoveTheBound) {
  testenv::TestEnvironment t;
  ASSERT_TRUE(t.env()->CreateDir("w").ok());
  const std::string path = "w/000001.sst";
  WritableFilePtr f;
  ASSERT_TRUE(t.env()->NewWritableFile(path, &f).ok());
  const std::string k = IKey("m", 9);          // data at "m", sequence 9
  std::string builder_largest;
  {
    TableBuilder b(f.get());
    b.Add(Slice(k), Slice("v"));
    // The tombstone ENDS at "m" -- the same user key -- with a LOWER sequence.
    b.AddRangeTombstone(Slice("a"), Slice("m"), RangeTag(4));
    ASSERT_TRUE(b.Finish().ok());
    builder_largest = b.largest().ToString();
  }
  ASSERT_TRUE(f->Sync().ok());
  ASSERT_TRUE(f->Close().ok());

  const std::string image = t.ContentNow(path);
  const TableCheck v = ValidateTable(Slice(image));
  ASSERT_TRUE(v.ok()) << TableFaultName(v.fault) << ": " << v.why;

  EXPECT_EQ(v.largest_key, builder_largest)
      << "the writer and the classifier disagree about this table's largest "
         "key, so the manifest records one and is held to the other, and every "
         "Open fails with \"key bounds disagree with the manifest\"";
}

}  // namespace
}  // namespace sst
}  // namespace rift
