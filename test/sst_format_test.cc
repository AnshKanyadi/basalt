// The SSTable classifier, driven from hand-built byte images: no writer, no
// memtable, no Env, no rig. Just fixture bytes and a verdict.
//
// This is B1.7a's ordering ruling applied to the second frozen format. The
// writer lands in B2.2 and is checked against rules that have ALREADY been seen
// to reject every illegal shape -- rather than against a decoder written to
// agree with it. Every fixture below builds something a correct writer will
// never emit, which is the point: a classifier that can only be shown legal
// bytes has not been shown to classify.
#include "range_tombstone.h"
#include "table_check.h"

#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "crc32c.h"
#include "table_format.h"
#include "internal_key.h"
#include "memtable.h"
#include "basalt/slice.h"

namespace basalt {
namespace sst {
namespace {

using KV = std::pair<std::string, std::string>;

// EVERY KEY IN THESE FIXTURES IS A REAL INTERNAL KEY. Single-letter keys were
// what let the first draft of this file compare raw bytes and look correct:
// with no tag, memcmp and the internal order agree, and the one case where they
// disagree -- two versions of one user key -- could not be written down.
std::string IKey(const std::string& user, SeqNum seq,
                 ValueType type = ValueType::kValue) {
  std::string out;
  AppendInternalKey(&out, Slice(user), MakeTag(seq, type));
  return out;
}

std::string DataBlock(const std::vector<KV>& kvs) {
  BlockBuilder b;
  for (const KV& kv : kvs) b.Add(Slice(kv.first), Slice(kv.second));
  return b.Finish();
}

std::string IndexBlock(const std::vector<std::pair<std::string, BlockHandle>>& v) {
  BlockBuilder b;
  for (const auto& e : v) {
    std::string handle;
    EncodeHandle(e.second, &handle);
    b.Add(Slice(e.first), Slice(handle));
  }
  return b.Finish();
}

// Assembles arbitrary table images, legal and otherwise. Every footer field is
// settable and the index is supplied by the caller, so an index that lies about
// its blocks -- the shape B2-D2 exists to make checkable -- is constructible.
class Table {
 public:
  BlockHandle Append(const std::string& block) {
    BlockHandle h;
    h.offset = bytes_.size();
    h.size = static_cast<uint32_t>(block.size());
    bytes_.append(block);
    return h;
  }
  void SetFilter(BlockHandle h) { filter_ = h; }
  void SetVersion(uint32_t v) { version_ = v; }
  void SetRangeOffset(uint64_t o) { range_offset_ = o; }

  std::string Finish(BlockHandle index) const {
    std::string out = bytes_;
    Footer f;
    f.filter = filter_;
    f.index = index;
    f.format_version = version_;
    f.range_offset = range_offset_;
    EncodeFooter(f, &out);
    return out;
  }

 private:
  std::string bytes_;
  BlockHandle filter_;
  uint32_t version_ = kFormatVersion;
  uint64_t range_offset_ = 0;
};

// The canonical LEGAL table: two data blocks, an index naming each block's
// exact last key. Tests below corrupt exactly one thing about it, so a failure
// names the thing that was corrupted and nothing else.
struct Canonical {
  std::string image;
  BlockHandle block0;
  BlockHandle block1;
  BlockHandle index;
};

Canonical Good() {
  Canonical c;
  Table t;
  c.block0 = t.Append(DataBlock({{IKey("a", 1), "1"}, {IKey("b", 1), "2"}}));
  c.block1 = t.Append(DataBlock({{IKey("c", 1), "3"}, {IKey("d", 1), "4"}}));
  const std::string index =
      IndexBlock({{IKey("b", 1), c.block0}, {IKey("d", 1), c.block1}});
  c.index = t.Append(index);
  c.image = t.Finish(c.index);
  return c;
}

// Recomputes a block's trailing checksum in place, so a test can corrupt the
// CONTENT of a block and still be testing the thing it means to test rather
// than the checksum it broke on the way.
void RestampFooterCrc(std::string* image) {
  char* p = &(*image)[image->size() - kFooterBytes];
  const uint32_t crc = wal::Crc32c(p, kFooterCrcCovers);
  for (int i = 0; i < 4; ++i) {
    p[kFooterCrcCovers + i] = static_cast<char>((crc >> (8 * i)) & 0xff);
  }
}

void RestampBlockCrc(std::string* image, const BlockHandle& h) {
  char* p = &(*image)[h.offset];
  const uint32_t crc = wal::Crc32c(p, h.size - kBlockTrailerBytes);
  for (int i = 0; i < 4; ++i) {
    p[h.size - kBlockTrailerBytes + i] = static_cast<char>((crc >> (8 * i)) & 0xff);
  }
}

TableCheck Check(const std::string& image) { return ValidateTable(Slice(image)); }

// --------------------------------------------------------------- the format

TEST(SstFormat, HandleRoundTrip) {
  BlockHandle in;
  in.offset = 0x0102030405060708ull;
  in.size = 0x0a0b0c0du;
  std::string encoded;
  EncodeHandle(in, &encoded);
  ASSERT_EQ(kHandleBytes, encoded.size());
  BlockHandle out;
  ASSERT_TRUE(DecodeHandle(Slice(encoded), &out));
  EXPECT_EQ(in.offset, out.offset);
  EXPECT_EQ(in.size, out.size);
  // A handle is FIXED WIDTH, and a decoder that accepted a short one would let
  // an index entry of the wrong length through as a plausible handle.
  const std::string short_handle = encoded.substr(0, kHandleBytes - 1);
  EXPECT_FALSE(DecodeHandle(Slice(short_handle), &out));
  const std::string long_handle = encoded + '\0';
  EXPECT_FALSE(DecodeHandle(Slice(long_handle), &out));
}

TEST(SstFormat, FooterRoundTripAndWidth) {
  Footer in;
  in.filter.offset = 11;
  in.filter.size = 22;
  in.index.offset = 33;
  in.index.size = 44;
  in.format_version = kFormatVersion;
  std::string image = "some data blocks before the footer";
  const std::size_t before = image.size();
  EncodeFooter(in, &image);
  ASSERT_EQ(kFooterBytes, image.size() - before);
  // The magic sits at a KNOWN offset from the end. Pinned here because the
  // classifier reads it before trusting anything else in the file.
  EXPECT_EQ(0, std::memcmp(image.data() + image.size() - 12, kMagic, sizeof(kMagic)));

  Footer out;
  std::string why;
  ASSERT_TRUE(DecodeFooter(Slice(image), &out, &why)) << why;
  EXPECT_EQ(in.filter.offset, out.filter.offset);
  EXPECT_EQ(in.filter.size, out.filter.size);
  EXPECT_EQ(in.index.offset, out.index.offset);
  EXPECT_EQ(in.index.size, out.index.size);
  EXPECT_EQ(in.format_version, out.format_version);
}

TEST(SstFormat, FooterMagicIsReportedBeforeChecksum) {
  // A FOREIGN FILE IS A DIFFERENT REPORT FROM A CORRUPTED ONE. Both fields are
  // wrong here; the verdict must be the one an operator can act on.
  Footer f;
  f.format_version = kFormatVersion;
  std::string image;
  EncodeFooter(f, &image);
  image[image.size() - 12] = 'X';        // break the magic
  image[image.size() - 1] ^= 0x01;       // and the checksum
  Footer out;
  std::string why;
  EXPECT_FALSE(DecodeFooter(Slice(image), &out, &why));
  EXPECT_NE(std::string::npos, why.find("magic")) << why;
}

TEST(SstFormat, BlockBuilderPlacesARestartEveryInterval) {
  // The restart interval is part of the frozen shape, so it is asserted rather
  // than assumed: a builder that emitted one restart per block would still
  // round-trip, and the index it produces would still be legal.
  BlockBuilder b;
  std::vector<std::string> keys;
  const std::size_t n = kRestartInterval * 2 + 3;
  for (std::size_t i = 0; i < n; ++i) {
    keys.push_back(IKey("key" + std::string(1, static_cast<char>('a' + i)), 1));
  }
  for (const std::string& k : keys) b.Add(Slice(k), Slice(k));
  const std::string block = b.Finish();

  std::vector<BlockEntry> entries;
  std::vector<uint32_t> restarts;
  std::string why;
  ASSERT_TRUE(ParseBlock(Slice(block), &entries, &restarts, &why)) << why;
  EXPECT_EQ(n, entries.size());
  EXPECT_EQ(3u, restarts.size());
  EXPECT_EQ(0u, restarts[0]);
  // Every restart offset must be the offset of an entry, not merely in range.
  for (uint32_t r : restarts) {
    bool matches_an_entry = false;
    for (const BlockEntry& e : entries) matches_an_entry |= (e.offset == r);
    EXPECT_TRUE(matches_an_entry) << "restart " << r << " is not an entry offset";
  }
}

TEST(SstFormat, EmptyValuesAndEmptyKeysRoundTrip) {
  // A tombstone is an entry with an empty value. If the parser treated a zero
  // length as absence, deletes would decode as something else entirely.
  const std::string block = DataBlock({{IKey("a", 1), ""}, {IKey("b", 1), "v"}});
  std::vector<BlockEntry> entries;
  std::vector<uint32_t> restarts;
  std::string why;
  ASSERT_TRUE(ParseBlock(Slice(block), &entries, &restarts, &why)) << why;
  ASSERT_EQ(2u, entries.size());
  EXPECT_EQ(0u, entries[0].value.size());
  EXPECT_EQ("v", entries[1].value.ToString());
}

// ------------------------------------------------------- the legal baseline

TEST(SstClassifier, AcceptsACanonicalTable) {
  const Canonical c = Good();
  const TableCheck v = Check(c.image);
  ASSERT_TRUE(v.ok()) << TableFaultName(v.fault) << ": " << v.why;
  // ANY COUNT A RUN PRINTS IS EITHER ASSERTED OR DELETED.
  EXPECT_EQ(2u, v.data_blocks);
  EXPECT_EQ(4u, v.entries);
}

TEST(SstClassifier, AcceptsATableWithAFilterBlock) {
  // The filter is not PARSED until B2.1; its handle is range-checked from
  // B2.0, so a table carrying one must already validate.
  Table t;
  const BlockHandle b0 = t.Append(DataBlock({{IKey("a", 1), "1"}}));
  const std::string filter_bytes(64, '\xEE');
  const BlockHandle f = t.Append(filter_bytes);
  t.SetFilter(f);
  const BlockHandle idx = t.Append(IndexBlock({{IKey("a", 1), b0}}));
  const TableCheck v = Check(t.Finish(idx));
  ASSERT_TRUE(v.ok()) << TableFaultName(v.fault) << ": " << v.why;
  EXPECT_EQ(1u, v.data_blocks);
}

// ------------------------------------------------------- the rejection list

TEST(SstClassifier, RejectsAnImageTooSmallForAFooter) {
  const std::string tiny(kFooterBytes - 1, '\0');
  EXPECT_EQ(TableFault::kTooSmall, Check(tiny).fault);
  EXPECT_EQ(TableFault::kTooSmall, Check(std::string()).fault);
}

TEST(SstClassifier, RejectsAForeignFile) {
  Canonical c = Good();
  c.image[c.image.size() - 12] = 'X';
  const TableCheck v = Check(c.image);
  EXPECT_EQ(TableFault::kBadMagic, v.fault);
  EXPECT_EQ(c.image.size() - kFooterBytes, v.offset);
}

TEST(SstClassifier, RejectsADamagedFooter) {
  // One bit inside the CRC's coverage, magic left intact: a DAMAGED SSTable,
  // reported differently from a foreign one.
  Canonical c = Good();
  c.image[c.image.size() - kFooterBytes] ^= 0x01;
  EXPECT_EQ(TableFault::kBadFooterChecksum, Check(c.image).fault);
}

TEST(SstClassifier, RejectsAnUnknownFormatVersion) {
  // With a VALID checksum over the wrong version: this is a file from a future
  // build, not a corrupt one, and it must be refused rather than guessed at.
  Table t;
  const BlockHandle b0 = t.Append(DataBlock({{IKey("a", 1), "1"}}));
  const BlockHandle idx = t.Append(IndexBlock({{IKey("a", 1), b0}}));
  t.SetVersion(kFormatVersion + 1);
  const TableCheck v = Check(t.Finish(idx));
  EXPECT_EQ(TableFault::kUnknownFormatVersion, v.fault);
  EXPECT_NE(std::string::npos, v.why.find(std::to_string(kFormatVersion + 1)));
}

TEST(SstClassifier, RejectsAnIndexHandlePastTheFooter) {
  Table t;
  const BlockHandle b0 = t.Append(DataBlock({{IKey("a", 1), "1"}}));
  BlockHandle idx = t.Append(IndexBlock({{IKey("a", 1), b0}}));
  idx.size += 1;  // one byte past where the footer begins
  EXPECT_EQ(TableFault::kHandleOutOfRange, Check(t.Finish(idx)).fault);
}

TEST(SstClassifier, RejectsAFilterHandlePastTheFooter) {
  Table t;
  const BlockHandle b0 = t.Append(DataBlock({{IKey("a", 1), "1"}}));
  const BlockHandle idx = t.Append(IndexBlock({{IKey("a", 1), b0}}));
  BlockHandle bogus;
  bogus.offset = 1;
  bogus.size = 0xFFFFFFFFu;
  t.SetFilter(bogus);
  const TableCheck v = Check(t.Finish(idx));
  EXPECT_EQ(TableFault::kHandleOutOfRange, v.fault);
  EXPECT_NE(std::string::npos, v.why.find("filter")) << v.why;
}

TEST(SstClassifier, RejectsAnOffsetThatWouldOverflowWhenAdded) {
  // offset + size must not be computed by ADDING them. A handle near 2^64
  // would wrap and land back inside the file, which is a range check that
  // reports the bytes as fine.
  Table t;
  const BlockHandle b0 = t.Append(DataBlock({{IKey("a", 1), "1"}}));
  const BlockHandle idx = t.Append(IndexBlock({{IKey("a", 1), b0}}));
  BlockHandle huge;
  huge.offset = 0xFFFFFFFFFFFFFFF0ull;
  huge.size = 64;
  t.SetFilter(huge);
  EXPECT_EQ(TableFault::kHandleOutOfRange, Check(t.Finish(idx)).fault);
}

TEST(SstClassifier, RejectsADataBlockWithABadChecksum) {
  Canonical c = Good();
  c.image[c.block1.offset + 5] ^= 0x01;  // corrupt, and do NOT restamp
  const TableCheck v = Check(c.image);
  EXPECT_EQ(TableFault::kBadBlockChecksum, v.fault);
  EXPECT_EQ(c.block1.offset, v.offset);
}

TEST(SstClassifier, RejectsAnIndexBlockWithABadChecksum) {
  Canonical c = Good();
  c.image[c.index.offset + 5] ^= 0x01;
  const TableCheck v = Check(c.image);
  EXPECT_EQ(TableFault::kBadBlockChecksum, v.fault);
  EXPECT_EQ(c.index.offset, v.offset);
}

TEST(SstClassifier, RejectsABlockDeclaringZeroRestarts) {
  Canonical c = Good();
  const std::size_t count_at = c.block0.offset + c.block0.size - kBlockTrailerBytes - 4;
  for (int i = 0; i < 4; ++i) c.image[count_at + i] = 0;
  RestampBlockCrc(&c.image, c.block0);
  const TableCheck v = Check(c.image);
  EXPECT_EQ(TableFault::kMalformedBlock, v.fault);
  EXPECT_NE(std::string::npos, v.why.find("restart")) << v.why;
}

TEST(SstClassifier, RejectsARestartArrayThatDoesNotFit) {
  // A CORRUPT COUNT MUST NOT MAKE THE READER WALK OFF THE BLOCK. The count is
  // checked against the block's own size before the array is read, which is
  // section 5.3.3's rule about lengths, one format over.
  Canonical c = Good();
  const std::size_t count_at = c.block0.offset + c.block0.size - kBlockTrailerBytes - 4;
  const uint32_t absurd = 0x40000000u;
  for (int i = 0; i < 4; ++i) {
    c.image[count_at + i] = static_cast<char>((absurd >> (8 * i)) & 0xff);
  }
  RestampBlockCrc(&c.image, c.block0);
  const TableCheck v = Check(c.image);
  EXPECT_EQ(TableFault::kMalformedBlock, v.fault);
  EXPECT_NE(std::string::npos, v.why.find("does not fit")) << v.why;
}

TEST(SstClassifier, RejectsARestartOffsetPastTheEntries) {
  Canonical c = Good();
  // The last restart slot sits immediately before the count.
  const std::size_t restart_at = c.block0.offset + c.block0.size - kBlockTrailerBytes - 8;
  const uint32_t absurd = 0xFFFFu;
  for (int i = 0; i < 4; ++i) {
    c.image[restart_at + i] = static_cast<char>((absurd >> (8 * i)) & 0xff);
  }
  RestampBlockCrc(&c.image, c.block0);
  const TableCheck v = Check(c.image);
  EXPECT_EQ(TableFault::kMalformedBlock, v.fault);
  EXPECT_NE(std::string::npos, v.why.find("past the entries")) << v.why;
}

TEST(SstClassifier, RejectsAKeyLengthRunningPastTheBlock) {
  Canonical c = Good();
  const uint32_t absurd = 0xFFFFu;
  for (int i = 0; i < 4; ++i) {
    c.image[c.block0.offset + i] = static_cast<char>((absurd >> (8 * i)) & 0xff);
  }
  RestampBlockCrc(&c.image, c.block0);
  const TableCheck v = Check(c.image);
  EXPECT_EQ(TableFault::kMalformedBlock, v.fault);
  EXPECT_NE(std::string::npos, v.why.find("runs past")) << v.why;
}

TEST(SstClassifier, AcceptsTwoVersionsOfOneKeyNewestFirst) {
  // THE CASE MEMCMP GETS BACKWARDS, and the reason internal_key.h exists.
  // Tags are stored little-endian, so bytewise these two entries DESCEND -- a
  // classifier comparing raw bytes refuses this table, which is legal and is
  // what every flush of a key written twice produces.
  Table t;
  const BlockHandle b0 = t.Append(DataBlock({{IKey("a", 9), "new"},
                                             {IKey("a", 4), "old"},
                                             {IKey("b", 1), "x"}}));
  const BlockHandle idx = t.Append(IndexBlock({{IKey("b", 1), b0}}));
  const TableCheck v = Check(t.Finish(idx));
  ASSERT_TRUE(v.ok()) << TableFaultName(v.fault) << ": " << v.why;
  EXPECT_EQ(3u, v.entries);
}

TEST(SstClassifier, RejectsTwoVersionsOfOneKeyOldestFirst) {
  // The other half, and the half a bytewise classifier ACCEPTS. A reader
  // seeking a snapshot in this block finds the OLDEST visible version of "a" at
  // every timestamp -- a wrong answer with no corruption anywhere in the file,
  // which is why the order is checked at all.
  Table t;
  const BlockHandle b0 = t.Append(DataBlock({{IKey("a", 4), "old"},
                                             {IKey("a", 9), "new"}}));
  const BlockHandle idx = t.Append(IndexBlock({{IKey("a", 9), b0}}));
  EXPECT_EQ(TableFault::kEntriesNotAscending, Check(t.Finish(idx)).fault);
}

TEST(SstClassifier, RejectsAnIndexOrderedByBytesRatherThanByVersion) {
  // The same disagreement one level up: two blocks whose last keys are versions
  // of one user key. Bytewise, block1's separator precedes block0's.
  Table t;
  const BlockHandle b0 = t.Append(DataBlock({{IKey("a", 9), "new"}}));
  const BlockHandle b1 = t.Append(DataBlock({{IKey("a", 4), "old"}}));
  const BlockHandle bad =
      t.Append(IndexBlock({{IKey("a", 4), b1}, {IKey("a", 9), b0}}));
  EXPECT_EQ(TableFault::kIndexNotAscending, Check(t.Finish(bad)).fault);

  // And the legal pairing of the same two blocks, so the rejection above is
  // about ORDER and not about the blocks.
  Table u;
  const BlockHandle u0 = u.Append(DataBlock({{IKey("a", 9), "new"}}));
  const BlockHandle u1 = u.Append(DataBlock({{IKey("a", 4), "old"}}));
  const BlockHandle good =
      u.Append(IndexBlock({{IKey("a", 9), u0}, {IKey("a", 4), u1}}));
  const TableCheck v = Check(u.Finish(good));
  EXPECT_TRUE(v.ok()) << TableFaultName(v.fault) << ": " << v.why;
}

TEST(SstClassifier, RejectsAKeyTooShortToCarryATag) {
  // A CORRUPT FILE MUST PRODUCE A VERDICT, NOT AN ABORT. ExtractUserKey
  // BASALT_CHECKs its length because in-process keys are well formed by
  // construction; anything reading a FILE checks first and reports.
  Table t;
  BlockBuilder b;
  const std::string stub = "abc";  // three bytes: no room for a tag
  b.Add(Slice(stub), Slice("v"));
  const BlockHandle b0 = t.Append(b.Finish());
  const BlockHandle idx = t.Append(IndexBlock({{IKey("a", 1), b0}}));
  const TableCheck v = Check(t.Finish(idx));
  EXPECT_EQ(TableFault::kNotAnInternalKey, v.fault);
  EXPECT_EQ(b0.offset, v.offset);
}

TEST(SstClassifier, RejectsAnIndexKeyTooShortToCarryATag) {
  Table t;
  const BlockHandle b0 = t.Append(DataBlock({{IKey("a", 1), "1"}}));
  BlockBuilder b;
  const std::string stub = "abc";
  std::string handle;
  EncodeHandle(b0, &handle);
  b.Add(Slice(stub), Slice(handle));
  const BlockHandle idx = t.Append(b.Finish());
  EXPECT_EQ(TableFault::kNotAnInternalKey, Check(t.Finish(idx)).fault);
}

TEST(SstClassifier, TheMemtableEmitsTableOrder) {
  // THE FLUSH'S PRECONDITION, ASSERTED BEFORE THE FLUSH EXISTS. B2.4 writes the
  // memtable out in iterator order and the result must be a legal table, so the
  // two orders have to be the same order. They are implemented separately --
  // the memtable compares a stored entry against a (user_key, tag) pair, the
  // classifier compares two encoded keys -- and this is what would catch them
  // drifting apart.
  MemTable m;
  for (SeqNum seq = 1; seq <= 5; ++seq) {
    for (const char* user : {"alpha", "beta", "gamma"}) {
      const std::string u(user);
      m.Add(seq, seq % 2 == 0 ? ValueType::kDeletion : ValueType::kValue,
            Slice(u), Slice(u));
    }
  }
  MemTable::Iter it(&m);
  std::string previous;
  std::size_t n = 0;
  for (it.SeekToFirst(); it.Valid(); it.Next()) {
    std::string ikey;
    AppendInternalKey(&ikey, it.user_key(), it.tag());
    if (n > 0) {
      EXPECT_GT(CompareInternalKey(Slice(ikey), Slice(previous)), 0)
          << "the memtable emitted an order no SSTable may hold, at entry " << n;
    }
    previous = ikey;
    ++n;
  }
  EXPECT_EQ(15u, n);
}

TEST(SstClassifier, RejectsEntriesThatDoNotAscend) {
  Table t;
  const BlockHandle b0 = t.Append(DataBlock({{IKey("b", 1), "1"}, {IKey("a", 1), "2"}}));
  const BlockHandle idx = t.Append(IndexBlock({{IKey("a", 1), b0}}));
  EXPECT_EQ(TableFault::kEntriesNotAscending, Check(t.Finish(idx)).fault);
}

TEST(SstClassifier, RejectsDuplicateKeysWithinABlock) {
  // STRICTLY ascending, not merely non-descending. Two entries with the same
  // internal key -- same user key AND same tag -- are two versions the merge
  // order cannot distinguish, and the reader would silently pick one.
  Table t;
  const BlockHandle b0 = t.Append(DataBlock({{IKey("a", 1), "1"}, {IKey("a", 1), "2"}}));
  const BlockHandle idx = t.Append(IndexBlock({{IKey("a", 1), b0}}));
  EXPECT_EQ(TableFault::kEntriesNotAscending, Check(t.Finish(idx)).fault);
}

TEST(SstClassifier, RejectsAnIndexThatDoesNotAscend) {
  Table t;
  const BlockHandle b0 = t.Append(DataBlock({{IKey("c", 1), "1"}}));
  const BlockHandle b1 = t.Append(DataBlock({{IKey("a", 1), "2"}}));
  const BlockHandle idx = t.Append(IndexBlock({{IKey("c", 1), b0}, {IKey("a", 1), b1}}));
  EXPECT_EQ(TableFault::kIndexNotAscending, Check(t.Finish(idx)).fault);
}

TEST(SstClassifier, RejectsAnIndexSeparatorThatIsNotTheBlocksLastKey) {
  // B2-D2 MADE CHECKABLE, and the reason the decision was taken. A separator
  // that merely SEPARATES -- LevelDB's -- would leave nothing in the table to
  // compare against, and this assertion could not be written at all.
  Table t;
  const BlockHandle b0 = t.Append(DataBlock({{IKey("a", 1), "1"}, {IKey("b", 1), "2"}}));
  const BlockHandle b1 = t.Append(DataBlock({{IKey("c", 1), "3"}, {IKey("d", 1), "4"}}));
  // "bb" separates the two blocks correctly. It is still refused.
  const BlockHandle idx = t.Append(IndexBlock({{IKey("bb", 1), b0}, {IKey("d", 1), b1}}));
  const TableCheck v = Check(t.Finish(idx));
  EXPECT_EQ(TableFault::kIndexSeparatorMismatch, v.fault);
  // The offset must land inside the INDEX -- the entry that lied -- and not on
  // the data block, which is blameless.
  EXPECT_GE(v.offset, idx.offset);
  EXPECT_LT(v.offset, idx.offset + idx.size);
}

TEST(SstClassifier, RejectsAnIndexPointingAtTheWrongBlock) {
  // The separators are correct keys, and each names the last key of THE OTHER
  // block. Both blocks are individually legal; only the pairing is wrong.
  Table t;
  const BlockHandle b0 = t.Append(DataBlock({{IKey("a", 1), "1"}, {IKey("b", 1), "2"}}));
  const BlockHandle b1 = t.Append(DataBlock({{IKey("c", 1), "3"}, {IKey("d", 1), "4"}}));
  const BlockHandle idx = t.Append(IndexBlock({{IKey("b", 1), b1}, {IKey("d", 1), b0}}));
  EXPECT_EQ(TableFault::kIndexSeparatorMismatch, Check(t.Finish(idx)).fault);
}

TEST(SstClassifier, RejectsAnIndexValueThatIsNotAHandle) {
  Table t;
  t.Append(DataBlock({{IKey("a", 1), "1"}}));
  BlockBuilder b;
  const std::string not_a_handle = "short";
  const std::string ikey = IKey("a", 1);
  b.Add(Slice(ikey), Slice(not_a_handle));
  const BlockHandle idx = t.Append(b.Finish());
  const TableCheck v = Check(t.Finish(idx));
  EXPECT_EQ(TableFault::kMalformedBlock, v.fault);
  EXPECT_NE(std::string::npos, v.why.find("handle")) << v.why;
}

TEST(SstClassifier, RejectsADataHandlePastTheFooter) {
  Table t;
  const BlockHandle real = t.Append(DataBlock({{IKey("a", 1), "1"}}));
  BlockHandle bogus = real;
  bogus.size = 0xFFFFFFFFu;
  const BlockHandle idx = t.Append(IndexBlock({{IKey("a", 1), bogus}}));
  const TableCheck v = Check(t.Finish(idx));
  EXPECT_EQ(TableFault::kHandleOutOfRange, v.fault);
  EXPECT_NE(std::string::npos, v.why.find("data block")) << v.why;
}

TEST(SstClassifier, RejectsATableWhoseIndexNamesNoBlocks) {
  // An SSTable with no data is not a legal file: the flush that produced it had
  // nothing to write, and B2.4 must not create one rather than skip the flush.
  Table t;
  const BlockHandle idx = t.Append(IndexBlock({}));
  EXPECT_EQ(TableFault::kEmptyTable, Check(t.Finish(idx)).fault);
}

TEST(SstClassifier, RejectsAnEmptyDataBlock) {
  Table t;
  const BlockHandle empty = t.Append(DataBlock({}));
  const BlockHandle idx = t.Append(IndexBlock({{IKey("a", 1), empty}}));
  const TableCheck v = Check(t.Finish(idx));
  EXPECT_EQ(TableFault::kMalformedBlock, v.fault);
  EXPECT_NE(std::string::npos, v.why.find("empty")) << v.why;
}

// ------------------------------------------------- the classifier's closure

TEST(SstClassifier, EveryFaultHasADistinctName) {
  // TableFaultName's switch has no default arm, so an unnamed enumerator is a
  // build failure. This asserts the second half: that the names are distinct,
  // because two faults sharing a name is a verdict nobody can act on.
  const TableFault all[] = {
      TableFault::kNone,
      TableFault::kTooSmall,
      TableFault::kBadMagic,
      TableFault::kBadFooterChecksum,
      TableFault::kUnknownFormatVersion,
      TableFault::kHandleOutOfRange,
      TableFault::kBadBlockChecksum,
      TableFault::kMalformedBlock,
      TableFault::kEntriesNotAscending,
      TableFault::kIndexNotAscending,
      TableFault::kIndexSeparatorMismatch,
      TableFault::kEmptyTable,
  };
  std::vector<std::string> names;
  for (TableFault f : all) {
    const std::string name = TableFaultName(f);
    EXPECT_FALSE(name.empty());
    for (const std::string& seen : names) EXPECT_NE(seen, name);
    names.push_back(name);
  }
}

TEST(SstClassifier, EveryByteOfACanonicalTableMatters) {
  // THE STRONGEST STATEMENT THIS FILE CAN MAKE, and it is a sweep rather than a
  // claim: flip one bit in every byte of a legal table and require the
  // classifier to refuse each one, with NO EXCEPTIONS -- the reserved footer
  // bytes included, because the footer's checksum covers them even though the
  // reader ignores their value. A byte that can be flipped without notice is a
  // byte no checksum covers, which is exactly how section 5.3.3's
  // length-outside-the-CRC defect would present if it were reintroduced here.
  const Canonical c = Good();
  std::size_t checked = 0;
  for (std::size_t i = 0; i < c.image.size(); ++i) {
    std::string damaged = c.image;
    damaged[i] ^= 0x01;
    const TableCheck v = ValidateTable(Slice(damaged));
    EXPECT_FALSE(v.ok()) << "byte " << i << " is not covered by any check";
    ++checked;
  }
  EXPECT_EQ(c.image.size(), checked);
  EXPECT_GT(checked, kFooterBytes);
}

// THE RESERVE, SPENT AT B3.5 -- AND THIS TEST IS REWRITTEN RATHER THAN LOOSENED.
//
// B2 asserted TWO properties of the eight reserved bytes, and only one of them
// could survive the reserve being spent:
//
//   WRITTEN ZERO  -- still true, and now load-bearing for a different reason: a
//                    B2-era table decodes as `range_offset == 0`, which means
//                    "no range block", so it reads correctly on this build.
//   NOT READ      -- GONE, necessarily. The reader now reads those bytes as a
//                    range-block offset, so a file "from a future build" that
//                    put something else there is REFUSED rather than ignored.
//
// The second property was forward compatibility, and **spending a reserve is
// exactly the act that ends it.** That is the half of the reserve's cost B2 did
// not price, and it is recorded in BUGS.md GF-17 rather than quietly dropped.
TEST(SstClassifier, TheSpentReserveIsZeroWithNoRangeBlockAndRefusedWhenItLies) {
  const Canonical c = Good();
  const std::size_t reserved_begin = c.image.size() - 20;
  for (std::size_t i = 0; i < 8; ++i) {
    EXPECT_EQ('\0', c.image[reserved_begin + i])
        << "byte " << i << " of the range offset, with no range block";
  }

  std::string lying = c.image;
  for (std::size_t i = 0; i < 8; ++i) {
    lying[reserved_begin + i] = static_cast<char>(0xFF);
  }
  RestampFooterCrc(&lying);
  const TableCheck v = ValidateTable(Slice(lying));
  EXPECT_FALSE(v.ok()) << "a range offset past the footer must be refused, not ignored";
  EXPECT_EQ(TableFault::kHandleOutOfRange, v.fault);
}

// ------------------------------- the range block, from hand-built bytes
//
// SECTION 6.1's REFUSAL THAT IS NOT ABOUT THE BLOCK, INDUCED. The writer always
// widens a table's bounds to admit its own tombstones, so this shape cannot be
// produced by `TableBuilder` -- which is exactly why it is built by hand. A rule
// only the writer can be trusted to keep is a rule that is not checked.

// A range block holding one tombstone, framed as a data block: the tombstone is
// the ENTRY KEY and the value is empty.
std::string RangeBlock(const std::vector<std::tuple<std::string, std::string, SeqNum>>& ts) {
  BlockBuilder b;
  for (const auto& t : ts) {
    std::string encoded;
    EncodeRangeTombstone(Slice(std::get<0>(t)), Slice(std::get<1>(t)),
                         MakeTag(std::get<2>(t), ValueType::kDeletion), &encoded);
    b.Add(Slice(encoded), Slice());
  }
  return b.Finish();
}

// A table whose only data key is "m", carrying a range block the caller chooses.
std::string TableWithRange(const std::vector<std::tuple<std::string, std::string, SeqNum>>& ts) {
  Table t;
  const BlockHandle block0 = t.Append(DataBlock({{IKey("m", 1), "1"}}));
  const BlockHandle index = t.Append(IndexBlock({{IKey("m", 1), block0}}));
  const BlockHandle range = t.Append(RangeBlock(ts));
  t.SetRangeOffset(range.offset);
  return t.Finish(index);
}

// THE CLASSIFIER DERIVES THE BOUNDS INCLUDING THE TOMBSTONES, which is section
// 6.1's requirement in its enforceable form (see DESIGN-B3 6.1a). The manifest
// is then held to THIS derivation at every Open, so no manifest can record
// bounds that fail to admit a tombstone.
TEST(SstClassifier, ARangeTombstoneWidensTheBoundsTheClassifierDerives) {
  // The table's only data key is "m"; the tombstone reaches from "a" to "z".
  const std::string image = TableWithRange({{"a", "z", 9}});
  const TableCheck v = ValidateTable(Slice(image));
  ASSERT_TRUE(v.ok()) << TableFaultName(v.fault) << ": " << v.why;
  EXPECT_EQ(1u, v.range_tombstones);
  EXPECT_EQ("a", ExtractUserKey(Slice(v.smallest_key)).ToString());
  EXPECT_EQ("z", ExtractUserKey(Slice(v.largest_key)).ToString())
      << "the end bound is exclusive and is included anyway: over-covering "
         "costs a read, under-covering resurrects data";
  EXPECT_EQ(9u, v.largest_seq) << "a tombstone is a version and counts";
}

// GF-14: THE OTHER HALF. Without it, "tombstones widen the bounds" could be
// produced by a classifier that ignored the data keys entirely.
TEST(SstClassifier, ATombstoneInsideTheDataDoesNotWidenAnything) {
  Table t;
  const BlockHandle block0 =
      t.Append(DataBlock({{IKey("a", 1), "1"}, {IKey("z", 1), "2"}}));
  const BlockHandle index = t.Append(IndexBlock({{IKey("z", 1), block0}}));
  const BlockHandle range = t.Append(RangeBlock({{"m", "n", 9}}));
  t.SetRangeOffset(range.offset);
  const std::string image = t.Finish(index);
  const TableCheck v = ValidateTable(Slice(image));
  ASSERT_TRUE(v.ok()) << TableFaultName(v.fault) << ": " << v.why;
  EXPECT_EQ("a", ExtractUserKey(Slice(v.smallest_key)).ToString());
  EXPECT_EQ("z", ExtractUserKey(Slice(v.largest_key)).ToString());
}

}  // namespace
}  // namespace sst
}  // namespace basalt
