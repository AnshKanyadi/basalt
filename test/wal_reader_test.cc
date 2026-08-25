// The torn-tail rule and fragment-chain legality, driven from hand-built byte
// images: no writer, no memtable, no Env faults, just fixture bytes.
//
// THESE GATES LAND BEFORE THE WRITER IS TRUSTED. Section 14.4 splits the reader
// from recovery because the reader's gates need only B1.1, and section 14.2
// notes this is the cheapest place in the sequence to induce failures
// exhaustively. Building the format's classifier first means the writer's
// output is later checked against rules that have already been seen to reject
// every illegal shape -- rather than against a decoder written to agree with it.
#include "reader.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "crc32c.h"
#include "format.h"
#include "slice.h"

namespace rift {
namespace wal {
namespace {

// Builds arbitrary WAL images, including illegal ones. Everything a real writer
// would refuse to emit has to be constructible here or the six-case table
// cannot be driven.
class Image {
 public:
  // A fragment with a correct checksum. Asserts it fits in its block, because
  // a builder that silently straddled a boundary would be testing the reader
  // against bytes no writer can produce.
  void Fragment(FragmentType type, Slice payload) {
    const uint64_t block_off = bytes_.size() % kBlockBytes;
    EXPECT_LE(block_off + kHeaderBytes + payload.size(), kBlockBytes)
        << "fixture fragment straddles a block boundary";
    Header(FragmentCrc(static_cast<uint16_t>(payload.size()), type, payload),
           static_cast<uint16_t>(payload.size()), type);
    bytes_.append(payload.data(), payload.size());
  }

  // Same, with the checksum deliberately wrong: a torn write that landed on a
  // fragment boundary.
  void BadCrcFragment(FragmentType type, Slice payload) {
    Header(0xDEADBEEFu, static_cast<uint16_t>(payload.size()), type);
    bytes_.append(payload.data(), payload.size());
  }

  // A fragment large enough to fill the remainder of the current block exactly,
  // so the next thing written starts at a block boundary.
  void FragmentFillingBlock(FragmentType type, char fill) {
    const uint64_t block_off = bytes_.size() % kBlockBytes;
    const std::size_t payload = kBlockBytes - block_off - kHeaderBytes;
    const std::string bytes(payload, fill);
    Fragment(type, Slice(bytes));
  }

  std::size_t PayloadToFillBlock() const {
    return kBlockBytes - (bytes_.size() % kBlockBytes) - kHeaderBytes;
  }

  void Zeros(std::size_t n) { bytes_.append(n, '\0'); }
  void Garbage(std::size_t n) { bytes_.append(n, '\xAB'); }
  void PadBlock() { Zeros(kBlockBytes - (bytes_.size() % kBlockBytes)); }

  // Corrupts only the two length bytes of the fragment header at `offset`.
  void CorruptLengthAt(uint64_t offset) {
    bytes_[offset + 4] = static_cast<char>(static_cast<unsigned char>(bytes_[offset + 4]) ^ 0x01);
  }
  void FlipByteAt(uint64_t offset) {
    bytes_[offset] = static_cast<char>(static_cast<unsigned char>(bytes_[offset]) ^ 0x40);
  }

  uint64_t size() const { return bytes_.size(); }
  Slice slice() const { return Slice(bytes_); }

 private:
  void Header(uint32_t crc, uint16_t len, FragmentType type) {
    for (int i = 0; i < 4; ++i) bytes_.push_back(static_cast<char>((crc >> (8 * i)) & 0xff));
    bytes_.push_back(static_cast<char>(len & 0xff));
    bytes_.push_back(static_cast<char>((len >> 8) & 0xff));
    bytes_.push_back(static_cast<char>(static_cast<uint8_t>(type)));
  }
  std::string bytes_;
};

std::string Batch(SeqNum seq, const std::string& key, const std::string& value) {
  std::vector<Op> ops;
  Op op;
  op.kind = OpKind::kSet;
  op.key = Slice(key);
  op.value = Slice(value);
  ops.push_back(op);
  std::string out;
  EncodeBatch(seq, ops, &out);
  return out;
}
// A BATCH whose encoded size is EXACTLY n bytes. Needed because a fragment that
// fills a block must still be a well-formed record when it is a FULL: the
// reader peeks the kind, and a block of 'x' is not a record. (The first draft of
// these fixtures used filler bytes and the reader rejected them at offset 0 --
// correctly, and the tests were wrong.)
std::string BatchOfSize(SeqNum seq, std::size_t n) {
  EXPECT_GE(n, 23u);
  std::vector<Op> ops;
  Op op;
  op.kind = OpKind::kSet;
  const std::string key(n - 23, 'k');
  const std::string value(1, 'v');
  op.key = Slice(key);
  op.value = Slice(value);
  ops.push_back(op);
  std::string out;
  EncodeBatch(seq, ops, &out);
  EXPECT_EQ(out.size(), n);
  return out;
}

std::string GroupEnd(SeqNum high, uint32_t count) {
  std::string out;
  EncodeGroupEnd(high, count, &out);
  return out;
}

// ------------------------------------------------------------------ crc32c

TEST(Crc32c, MatchesTheCanonicalCheckValue) {
  EXPECT_EQ(Crc32c("123456789", 9), kCrc32cCheckValue);
  EXPECT_EQ(Crc32c("", 0), 0u);
}

// THE DIVERGENCE FROM LEVELDB, ASSERTED DIRECTLY.
//
// Same type, same payload, different LENGTH field. Under upstream's coverage
// (`type || data` only) these two checksums would be EQUAL and a corrupted
// length would be undetectable as such. BM10 is the mutant that reverts the
// coverage, and this is the assertion it has to get past.
TEST(Crc32c, TheFragmentChecksumCoversTheLengthField) {
  const Slice payload("abcde", 5);
  EXPECT_NE(FragmentCrc(5, FragmentType::kFull, payload),
            FragmentCrc(4, FragmentType::kFull, payload))
      << "the length field is outside the checksum, so a corrupt length is not "
         "detected as one -- and section 5.4's discriminator needs the failure "
         "point to be a KNOWN OFFSET";
  EXPECT_NE(FragmentCrc(5, FragmentType::kFull, payload),
            FragmentCrc(5, FragmentType::kFirst, payload));
}

// ------------------------------------------------------------ happy paths

TEST(WalReader, ASingleFullRecordScansClean) {
  Image img;
  {
    const std::string payload_1 = Batch(1, "a", "1");
    img.Fragment(FragmentType::kFull, Slice(payload_1));
  }
  {
    const std::string payload_2 = GroupEnd(1, 1);
    img.Fragment(FragmentType::kFull, Slice(payload_2));
  }
  const ScanResult r = ScanLog(img.slice());
  EXPECT_EQ(r.outcome, ScanOutcome::kCleanEnd);
  ASSERT_EQ(r.records.size(), 2u);
  EXPECT_EQ(r.committed_count, 2u);
  EXPECT_EQ(r.last_committed_seq, 1u);
}

TEST(WalReader, AMultiFragmentRecordIsReassembledAcrossBlocks) {
  Image img;
  const std::string big = Batch(1, std::string(40000, 'k'), "v");
  const std::size_t first = kBlockBytes - kHeaderBytes;
  img.Fragment(FragmentType::kFirst, Slice(big.data(), first));
  img.Fragment(FragmentType::kLast, Slice(big.data() + first, big.size() - first));
  {
    const std::string payload_3 = GroupEnd(1, 1);
    img.Fragment(FragmentType::kFull, Slice(payload_3));
  }
  const ScanResult r = ScanLog(img.slice());
  ASSERT_EQ(r.outcome, ScanOutcome::kCleanEnd) << r.failure_reason;
  ASSERT_EQ(r.records.size(), 2u);
  EXPECT_EQ(r.records[0].payload, big) << "the reassembled record is not the "
                                          "record that was fragmented";
}

// BATCH records after the last GROUP_END are the tail. Discarding them is not
// an error and is not reported as one.
TEST(WalReader, BatchesAfterTheLastGroupEndAreNotCommitted) {
  Image img;
  {
    const std::string payload_4 = Batch(1, "a", "1");
    img.Fragment(FragmentType::kFull, Slice(payload_4));
  }
  {
    const std::string payload_5 = GroupEnd(1, 1);
    img.Fragment(FragmentType::kFull, Slice(payload_5));
  }
  {
    const std::string payload_6 = Batch(2, "b", "2");
    img.Fragment(FragmentType::kFull, Slice(payload_6));
  }
  const ScanResult r = ScanLog(img.slice());
  EXPECT_EQ(r.outcome, ScanOutcome::kCleanEnd);
  EXPECT_EQ(r.records.size(), 3u);
  EXPECT_EQ(r.committed_count, 2u) << "an uncommitted batch was banked";
  EXPECT_EQ(r.last_committed_seq, 1u);
}

// --------------------------------------------- the six-case table, 5.4.2

// 1. FIRST, MIDDLE, then EOF -- valid chain, INSIDE at EOF.
TEST(WalReaderSixCases, IncompleteChainAtEofIsATornTail) {
  Image img;
  img.FragmentFillingBlock(FragmentType::kFirst, 'x');
  {
    const std::string payload_7 = std::string(100, 'y');
    img.Fragment(FragmentType::kMiddle, Slice(payload_7));
  }
  const ScanResult r = ScanLog(img.slice());
  EXPECT_EQ(r.outcome, ScanOutcome::kTornTail) << r.failure_reason;
}

// 2. FIRST, MIDDLE, <torn MIDDLE> -- CRC failure while INSIDE.
TEST(WalReaderSixCases, ACrcFailureWhileInsideIsATornTail) {
  Image img;
  img.FragmentFillingBlock(FragmentType::kFirst, 'x');
  {
    const std::string payload_8 = std::string(100, 'y');
    img.BadCrcFragment(FragmentType::kMiddle, Slice(payload_8));
  }
  const ScanResult r = ScanLog(img.slice());
  EXPECT_EQ(r.outcome, ScanOutcome::kTornTail);
  EXPECT_EQ(r.failure_offset, kBlockBytes) << "the failure point must be the "
                                              "fragment's own offset";
  EXPECT_NE(r.failure_reason.find("CRC"), std::string::npos);
}

// 3. FIRST, then a block of zeros, then EOF -- type 0 at the next fragment.
TEST(WalReaderSixCases, ZeroFillIsUnambiguouslyNotARecord) {
  Image img;
  img.FragmentFillingBlock(FragmentType::kFirst, 'x');
  img.Zeros(kBlockBytes);
  const ScanResult r = ScanLog(img.slice());
  EXPECT_EQ(r.outcome, ScanOutcome::kTornTail);
  EXPECT_NE(r.failure_reason.find("type is invalid"), std::string::npos)
      << "zeros were rejected for some reason other than the reserved type, so "
         "the two reservations that make section 5.4's false-positive analysis "
         "work are not both load-bearing";
}

// 4. FIRST, garbage block, then a VALID FULL with a higher sequence.
//    This cannot arise from prefix truncation, so step 1's premise is false and
//    truncation would be unsafe.
TEST(WalReaderSixCases, AValidRecordAfterGarbageIsInteriorCorruption) {
  Image img;
  img.FragmentFillingBlock(FragmentType::kFirst, 'x');
  img.Garbage(kBlockBytes);
  {
    const std::string payload_9 = Batch(7, "z", "9");
    img.Fragment(FragmentType::kFull, Slice(payload_9));
  }
  const ScanResult r = ScanLog(img.slice());
  ASSERT_EQ(r.outcome, ScanOutcome::kInteriorCorruption) << r.failure_reason;
  EXPECT_EQ(r.failure_offset, kBlockBytes);
  EXPECT_EQ(r.failure_block, 1u);
  EXPECT_EQ(r.resync_offset, 2 * kBlockBytes)
      << "resync must report WHERE the valid record was found; a refused open "
         "that cannot say where is one nobody can act on";
}

// 5. FIRST immediately followed by another FIRST, both CRC-valid.
//    No crash produces it; it is a writer bug or corruption that landed on a
//    fragment boundary. Classified directly, without resync.
TEST(WalReaderSixCases, FirstFollowedByFirstIsInteriorCorruption) {
  Image img;
  {
    const std::string payload_10 = std::string(50, 'x');
    img.Fragment(FragmentType::kFirst, Slice(payload_10));
  }
  {
    const std::string payload_11 = std::string(50, 'y');
    img.Fragment(FragmentType::kFirst, Slice(payload_11));
  }
  const ScanResult r = ScanLog(img.slice());
  EXPECT_EQ(r.outcome, ScanOutcome::kInteriorCorruption);
  EXPECT_EQ(r.failure_reason, "illegal fragment transition");
  EXPECT_EQ(r.failure_offset, kHeaderBytes + 50);
}

TEST(WalReaderSixCases, EveryIllegalTransitionIsRejected) {
  // OUTSIDE --MIDDLE--> and OUTSIDE --LAST-->
  for (FragmentType t : {FragmentType::kMiddle, FragmentType::kLast}) {
    Image img;
    const std::string payload(20, 'q');
    img.Fragment(t, Slice(payload));
    const ScanResult r = ScanLog(img.slice());
    EXPECT_EQ(r.outcome, ScanOutcome::kInteriorCorruption)
        << "a bare " << static_cast<int>(t) << " was accepted while OUTSIDE";
  }
  // INSIDE --FULL-->
  {
    Image img;
    {
      const std::string payload_12 = std::string(20, 'q');
      img.Fragment(FragmentType::kFirst, Slice(payload_12));
    }
    {
      const std::string payload_13 = Batch(1, "a", "1");
      img.Fragment(FragmentType::kFull, Slice(payload_13));
    }
    const ScanResult r = ScanLog(img.slice());
    EXPECT_EQ(r.outcome, ScanOutcome::kInteriorCorruption);
  }
}

// 6. A bare MIDDLE found during resync is NOT a candidate.
//    Accepting one would let garbage masquerade as interior corruption and
//    MANUFACTURE a refused open -- an availability bug produced by a safety
//    rule, which is the cost (d) has to keep paying and not exceed.
TEST(WalReaderSixCases, ABareMiddleIsNotAResyncCandidate) {
  Image img;
  img.FragmentFillingBlock(FragmentType::kFirst, 'x');
  img.Garbage(kBlockBytes);
  {
    const std::string payload_14 = std::string(50, 'm');
    img.Fragment(FragmentType::kMiddle, Slice(payload_14));
  }
  const ScanResult r = ScanLog(img.slice());
  EXPECT_EQ(r.outcome, ScanOutcome::kTornTail)
      << "a bare MIDDLE was accepted as a resync candidate, so garbage can now "
         "turn a normal torn tail into a refused open";
}

// A candidate must also carry a sequence ABOVE the last committed group's.
// Without that clause, a stale-looking record in a damaged block could reopen a
// log that recovery had already correctly terminated.
TEST(WalReaderSixCases, AResyncCandidateMustCarryAHigherSequence) {
  Image img;
  {
    const std::string payload_15 = Batch(9, "a", "1");
    img.Fragment(FragmentType::kFull, Slice(payload_15));
  }
  {
    const std::string payload_16 = GroupEnd(9, 1);
    img.Fragment(FragmentType::kFull, Slice(payload_16));
  }
  img.PadBlock();
  img.FragmentFillingBlock(FragmentType::kFirst, 'x');
  img.Garbage(kBlockBytes);
  const std::string stale = Batch(2, "old", "v");  // seq 2 < 9
  img.Fragment(FragmentType::kFull, Slice(stale));
  const ScanResult r = ScanLog(img.slice());
  EXPECT_EQ(r.outcome, ScanOutcome::kTornTail) << r.failure_reason;
  EXPECT_EQ(r.last_committed_seq, 9u);
}

// ------------------------------------------------- interior corruption, offsets

TEST(WalReader, AFlippedByteInsideACommittedGroupIsReportedWithAnOffset) {
  Image img;
  {
    const std::string payload_17 = BatchOfSize(1, img.PayloadToFillBlock());
    img.Fragment(FragmentType::kFull, Slice(payload_17));
  }
  {
    const std::string payload_18 = Batch(5, "b", "2");
    img.Fragment(FragmentType::kFull, Slice(payload_18));
  }
  img.FlipByteAt(kBlockBytes + kHeaderBytes + 2);  // inside the second payload
  const ScanResult r = ScanLog(img.slice());
  EXPECT_EQ(r.outcome, ScanOutcome::kTornTail)
      << "nothing valid follows, so this one IS a tail -- the point of the "
         "assertion below is that it was detected at a known offset";
  EXPECT_EQ(r.failure_offset, kBlockBytes);
  EXPECT_EQ(r.failure_block, 1u);
}

// Corrupting ONLY the length field must fail at the fragment's own offset.
TEST(WalReader, ACorruptLengthFailsAtAKnownOffset) {
  Image img;
  {
    const std::string payload_19 = Batch(1, "a", "1");
    img.Fragment(FragmentType::kFull, Slice(payload_19));
  }
  img.CorruptLengthAt(0);
  const ScanResult r = ScanLog(img.slice());
  EXPECT_NE(r.outcome, ScanOutcome::kCleanEnd);
  EXPECT_EQ(r.failure_offset, 0u)
      << "the failure point must be the fragment's own offset; resync has no "
         "sound starting point otherwise";
}

// ----------------------------------------------------------------- padding

TEST(WalReader, FewerThanEightBytesLeftInABlockIsPadding) {
  Image img;
  // Leave exactly 5 bytes of the block unused: fewer than the 8 a header plus
  // one payload byte needs, so the remainder is padding.
  const std::string filler = BatchOfSize(1, img.PayloadToFillBlock() - 5);
  img.Fragment(FragmentType::kFull, Slice(filler));
  img.Zeros(5);
  {
    const std::string payload_20 = Batch(1, "a", "1");
    img.Fragment(FragmentType::kFull, Slice(payload_20));
  }
  const ScanResult r = ScanLog(img.slice());
  EXPECT_EQ(r.outcome, ScanOutcome::kCleanEnd) << r.failure_reason;
  EXPECT_EQ(r.records.size(), 2u);
}

}  // namespace
}  // namespace wal
}  // namespace rift
