// THE FIRST THING THIS ENGINE DOES THAT DELETES DATA THE SYSTEM PREVIOUSLY HELD.
//
// Everything before B3 only ever added: a WAL appends, a flush copies a memtable
// into a table, a manifest records. Compaction is the first operation whose
// CORRECTNESS CONDITION IS ABOUT WHAT IS NO LONGER THERE -- so a checker that
// looks at the surviving state cannot see the bug, because a wrongly dropped
// version leaves a state that is internally consistent and simply missing
// something. That is B3-D1's whole reason for existing, and it is why the drop
// adjudicator was written before this file.
//
// ---------------------------------------------------------------------------
// B3-D1, THE DROP CLAIM, AS IMPLEMENTED HERE.
//
// For a user key `k` with versions ordered by sequence DESCENDING, an entry `e`
// may be dropped if and only if BOTH hold:
//
//   1. `e` is not in `keep(k)` -- where keep(k) is, for each observable
//      sequence `s`, the newest version of k with seq <= s, IF THAT VERSION IS A
//      VALUE. A deletion is never required.
//   2. if `e` is a DELETION, no version of k with a smaller sequence survives
//      anywhere.
//
// ---------------------------------------------------------------------------
// CLAUSE 2 IS WHY INPUT SELECTION IS A CORRECTNESS CONCERN AND NOT A POLICY ONE.
//
// A deletion dropped while an older value survives IN A FILE THIS COMPACTION DID
// NOT READ RESURRECTS DELETED DATA. So `bottom_most` is not a hint: it is the
// caller's assertion that the inputs hold EVERY version of every key they
// contain, and a caller that cannot assert it must pass false, in which case no
// deletion is dropped at all.
//
// ---------------------------------------------------------------------------
// WHAT THIS COMPUTES FROM THE INPUTS IS AN OVER-APPROXIMATION OF keep(k), AND
// THAT DIRECTION IS THE SAFE ONE.
//
// keep(k) is defined over every version of k IN THE DATABASE, and the memtables
// hold versions this merge never sees. Judging from the inputs alone can only
// make a version look MORE required than it is -- a newer version outside the
// inputs would have shadowed it. Over-keeping is permitted by the claim; under-
// keeping is the violation. The one place the direction reverses is clause 2,
// which is about OLDER versions, and that is exactly what `bottom_most` guards.
#ifndef RIFT_COMPACTION_H_
#define RIFT_COMPACTION_H_

#include <cstdint>
#include <vector>

#include "internal_key.h"
#include "merged_iter.h"
#include "status.h"
#include "table_builder.h"

namespace rift {

// WHERE A COMPACTION'S SURVIVORS GO.
//
// It is an interface rather than a `TableBuilder*` because B3-D3(b) says L1 is a
// RUN, and a run of one file is candidate (a) wearing (b)'s name: every
// compaction would rewrite the whole database, which is the write amplification
// (b) exists to bound and the reason (a) was rejected.
//
// `boundary` is true when this entry begins a NEW USER KEY. An implementation
// that rolls to a new file may roll ONLY THERE: two files of one run that share
// a user key are not a run, `L1FileFor` would find one of them, and the other's
// versions would be unreachable -- a deletion that stops hiding a value.
class CompactionSink {
 public:
  virtual ~CompactionSink() = default;
  virtual Status Add(Slice internal_key, Slice value, bool boundary) = 0;
};

struct CompactionStats {
  // B3-D7a'S PROGRESS QUANTITY. One per iteration, whatever the iteration
  // decides -- emit, drop, or skip -- which is why it and not a cursor is the
  // honest measure. See the loop.
  uint64_t inputs_consumed = 0;
  uint64_t emitted = 0;
  uint64_t dropped = 0;
  // 1 if the watermark pin below forced an entry to be kept that the drop claim
  // would have permitted dropping. Asserted by a test, per section 8.4.
  uint64_t pinned = 0;
};

// `observable` is `S`: every live snapshot sequence plus the current visible
// sequence, ASCENDING and distinct.
//
// `bound` is B3-D7a's DERIVED bound: the sum of the input tables' entry counts
// as `ValidateTable` counted them. GF-13 -- it is derived from another
// instrument's measurement, so it cannot be raised without contradicting that
// instrument. There is no number here to tune.
//
// `out` may finish with NO ENTRIES; the caller must handle that, because a
// compaction that drops everything is the correct outcome for a key written and
// deleted with no snapshot below it, and `TableBuilder::Finish` refuses an empty
// table on purpose.
//
// ---------------------------------------------------------------------------
// `pin_seq` -- THE HIGHEST SEQUENCE HELD BY ANY INPUT, AND THE OUTPUT MUST
// CARRY IT. This is an obligation the drop claim does not state, found before
// this loop ran, and the reason is that the two speak about different things:
//
//   THE DROP CLAIM IS ABOUT THE ANSWER A READER GETS. THE WATERMARK IS A
//   PROMISE ABOUT A SEQUENCE. A compaction can preserve every answer exactly
//   and still destroy the engine's only proof of a promise it already made.
//
// Concretely: `Open` recomputes the durable floor as the maximum `largest_seq`
// over the live tables, and `largest_seq` is re-derived from each table's own
// bytes -- the manifest may not record a durable sequence (D7's forward
// binding), so there is nowhere else for that number to come from. If the
// highest-sequenced entry in the database is a tombstone the claim permits
// dropping, dropping it lowers that maximum, and `DurableSeq` GOES BACKWARDS
// ACROSS A RESTART. The frozen contract says monotone non-decreasing.
//
// So exactly one entry at `pin_seq` is kept when the rules would drop them all.
// It is over-keeping, which the claim permits -- "may drop", never "must" -- and
// it costs at most one entry per compaction.
//
// `out` may receive NO ENTRIES at all: a compaction that drops everything is
// the correct outcome for a key written and deleted with no snapshot below it.
Status RunCompaction(MergedIter* input, const std::vector<SeqNum>& observable,
                     bool bottom_most, SeqNum pin_seq, uint64_t bound,
                     CompactionSink* out, CompactionStats* stats);

}  // namespace rift

#endif  // RIFT_COMPACTION_H_
