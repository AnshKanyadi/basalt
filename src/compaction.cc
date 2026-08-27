#include "compaction.h"

#include <algorithm>
#include <string>

#include "check.h"
#include "range_tombstone.h"

namespace rift {
namespace {

// [lo, hi) is exactly the set of sequences at which a given version is THE
// NEWEST VERSION OF ITS KEY: `lo` is its own sequence, `hi` is the sequence of
// the next-newer version (or one past the end of the sequence space, for the
// newest). Clause 1 walks the observable sequences inside it directly, because
// the tombstone test has to be made AT EACH ONE -- see the loop.

// Is some observable sequence strictly below `q`? If none is, then no version
// older than `q` can be the newest at any observable sequence -- so nothing
// older is in keep(k), and clause 2 is satisfied without looking at the older
// versions at all.
bool AnyObservableBelow(const std::vector<SeqNum>& s, SeqNum q) {
  return !s.empty() && s.front() < q;
}

}  // namespace

namespace {

bool CoversKey(const CompactionTombstone& t, Slice user_key) {
  // ONE PREDICATE, and it is `sst::RangeTombstone::Covers` -- the engine has
  // exactly one answer to "does this range contain this key", shared by the
  // memtable, the table and the compaction.
  sst::RangeTombstone v;
  v.start = Slice(t.start);
  v.end = Slice(t.end);
  v.end_unbounded = t.end_unbounded;
  return v.Covers(user_key);
}

// The newest tombstone at or below `at` covering `user_key`, or 0.
SeqNum NewestCoveringTombstone(const std::vector<CompactionTombstone>& ts,
                               Slice user_key, SeqNum at) {
  SeqNum best = 0;
  for (const CompactionTombstone& t : ts) {
    if (t.seq > at || t.seq <= best) continue;
    if (CoversKey(t, user_key)) best = t.seq;
  }
  return best;
}

}  // namespace

Status RunCompaction(MergedIter* input, const std::vector<SeqNum>& observable,
                     bool bottom_most, SeqNum pin_seq, uint64_t bound,
                     const std::vector<CompactionTombstone>& tombstones,
                     CompactionSink* out, CompactionStats* stats) {
  // ASCENDING AND DISTINCT is a precondition, asserted rather than sorted for:
  // a caller that hands over an unsorted S has a bug in how it collected live
  // snapshots, and quietly sorting it here would hide that.
  for (std::size_t i = 1; i < observable.size(); ++i) {
    RIFT_CHECK(observable[i - 1] < observable[i]);
  }

  *stats = CompactionStats();

  // THE TOMBSTONE VERDICT COMES FIRST, AND THE HEADER SAYS SO: "handed over
  // BEFORE the first entry". The sink closes output files DURING the merge, and
  // a file closed before it has been told the tombstones writes none.
  //
  // Nothing in the verdict depends on the merge -- clause 2 over a range asks
  // only about `observable` and `bottom_most` -- so computing it here is not a
  // reordering for convenience. It is where it belongs, and the first version
  // put it at the end where the interface's own words said it must not be.
  //
  // A tombstone may be dropped when NOTHING OBSERVABLE IS BELOW IT -- then no
  // reader can ask what the key held before it, so it has no work left -- and
  // when the inputs are BOTTOM-MOST, because an older value in a file this
  // compaction did not read would come back. The same pair of conditions a
  // point deletion answers.
  //
  // The watermark pin cannot conflict with it: the pinned entry sits at
  // `pin_seq`, the maximum sequence the inputs hold, which is at or above every
  // tombstone in them -- so a dropped tombstone never had it to hide.
  {
    std::vector<CompactionTombstone> survivors;
    for (const CompactionTombstone& t : tombstones) {
      if (bottom_most && !AnyObservableBelow(observable, t.seq)) {
        ++stats->tombstones_dropped;
        continue;
      }
      survivors.push_back(t);
      ++stats->tombstones_kept;
    }
    out->SetTombstones(std::move(survivors));
  }

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
      // CLAUSE 1, WITH RANGE TOMBSTONES: the version is required exactly when
      // SOME OBSERVABLE SEQUENCE RETURNS IT -- which needs both halves at the
      // SAME `s`. It must be the newest version at `s` (the interval test), and
      // no tombstone visible AT THAT `s` may hide it.
      //
      // DO NOT HOIST THE TOMBSTONE CALL OUT OF THIS LOOP. It looks independent
      // of `s` and is not: `s` is THE SEQUENCE AT WHICH THE TOMBSTONE MUST BE
      // VISIBLE. `BM105` is that exact edit, preserved as a class.
      //
      // THE PER-SEQUENCE TEST IS THE WHOLE POINT, AND THE FIRST IMPLEMENTATION
      // GOT IT WRONG. It asked whether a tombstone covered the key at the TOP
      // of the interval, which conflates two different sequences: a snapshot at
      // 5 and a tombstone at 9 are both "in the interval" of a version at 4,
      // and the tombstone is INVISIBLE to the snapshot. The version is the
      // answer at 5 and was being dropped -- data loss for a snapshot below the
      // tombstone, masked only by the snapshot holding the pre-compaction
      // tables resident.
      //
      // CONSULTING TOMBSTONES AT ALL IS NOT AN OPTIMISATION. Without it the
      // merge could KEEP a version a tombstone hides while DROPPING the
      // tombstone -- clause 2 drops a tombstone when nothing observable is
      // below it, and that is exactly when every version beneath it is
      // unobservable too. Keeping one and dropping the other resurrects the
      // value. So the rule must be neither too strict nor too loose, and it is
      // stated per observable sequence because that is what the claim says.
      keep = false;
      for (SeqNum s : observable) {
        if (s < seq) continue;
        if (s >= newer_seq) break;          // ascending, so nothing later fits
        if (NewestCoveringTombstone(tombstones, user_key, s) > seq) continue;
        keep = true;
        break;
      }
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
