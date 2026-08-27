// THE INTERNAL KEY: one encoder, and one comparator, for every user of it.
//
// B1 defined this layout in the memtable and nowhere else, because the memtable
// was the only thing that held keys. B2 gives it a second holder -- the SSTable
// -- and a second holder is exactly the moment a layout defined in one file's
// private helpers becomes two layouts that agree until they do not. Section
// 7.5's one-mechanism rule; the same argument that put the WAL's fragment
// encoding in a shared header.
//
//   internal_key = user_key || tag,  tag = ((seq << 8) | value_type) as u64 LE
//
// ---------------------------------------------------------------------------
// THE ORDER IS NOT MEMCMP, AND THIS IS THE TRAP.
//
//   user key ASCENDING by memcmp, then tag DESCENDING
//
// so the NEWEST version of a key sorts FIRST and a snapshot read is one seek.
// The tag is stored little-endian, so a bytewise comparison of two versions of
// the same user key orders them by ASCENDING tag -- the exact opposite of the
// required order, and it is a plausible-looking wrongness: every key is still
// unique, every block still parses, and a table validated with memcmp looks
// perfectly well formed while a reader seeking a snapshot finds the OLDEST
// visible version instead of the newest.
//
// THIS FILE EXISTS BECAUSE THAT DEFECT WAS WRITTEN. The B2.0 classifier was
// drafted comparing raw bytes, and the fixtures that would have caught it did
// not exist yet because they used single-letter keys with no tag at all. It was
// found by building the fixtures the format actually requires -- which is the
// entire argument for landing the classifier before the writer, arriving one
// step earlier than expected.
#ifndef RIFT_INTERNAL_KEY_H_
#define RIFT_INTERNAL_KEY_H_

#include <cstdint>
#include <string>

#include "slice.h"

namespace rift {

// LevelDB's convention, and it is load-bearing for ordering: a deletion and a
// value at the same sequence must sort deterministically, and the type is the
// low byte of the tag so they do.
enum class ValueType : uint8_t {
  kDeletion = 0,
  kValue = 1,
};

using SeqNum = uint64_t;

inline constexpr std::size_t kTagBytes = 8;

// The sequence occupies the upper 56 bits, so this is the largest one that
// survives the shift. A caller that exceeds it has run the database for 2^56
// writes, and the check is here so that if it ever happens it is an abort at
// the point of the mistake rather than a key that sorts somewhere surprising.
inline constexpr SeqNum kMaxSeqNum = (static_cast<SeqNum>(1) << 56) - 1;

uint64_t MakeTag(SeqNum seq, ValueType type);
inline SeqNum SeqOfTag(uint64_t tag) { return tag >> 8; }
inline ValueType TypeOfTag(uint64_t tag) {
  return static_cast<ValueType>(tag & 0xff);
}

void AppendInternalKey(std::string* out, Slice user_key, uint64_t tag);

// Both RIFT_CHECK that the key is at least kTagBytes long. A key that is not is
// a bug in this process, not a damaged file: code reading a FILE must check the
// length itself and report, which is what the table classifier does.
Slice ExtractUserKey(Slice internal_key);
uint64_t ExtractTag(Slice internal_key);

// User key ascending, tag descending. THE ONLY ORDER THIS ENGINE HAS.
int CompareInternalKey(Slice a, Slice b);

// A TABLE'S BOUNDS ARE STATEMENTS ABOUT USER KEYS, AND THIS IS THE ONE PLACE
// THAT DECIDES WHETHER A CANDIDATE WIDENS ONE.
//
// DESIGN-B3 §6.1a: *"The bound is a statement about USER KEYS. It is compared
// as one."* `CompareInternalKey` is the wrong comparator for the question,
// because the internal order is user key ascending and TAG DESCENDING -- so at
// one user key a SMALLER TAG SORTS LATER, and a candidate equal in user key can
// compare "greater" or "lesser" purely by its sequence.
//
// BUG-006 was that mismatch across two implementations of this rule. It exists
// as a function so there is ONE implementation and nothing to keep in step:
// `TableBuilder` and `ValidateTable` both call it, so they cannot disagree.
//
// TIES DO NOT WIDEN. Equal user keys mean the bound already covers the
// candidate, and a rule that widened on equality would make the recorded bound
// depend on which sequence happened to arrive -- which is precisely how the two
// sides diverged.
inline bool WidensUpperBound(Slice candidate_user_key, Slice current_bound) {
  if (current_bound.size() < kTagBytes) return true;  // no bound yet
  return candidate_user_key.compare(ExtractUserKey(current_bound)) > 0;
}
inline bool WidensLowerBound(Slice candidate_user_key, Slice current_bound) {
  if (current_bound.size() < kTagBytes) return true;
  return candidate_user_key.compare(ExtractUserKey(current_bound)) < 0;
}

}  // namespace rift

#endif  // RIFT_INTERNAL_KEY_H_
