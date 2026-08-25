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
// writes "(Apply/Get against Sync)", which is what the harness will drive once
// Apply and Sync exist -- B1.6 and B1.8. At B1.5 they do not, and the harness
// drives concurrent MemTable Add and Get. Using the document's wording today
// would be a claim that outruns its subject, which is the exact thing this
// constant exists to prevent, so the wording tracks the harness and widens in
// the same diff that widens the harness. Never before it.
#ifndef RIFT_CONCURRENCY_CLAIM_H_
#define RIFT_CONCURRENCY_CLAIM_H_

namespace rift {

inline constexpr char kConcurrencyClaim[] =
    "TSan observed no data race across one authored interleaving pattern "
    "(concurrent MemTable Add and Get); this is not a proof of race-freedom.";

}  // namespace rift

#endif  // RIFT_CONCURRENCY_CLAIM_H_
