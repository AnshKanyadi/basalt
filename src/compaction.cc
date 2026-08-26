#include "compaction.h"

#include <algorithm>
#include <string>

#include "check.h"

namespace rift {
namespace {

// Is some observable sequence in [lo, hi)? That interval is exactly the set of
// sequences at which a given version is THE NEWEST VERSION OF ITS KEY: `lo` is
// its own sequence, `hi` is the sequence of the next-newer version (or one past
// the end of the sequence space, for the newest).
bool AnyObservableIn(const std::vector<SeqNum>& s, SeqNum lo, SeqNum hi) {
  const auto it = std::lower_bound(s.begin(), s.end(), lo);
  return it != s.end() && *it < hi;
}

// Is some observable sequence strictly below `q`? If none is, then no version
// older than `q` can be the newest at any observable sequence -- so nothing
// older is in keep(k), and clause 2 is satisfied without looking at the older
// versions at all.
bool AnyObservableBelow(const std::vector<SeqNum>& s, SeqNum q) {
  return !s.empty() && s.front() < q;
}

}  // namespace

Status RunCompaction(MergedIter* input, const std::vector<SeqNum>& observable,
                     bool bottom_most, SeqNum pin_seq, uint64_t bound,
                     CompactionSink* out, CompactionStats* stats) {
  // ASCENDING AND DISTINCT is a precondition, asserted rather than sorted for:
  // a caller that hands over an unsorted S has a bug in how it collected live
  // snapshots, and quietly sorting it here would hide that.
  for (std::size_t i = 1; i < observable.size(); ++i) {
    RIFT_CHECK(observable[i - 1] < observable[i]);
  }

  *stats = CompactionStats();
  std::string current_key;
  bool have_key = false;
  // The sequence of the previous (NEWER) version of `current_key`. For the
  // first version of a key there is no newer one, so the interval it is newest
  // over runs to the top of the sequence space.
  SeqNum newer_seq = kMaxSeqNum + 1;
  std::string ikey;
  bool pin_satisfied = false;

  for (input->SeekToFirst(); input->Valid(); input->Next()) {
    // B3-D7a'S PROGRESS ASSERTION, AND IT IS A TERMINATION ASSERTION ONLY.
    //
    // GF-12: it says NOTHING about whether the output is right. What covers
    // that is `AdjudicateMerge` (order and values) and `AdjudicateDrops` (what
    // survived) -- two instruments, named before this loop was written.
    //
    // `inputs_consumed` rises by exactly one per iteration whatever the
    // iteration decides, which is why it and not a key or a cursor is the
    // honest quantity: a correct merge neither advances the output key on a
    // dropped entry nor advances a single input cursor monotonically.
    //
    // A correct compaction consumes each input entry EXACTLY ONCE, so it
    // terminates AT the bound. Reaching it is not a failure; EXCEEDING it can
    // only mean a source was rewound or an entry was counted twice, which are
    // the two ways a k-way merge fails to stop.
    ++stats->inputs_consumed;
    RIFT_CHECK(stats->inputs_consumed <= bound);

    const Slice user_key = input->user_key();
    const uint64_t tag = input->tag();
    const SeqNum seq = SeqOfTag(tag);
    const bool deletion = TypeOfTag(tag) == ValueType::kDeletion;

    if (!have_key || Slice(current_key) != user_key) {
      current_key = user_key.ToString();
      have_key = true;
      newer_seq = kMaxSeqNum + 1;
    }

    bool keep = false;
    if (!deletion) {
      // CLAUSE 1. The version is required exactly when some observable
      // sequence sees it -- that is, lands in the interval over which it is the
      // newest version of its key.
      keep = AnyObservableIn(observable, seq, newer_seq);
    } else {
      // CLAUSE 2, AND IT IS DELIBERATELY CONSERVATIVE. The tombstone drops when
      // no observable sequence is below it, because then nothing older is in
      // keep(k) and nothing it masked can survive. When one IS below it, the
      // tombstone is kept even in the cases where every older version happens
      // to be a deletion too -- the claim says a compaction MAY drop, never
      // that it must, and over-keeping is not a violation.
      //
      // Without `bottom_most` the tombstone is always kept: an older value in a
      // file this compaction did not read would come back from the dead.
      keep = !bottom_most || AnyObservableBelow(observable, seq);
    }

    // THE WATERMARK PIN. See compaction.h: the output must carry `pin_seq` or
    // the durable floor drops across a restart. One entry is enough, because
    // `largest_seq` is a MAXIMUM over entries -- so this fires at most once and
    // only when the rules above would have dropped every entry that holds it.
    if (seq == pin_seq) {
      if (!keep && !pin_satisfied) {
        keep = true;
        ++stats->pinned;
      }
      if (keep) pin_satisfied = true;
    }

    if (keep) {
      ikey.clear();
      AppendInternalKey(&ikey, user_key, tag);
      // THE BOUNDARY IS THE KEY CHANGE, NOT THE EMIT. `newer_seq` is still the
      // previous entry's sequence at this point only when this entry belongs to
      // the same user key, so the flag is computed from the key transition the
      // loop already found -- and a sink that rolls files anywhere else would
      // split a user key across two files of one run.
      const bool boundary = newer_seq == kMaxSeqNum + 1;
      const Status s = out->Add(Slice(ikey), input->value(), boundary);
      if (!s.ok()) return s;
      ++stats->emitted;
    } else {
      ++stats->dropped;
    }
    newer_seq = seq;
  }
  return Status::Ok();
}

}  // namespace rift
