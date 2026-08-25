// THE WAL RECORD LAYOUT. B1-D3, APPROVED, AND THIS IS THE FROZEN SURFACE.
//
// Fixed-width little-endian, no varints, no reflection, NO TIMESTAMPS anywhere
// -- not in a header, not in a record, not in a filename (ruling 2: no
// serialized byte this engine ever sees carries a Mono instant).
//
// Little-endian rather than the Go wire codec's big-endian because the WAL is
// never compared byte-for-byte across implementations -- only engine STATE is
// -- and LE is a memcpy on both targets. A pinned byte-vector test freezes the
// encoding regardless, so the choice cannot drift silently.
//
// ---------------------------------------------------------------------------
// PHYSICAL FRAMING
//
//   block = 32768 bytes
//
//   fragment header = 7 bytes, little-endian
//       offset 0    crc32c   u32
//       offset 4    length   u16    payload bytes in THIS fragment; always >= 1
//       offset 6    type     u8     0 = invalid (reserved)
//                                   1 = FULL  2 = FIRST  3 = MIDDLE  4 = LAST
//
// If fewer than 8 bytes remain in a block, the remainder is EXPLICITLY
// ZERO-FILLED and the next fragment starts in the next block. Combined with
// `length >= 1` and `type 0 = invalid`, a run of zeros -- padding, or a hole
// past the written extent -- can never be mistaken for a record. Both
// reservations are load-bearing rather than tidy: section 5.4's false-positive
// analysis is what stops a normal torn tail from being reported as interior
// corruption, and it rests on exactly these two facts.
//
// ---------------------------------------------------------------------------
// DELIBERATE DEPARTURE FROM LEVELDB: THE CRC COVERS THE LENGTH.
// The full argument is the comment on FragmentCrc below, where section 5.3.3
// requires it to live -- at the code, not only in a document, because reverting
// it looks like a cleanup.
#ifndef RIFT_WAL_FORMAT_H_
#define RIFT_WAL_FORMAT_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "slice.h"

namespace rift {
namespace wal {

inline constexpr std::size_t kBlockBytes = 32768;
inline constexpr std::size_t kHeaderBytes = 7;

// A fragment header plus at least one payload byte needs 8. Fewer than that
// left in a block means the remainder is padding.
inline constexpr std::size_t kMinFragmentBytes = kHeaderBytes + 1;

enum class FragmentType : uint8_t {
  kInvalid = 0,  // RESERVED, never written. See the zero-fill argument above.
  kFull = 1,
  kFirst = 2,
  kMiddle = 3,
  kLast = 4,
};

enum class RecordKind : uint8_t {
  kInvalid = 0,  // RESERVED, for the same reason type 0 is.
  kBatch = 1,
  kGroupEnd = 2,
  kFileHeader = 3,
};

enum class OpKind : uint8_t {
  kSet = 0,
  kDelete = 1,
  // RESERVED AND NEVER WRITTEN BEFORE B3. Reserving the byte now is free; a
  // format version bump at B3 is not. Section 8.6: real range tombstones retire
  // the whole iterate-and-point-delete cost, and this byte is what lets B3
  // write one without touching the format version.
  kDeleteRange = 2,
};

inline constexpr char kMagic[8] = {'R', 'I', 'F', 'T', 'W', 'A', 'L', '\0'};
inline constexpr uint32_t kFormatVersion = 1;

using SeqNum = uint64_t;

struct Op {
  OpKind kind = OpKind::kSet;
  Slice key;
  Slice value;  // SET: the value. DELETE_RANGE: the end key. DELETE: empty.
};

// THE SIZE FORMULA, FROZEN. Section 5.3.4.
//
//   record_bytes(batch) = 1 + 8 + 4                    // kind, seq, op_count
//                       + sum over ops of 1 + 4 + |key|
//                                       + (SET:          4 + |value|)
//                                       + (DELETE_RANGE: 4 + |end|)
//
// THE CAP APPLIES TO THIS LOGICAL PAYLOAD, NOT TO THE FRAMED SIZE, so the
// harness's predicate is a sum over the ops it submitted and does not have to
// model fragmentation. That is a deliberate choice in favour of the harness
// being able to compute the quantity it adjudicates on.
//
// THE RIG REIMPLEMENTS THIS RATHER THAN CALLING IT, and that is the point:
// section 7.6 requires the harness to compute record_bytes from its OWN record
// of what it submitted, never from the engine. Two implementations of one
// frozen formula, with both divergence directions asserted (BM19, BM20) -- an
// oracle that called this function would be asking the engine whether the
// engine was right.
uint64_t BatchRecordBytes(const std::vector<Op>& ops);

// Encoders/decoders for the three logical record kinds.
void EncodeBatch(SeqNum seq, const std::vector<Op>& ops, std::string* out);
void EncodeGroupEnd(SeqNum high_seq, uint32_t batch_count, std::string* out);
void EncodeFileHeader(uint64_t file_number, std::string* out);

struct DecodedBatch {
  SeqNum seq = 0;
  std::vector<Op> ops;  // Slices into the payload; valid while it is
};
// Decodes a BATCH payload. Returns false on any malformation -- a payload that
// passed its checksum can still be structurally wrong if the writer was.
bool DecodeBatch(Slice payload, DecodedBatch* out);

struct DecodedGroupEnd {
  SeqNum high_seq = 0;
  uint32_t batch_count = 0;
};
bool DecodeGroupEnd(Slice payload, DecodedGroupEnd* out);

struct DecodedFileHeader {
  uint32_t format_version = 0;
  uint64_t file_number = 0;
};
bool DecodeFileHeader(Slice payload, DecodedFileHeader* out);

// Collapses a batch to AT MOST ONE OP PER KEY, last op winning, sorted by key.
//
// B1-D10(a), ruled. Under LevelDB's scheme the internal sequence advances per
// OP and engine.SeqNum is the batch's last internal sequence, so the C++
// engine's sequences would jump (1, 5, 9, ...) while engine/model's advance by
// one per Apply. That is contract-legal and still wrong: B4's rig would then
// need a per-engine map from operation index to sequence in order to sync both
// engines "to the same point", AND A RIG THAT NEEDS A TRANSLATION TABLE IS A
// RIG WITH A PLACE TO BE WRONG.
//
// Last-wins reproduces the model's rule: within one batch, a Set after a Delete
// re-adds the key. The sort is what B1-D10 costs, and it buys an assertable
// invariant -- no two memtable entries ever share a (user_key, seq) pair.
std::vector<Op> CollapseBatch(const std::vector<Op>& ops);

// Reads the kind and, for kBatch/kGroupEnd, the sequence, without decoding the
// rest. This is what section 5.4's resync predicate needs: a candidate must be
// kind BATCH or GROUP_END and carry a sequence above the last committed group's.
bool PeekKindAndSeq(Slice payload, RecordKind* kind, SeqNum* seq);

// THE FRAGMENT CHECKSUM, AND THE DELIBERATE DEPARTURE FROM LEVELDB.
//
// This comment is required here, on the helper, and not only in DESIGN-B1
// section 5.3.3 -- because of WHO IT IS AIMED AT.
//
// A deliberate divergence from a well-known upstream is not attacked. It is
// HELPFULLY CORRECTED. BM10 is the one mutation in this project's catalogue
// that a reviewer would most likely APPROVE: it introduces no bug, it removes
// two bytes of work per fragment, and it makes us match LevelDB, whose header
// is byte-identical to ours. A defence written against a defect would be
// pointed the wrong way; this one is pointed at a competent, well-meaning
// reader, and it has to be where they will be standing.
//
//   UPSTREAM (LevelDB log_format.h / log_writer.cc): the record header is
//   crc32c:u32 || length:u16 || type:u8, and the CRC is computed over
//   `type || data` ONLY. The length is NOT covered.
//
//   HERE: the CRC is computed over `length || type || payload`. The length IS
//   covered. Two extra bytes of CRC input per fragment.
//
//   WHY. With the length outside the CRC, a corrupted length field is not
//   itself detected: recovery reads a wrong-sized payload, the CRC then fails
//   FOR THE WRONG REASON, and the number of bytes consumed before the failure
//   is a function of data recovery has already decided not to trust. Section
//   5.4's discriminator is "does anything structurally valid follow the failure
//   point", and answering that requires the failure point to be a KNOWN OFFSET.
//   With the length covered, a corrupt length is a CRC failure at a known
//   offset, resync starts from the next block boundary, and the discrimination
//   is sound.
//
//   LevelDB can afford the weaker coverage because its reporter treats interior
//   corruption and a torn tail THE SAME WAY. We cannot, because section 5.4
//   rejects exactly that conflation -- (b), "every checksum failure is
//   end-of-log", is the candidate that silently discards promised data.
//
//   REVERTING THIS TO UPSTREAM'S COVERAGE SILENTLY WEAKENS THE TORN-TAIL RULE.
//   BM10 is the mutant that blinds it, and the property it breaks is directly
//   assertable: two fragments with the same type and payload but different
//   LENGTH fields must have different checksums. Under upstream's coverage they
//   would have the same one.
uint32_t FragmentCrc(uint16_t length, FragmentType type, Slice payload);

}  // namespace wal
}  // namespace rift

#endif  // RIFT_WAL_FORMAT_H_
