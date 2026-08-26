// The only sanctioned wording for what the TSan lane establishes.
//
// Ruled: do not let a future summarizer upgrade this to "race-free". So the
// claim lives in exactly ONE constant, is printed by the lane, and is pinned by
// a test -- which means STRENGTHENING THE SENTENCE REQUIRES FAILING A TEST, and
// the rule is that the harness must be strengthened in the same diff that
// strengthens the claim. A systematic interleaving search would earn a stronger
// sentence. Nothing else would.
//
// TSan is required regardless of the lock, because a locked structure with a
// WRONG lock is still a race. And a TSan lane over single-threaded tests is a
// green lane that proves nothing, which is why the lane runs a dedicated
// multi-threaded harness rather than the ordinary unit suite, and why BM14
// exists to prove this one is not that.
//
// WHY THIS SENTENCE IS NARROWER THAN DESIGN-B1 SECTION 6.4's. The document
// writes "(Apply/Get against Sync)". At B1.5 neither existed and the harness
// drove concurrent MemTable Add and Get, so the wording tracked that.
//
// B2 WIDENS IT, IN THE DIFF THAT WIDENS THE HARNESS, and never before it. The
// flush gave the engine its first operation that REPLACES the WAL and the
// memtable underneath a concurrent writer -- so a Write that reads one and
// applies to the other loses a record with no corruption anywhere. The second
// pattern drives exactly that: a writer against a Sync that flushes. It is
// still ONE authored pattern per clause, and still not a search.
// B3 DOES NOT WIDEN IT, AND THE REASON IS WORTH KEEPING.
//
// Compaction gave the engine a second appender to the manifest, so the obvious
// next pattern is two concurrent Syncs. It is not authored, because the harness
// note above already answers it: "one writer and one syncer ... NOT MORE,
// BECAUSE MORE WOULD BE A CLAIM THE CONTRACT DOES NOT MAKE."
//
// A pattern for two Syncs would test behaviour the contract does not promise
// and would quietly turn a precondition into a supported mode. So the
// precondition is ENFORCED instead -- `SingleCaller` in `DB::Sync` -- and the
// claim below stays exactly as wide as the harness that backs it.
#ifndef RIFT_CONCURRENCY_CLAIM_H_
#define RIFT_CONCURRENCY_CLAIM_H_

namespace rift {

inline constexpr char kConcurrencyClaim[] =
    "TSan observed no data race across two authored interleaving patterns "
    "(concurrent MemTable Add and Get; concurrent DB Write and Sync across "
    "a flush); this is not a proof of race-freedom.";

}  // namespace rift

#endif  // RIFT_CONCURRENCY_CLAIM_H_
