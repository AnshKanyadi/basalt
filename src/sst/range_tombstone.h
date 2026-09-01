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
//   end_len == 0xFFFFFFFF means THE RANGE HAS NO UPPER BOUND, and `end` then
//   occupies ZERO BYTES. See below: this is B3-Q4's ruling.
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
// ---------------------------------------------------------------------------
// B3-Q4, RULED: THE UNBOUNDED END IS IN THE BYTES, NOT IN A CONVENTION.
//
// The frozen `Engine` interface has `DeleteRange(Bound, Bound)` with either
// bound possibly UNBOUNDED, and `DeleteRange(Unbounded, Unbounded)` is section
// 8.2's clear-everything case -- the one Amendment `[A3]` was ruled for. THERE
// IS NO BYTE STRING GREATER THAN EVERY BYTE STRING, so `[start, infinity)` has
// no representation as a pair of keys.
//
// ONLY THE END IS AFFECTED. An unbounded START is expressible as `""`: the empty
// user key is the minimum, so `["", end)` and `[unbounded, end)` cover the same
// set. The problem is one-sided and so is the fix.
//
//   `end_len == kUnboundedEndLen` (0xFFFFFFFF) means no upper bound, and the
//   record then carries NO end bytes at all.
//
// WHY NOT A FLAG BIT IN THE TAG, which was the other candidate: section 6.1
// says a range tombstone is A VERSION LIKE ANY OTHER, ordered against point
// versions by the same comparator. A tag that stops being a plain internal-key
// tag surrenders a property that holds for EVERY ENTRY in this engine, in
// exchange for one bit. A property that holds for every entry is worth more
// than a byte.
//
// `end > start` REMAINS A RULE ABOUT FINITE ENDS. The sentinel is not an
// exception to it -- it is a DIFFERENT CASE, in which there is no end to
// compare. Written here because the next reader will meet the guard first and
// the sentinel second, and "exception" is the wrong thing to conclude.
//
// AND THE TWO WAYS OF SAYING UNBOUNDED MUST NOT BOTH EXIST: a record claiming
// the sentinel while carrying end bytes is REFUSED, so there is exactly one
// encoding of "no upper bound" and no second one to disagree with it.
//
// ---------------------------------------------------------------------------
// THE BOUNDS ARE USER KEYS AND NOT INTERNAL KEYS, so the empty user key is a
// valid bound and there is no minimum length. This is the OPPOSITE of the point
// entry rule -- which refuses a key too short to carry a tag -- and it is stated
// here because a reader who has internalised that rule will assume it carries
// over. The tag is a separate field precisely so the bounds do not have to.
#ifndef BASALT_SST_RANGE_TOMBSTONE_H_
#define BASALT_SST_RANGE_TOMBSTONE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "internal_key.h"
#include "basalt/slice.h"

namespace basalt {
namespace sst {

// The one encoding of "no upper bound". A length no real key can have: a key
// that long cannot fit in a block, and the block decoder bounds every length
// before reading it.
inline constexpr uint32_t kUnboundedEndLen = 0xFFFFFFFFu;

struct RangeTombstone {
  Slice start;      // user key, inclusive
  Slice end;        // user key, EXCLUSIVE; empty and unused when unbounded
  bool end_unbounded = false;
  uint64_t tag = 0; // (seq << 8) | kDeleteRange
  uint64_t offset = 0;  // from the block's start, for error reporting

  SeqNum seq() const { return SeqOfTag(tag); }
  // Half-open, and the ONE place this question is answered.
  bool Covers(Slice user_key) const {
    if (user_key.compare(start) < 0) return false;
    return end_unbounded || user_key.compare(end) < 0;
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
  kUnboundedEndWithBytes,
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

// The same, for a range with NO UPPER BOUND. A separate function rather than a
// sentinel argument, so a caller cannot reach the unbounded encoding by
// accident with an empty `end` -- which is a legal FINITE bound this format
// refuses for a different reason.
void EncodeUnboundedRangeTombstone(Slice start, uint64_t tag, std::string* out);

// Parses AND CHECKS a range-tombstone block, from bytes alone. Pure: no Env, no
// engine, no writer. See DESIGN-B3 section 6.1 for what each refusal means.
RangeCheck ParseRangeBlock(Slice block, std::vector<RangeTombstone>* out);

}  // namespace sst
}  // namespace basalt

#endif  // BASALT_SST_RANGE_TOMBSTONE_H_
