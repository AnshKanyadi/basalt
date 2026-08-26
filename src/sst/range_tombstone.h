// RANGE TOMBSTONES: the format, and the rules that refuse an illegal one.
//
// FOURTH USE OF B2-D6's ORDERING, AND THE STRONGEST. The classifier lands with
// no writer, no encoder and no compaction in the tree -- so it CANNOT have been
// written to agree with an implementation, because there is nothing to agree
// with. DESIGN-B3 section 6.1 fixed every rule below before this file existed,
// which makes them a SPECIFICATION the writer will be checked against rather
// than a description of what some writer happened to emit.
//
// `[A3]` requires real range tombstones at B3. B2's DeleteRange expands to one
// point delete per live key: correct, O(keys) per operation, and the reason
// kMaxRecordBytes is reachable by a legal call.
//
// ---------------------------------------------------------------------------
//   range-tombstone block = tombstones || restart_array || restart_count:u32
//                                      || crc32c:u32
//   tombstone = start_len:u32 || start_user_key
//            || end_len:u32   || end_user_key
//            || tag:u64
//
// THE BLOCK REUSES THE DATA BLOCK'S FRAMING EXACTLY, so `ParseBlock` decodes it
// and there is ONE block decoder in this engine rather than two that can drift.
// Section 7.5's one mechanism, two users, for the third time.
//
// THE TAG IS A FULL INTERNAL-KEY TAG, not a bare sequence: a range tombstone is
// a VERSION LIKE ANY OTHER, ordered against point versions by the same
// comparator, and its ValueType is the `kDeleteRange` reserved in the WAL's
// OpKind since B1 for exactly this.
//
// BOUNDS ARE [start, end), half-open, agreeing with engine.InRange by
// construction -- the same choice `Bound` made at B1 and for its reason: two
// range conventions in one engine is a bug waiting for a boundary key.
//
// THE BOUNDS ARE USER KEYS AND NOT INTERNAL KEYS, so the empty user key is a
// valid bound and there is no minimum length. This is the OPPOSITE of the point
// entry rule -- which refuses a key too short to carry a tag -- and it is stated
// here because a reader who has internalised that rule will assume it carries
// over. The tag is a separate field precisely so the bounds do not have to.
#ifndef RIFT_SST_RANGE_TOMBSTONE_H_
#define RIFT_SST_RANGE_TOMBSTONE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "internal_key.h"
#include "slice.h"

namespace rift {
namespace sst {

struct RangeTombstone {
  Slice start;      // user key, inclusive
  Slice end;        // user key, EXCLUSIVE
  uint64_t tag = 0; // (seq << 8) | kDeleteRange
  uint64_t offset = 0;  // from the block's start, for error reporting

  SeqNum seq() const { return SeqOfTag(tag); }
  // Half-open, and the ONE place this question is answered.
  bool Covers(Slice user_key) const {
    return user_key.compare(start) >= 0 && user_key.compare(end) < 0;
  }
};

// CLOSED. -Werror=switch, no default: arm.
enum class RangeFault : uint8_t {
  kNone,
  kMalformedBlock,
  kEmptyOrInvertedRange,
  kNotAscending,
  kDuplicate,
  kNotADeleteRangeTag,
};
const char* RangeFaultName(RangeFault fault);

struct RangeCheck {
  RangeFault fault = RangeFault::kNone;
  uint64_t offset = 0;
  std::string why;
  bool ok() const { return fault == RangeFault::kNone; }
  std::size_t count = 0;
};

// Builds a range-tombstone block. Exposed because the classifier's fixtures
// build blocks by hand, and a fixture using a different encoder than the reader
// validates would be testing the fixture.
void EncodeRangeTombstone(Slice start, Slice end, uint64_t tag, std::string* out);

// Parses AND CHECKS a range-tombstone block, from bytes alone. Pure: no Env, no
// engine, no writer. See DESIGN-B3 section 6.1 for what each refusal means.
RangeCheck ParseRangeBlock(Slice block, std::vector<RangeTombstone>* out);

}  // namespace sst
}  // namespace rift

#endif  // RIFT_SST_RANGE_TOMBSTONE_H_
