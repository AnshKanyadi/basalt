// THE BLOOM FILTER. B2-D3, approved.
//
// One filter per table, over USER KEYS -- not internal keys. A point lookup
// asks "could this user key be in this table", and the answer must not depend
// on the sequence number the caller happens to be reading at, or the filter
// would be useless for exactly the lookups it exists to skip.
//
// ---------------------------------------------------------------------------
// NO FLOATING POINT APPEARS ANYWHERE ON THIS PATH.
//
// The textbook probe count is `ln 2 x bits_per_key`, and that is a FLOAT
// DECIDING ON-DISK BYTES. Ruling 5 forbids it, so the arithmetic is done ONCE,
// BY HAND, HERE, and the result is a pinned integer:
//
//     bits_per_key = 10
//     optimal k    = ln 2 x 10 = 6.93...
//     kProbes      = 7          (pinned; the rounding is done here, not at run time)
//
// The predicted false-positive rate at 10 bits/key and k=7 is about 0.82%. That
// number is a PREDICTION and it is not asserted -- see the two properties below.
//
// ---------------------------------------------------------------------------
// THE TWO PROPERTIES ARE NOT THE SAME KIND OF CLAIM.
//
//   NO FALSE NEGATIVES is SAFETY. A key in the table must always probe present.
//   It is asserted EXACTLY, over the whole key set, and a single counterexample
//   is a violation. Everything above this layer is entitled to skip a table the
//   filter denies, so a false negative is a LOST KEY -- silent, and shaped like
//   data loss rather than corruption.
//
//   THE FALSE-POSITIVE RATE is PERFORMANCE. It is asserted as a CEILING,
//   measured under section 10.3's shape. Pinning an exact rate would assert
//   noise: any change to the workload's key shapes moves it, and a lane that
//   reddens on a key-shape change is a lane that gets tuned rather than read.
//
// ---------------------------------------------------------------------------
// THE HASH IS Fnv1a64, already in the tree and already pinned by golden
// vectors. Two 32-bit halves drive double hashing:
//
//     probe_i = (h1 + i*h2) mod bits
//
// No PRNG (ruling 5) and NO PER-DB SALT. Section 6.2's ruling on tower heights
// applies unchanged, including its accepted cost: the function is public, so a
// key set that defeats the filter can be constructed. That is a PERFORMANCE
// property and not a safety one -- a defeated filter returns "maybe" more often
// and never returns a false negative -- and the declined fix is the same
// per-DB salt, declined for the same reason: it makes a file's meaning depend
// on state outside the file.
//
// ---------------------------------------------------------------------------
//   filter block = bits[nbytes] || probes:u32 || nbytes:u32 || crc32c:u32
//
// The trailer mirrors a data block's -- count-then-checksum, same crc32c, same
// four-byte trailer -- so "every block in this file ends with its own
// checksum" is a rule with no exceptions rather than a rule with one.
//
// `probes` is STORED rather than assumed. A reader uses the file's value, not
// its own constant, so a future build that pins a different k produces files
// this one still reads correctly. `nbytes` is stored for the same reason the
// WAL's length is inside its CRC: a length that must be inferred from the
// block's size is a length nothing can disagree with, and disagreement is the
// only way a truncated filter announces itself.
#ifndef BASALT_SST_BLOOM_H_
#define BASALT_SST_BLOOM_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "basalt/slice.h"

namespace basalt {
namespace sst {

inline constexpr uint32_t kBitsPerKey = 10;
inline constexpr uint32_t kProbes = 7;  // see the header: ln2 x 10 = 6.93, pinned
inline constexpr std::size_t kFilterTrailerBytes = 12;  // probes + nbytes + crc32c

// A tiny filter is mostly trailer and its rate is dominated by rounding, so a
// floor keeps a one-key table from carrying a filter that answers "maybe" to
// everything. 64 bits is the smallest size at which the probe pattern spreads.
inline constexpr std::size_t kMinFilterBytes = 8;

// A ceiling on a count that comes off disk and drives a loop. Not a policy: the
// point past which no honest writer could have produced the number.
inline constexpr uint32_t kMaxProbes = 64;

// ONE PROBE SEQUENCE, TWO USERS -- the builder that SETS the bits and the
// reader that TESTS them. If those two computed the sequence separately they
// could drift, and THE SYMPTOM OF DRIFT IS A FALSE NEGATIVE: the one thing this
// filter may not produce, and the one thing nothing downstream would report,
// because a table skipped is a key that was simply never there. Section 7.5's
// one-mechanism rule, applied to arithmetic instead of to an injector.
//
// IT IS PUBLIC BECAUSE THE PROPERTY BELOW HAS TO BE ASSERTABLE, not because
// anything outside this file needs to walk probes. `step() != 0` is invisible
// at every level above this one: a zero step makes all k probes hit one bit,
// which turns a 7-probe filter into a 1-probe filter WITHOUT producing a single
// false negative. Only the rate moves, and the rate is the thing this file
// deliberately refuses to assert exactly -- so the guard is asserted here, on
// the arithmetic, or it is not asserted at all.
class ProbeWalk {
 public:
  ProbeWalk(uint64_t hash, uint64_t bits);

  // The next bit position. Wraps within [0, bits).
  uint64_t Next();

  // Never zero, for any hash. That is the whole reason this class is public.
  uint64_t step() const { return step_; }

 private:
  uint64_t bits_;
  uint64_t pos_;
  uint64_t step_;
};

// The filter's hash of a user key. Named so the fixtures that hand-build filter
// blocks -- a block written with a different probe count, say -- use the SAME
// function the builder does rather than a copy of it.
uint64_t BloomHash(Slice user_key);

// Accumulates hashes, not keys: the builder holds one u64 per key and never the
// key bytes, so a flush's filter costs 8 bytes per entry and not the key set
// twice.
class FilterBuilder {
 public:
  // `user_key`, NOT an internal key. Callers holding internal keys strip the
  // 8-byte tag first; doing it here would make the builder know the key
  // encoding, and section 6.2's comparator argument applies -- the engine does
  // not learn what a key MEANS.
  void AddUserKey(Slice user_key);
  std::size_t keys() const { return hashes_.size(); }

  // Returns the filter block. ORDER-INDEPENDENT: bits are OR'ed, so the same
  // key set in any insertion order produces byte-identical output. Asserted,
  // because an order-dependent filter would make the byte digest of a flush
  // depend on skiplist traversal order.
  std::string Finish() const;

 private:
  std::vector<uint64_t> hashes_;
};

// A parsed filter block. Holds a Slice into the caller's bytes and copies
// nothing: the table image outlives every reader of it.
class FilterReader {
 public:
  // Validates the trailer, the checksum and the declared length, and reports
  // WHY on refusal. Returns false rather than aborting: a corrupt filter is a
  // damaged FILE, and the caller decides whether that voids a run.
  static bool Parse(Slice block, FilterReader* out, std::string* why);

  // "Maybe" or "definitely not". Never "definitely yes".
  bool MayContain(Slice user_key) const;

  uint32_t probes() const { return probes_; }
  std::size_t bits() const { return bits_.size() * 8; }

 private:
  Slice bits_;
  uint32_t probes_ = 0;
};

}  // namespace sst
}  // namespace basalt

#endif  // BASALT_SST_BLOOM_H_
