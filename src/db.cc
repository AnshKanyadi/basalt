#include "db.h"

#include <set>

#include <atomic>
#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "check.h"
#include "compaction.h"
#include "env_guard.h"
#include "manifest.h"
#include "merged_iter.h"
#include "recovery.h"
#include "single_caller.h"
#include "table.h"
#include "table_builder.h"
#include "wal.h"

namespace rift {

bool InRange(Slice key, const Bound& start, const Bound& end) {
  if (start.bounded() && key.compare(start.key()) < 0) return false;
  if (end.bounded() && key.compare(end.key()) >= 0) return false;
  return true;
}

WriteBatch& WriteBatch::Set(Slice key, Slice value) {
  Entry e;
  e.kind = wal::OpKind::kSet;
  e.key = key.ToString();
  e.value = value.ToString();
  ops_.push_back(std::move(e));
  return *this;
}
WriteBatch& WriteBatch::Delete(Slice key) {
  Entry e;
  e.kind = wal::OpKind::kDelete;
  e.key = key.ToString();
  ops_.push_back(std::move(e));
  return *this;
}
WriteBatch& WriteBatch::DeleteRange(const Bound& start, const Bound& end) {
  Entry e;
  e.kind = wal::OpKind::kDeleteRange;
  e.key = start.bounded() ? start.key().ToString() : std::string();
  e.end = end;
  // A DeleteRange's START is itself a bound; the entry keeps it in `end`'s
  // sibling field only because the frozen Op does. Unboundedness of the start
  // is carried by this flag rather than by an empty key, for divergence 3's
  // reason.
  e.value = start.bounded() ? "1" : "";
  ops_.push_back(std::move(e));
  return *this;
}

namespace {

// WHAT A READ SEES, AND WHAT KEEPS IT ALIVE WHILE IT LOOKS.
//
// THIS IS memtable.h's EXPIRING NOTE, DISCHARGED. B1's iterators returned
// pointers into an arena, "safe here for a reason that expires: B1 has no
// flush, so nodes are never freed and the arena outlives every iterator. B2
// must revisit this the moment a memtable can be retired." B2 retires
// memtables. An iterator holding a raw pointer into one would read freed arena
// the moment a flush completed, so a read captures SHARED POINTERS under the
// lock and holds them for its whole life. Refcounts, not lifetime by argument.
// Forward-declared because `Version::NewestCovering` needs it and it needs
// `Version`'s member type.
const sst::Table* L1FileFor(const std::vector<std::shared_ptr<sst::Table>>& l1,
                            Slice key);

struct Version {
  std::shared_ptr<MemTable> mem;
  std::shared_ptr<MemTable> imm;  // being flushed; may be null
  // NEWEST FIRST, AND WHERE THAT MATTERS IS `VersionGet` AND NOT `Build`.
  //
  // The distinction was found by a mutant that SURVIVED. `BM55` reversed the
  // order these sources are handed to the merge and every test stayed green,
  // because `MergedIter` orders by KEY and sequences are unique -- there are no
  // ties for source order to break, so the order it is given is irrelevant.
  //
  // This comment previously read "the first source holding a user key wins",
  // which is true of the point-read path below and FALSE here: in the merge the
  // smallest internal key wins, whatever source it came from. **A comment
  // asserting a load-bearing property for a line where it is not load-bearing
  // is worse than no comment**, because it is where the next reader looks for
  // the invariant and it sends them to a line nothing depends on.
  //
  // The order is kept because a vector that is newest-first everywhere is one
  // fewer thing to get right, not because this loop needs it.
  std::vector<std::shared_ptr<sst::Table>> l0;
  // ASCENDING BY KEY AND NON-OVERLAPPING -- B3-D3(b). The manifest refuses an
  // Open that says otherwise (VerifyL1IsARun), and ConcatIter asserts it again
  // in-process, because the two arrive from different places.
  std::vector<std::shared_ptr<sst::Table>> l1;

  // THE NEWEST RANGE TOMBSTONE COVERING `user_key` AT `snapshot`, ACROSS EVERY
  // STORE. It has to be across every store, because THE TOMBSTONE THAT HIDES A
  // KEY NEED NOT LIVE WHERE THE KEY DOES: a DeleteRange in the memtable hides a
  // value flushed to a table an hour ago, and a reader that asked only the
  // store holding the value would return it.
  wal::SeqNum NewestCovering(Slice user_key, wal::SeqNum snapshot) const {
    wal::SeqNum best = mem->NewestCovering(user_key, snapshot);
    if (imm != nullptr) {
      const wal::SeqNum s = imm->NewestCovering(user_key, snapshot);
      if (s > best) best = s;
    }
    // EVERY L0 FILE, because they overlap and any of them may hold it.
    for (const auto& t : l0) {
      const wal::SeqNum s = t->NewestCovering(user_key, snapshot);
      if (s > best) best = s;
    }
    // L1 IS A RUN, so at most one file's BOUNDS admit the key -- and the bounds
    // include tombstone extents (§6.1a), so the file the search finds is the
    // only one whose finite tombstones can cover it.
    if (const sst::Table* t = L1FileFor(l1, user_key)) {
      const wal::SeqNum s = t->NewestCovering(user_key, snapshot);
      if (s > best) best = s;
    }
    // WITH ONE EXCEPTION, AND IT IS THE ONE THE BOUNDS CANNOT DESCRIBE. A
    // tombstone with no upper bound reaches past every finite bound, so its
    // file is not found by a search over bounds. Only the LAST file of the run
    // may hold one -- anywhere else it would overlap its successor and L1 would
    // stop being a run -- which is asserted where L1 is installed, and is what
    // keeps this an O(1) extra look rather than a scan.
    if (!l1.empty() && l1.back()->check().unbounded_end) {
      const wal::SeqNum s = l1.back()->NewestCovering(user_key, snapshot);
      if (s > best) best = s;
    }
    return best;
  }

  void Build(MergedIter* out) const {
    out->AddMemTable(mem.get());
    if (imm != nullptr) out->AddMemTable(imm.get());
    for (const auto& t : l0) out->AddTable(t.get());
    // ONE SOURCE FOR THE WHOLE RUN, which is B3-D4's entire point: PickSmallest
    // is a linear scan over sources, so k must not grow with the database.
    std::vector<const sst::Table*> run;
    run.reserve(l1.size());
    for (const auto& t : l1) run.push_back(t.get());
    out->AddRun(std::move(run));
  }
};

// The L1 file that could hold `key`, or null. L1 is a run, so AT MOST ONE file
// can -- which is the property the binary search rests on and the property
// VerifyL1IsARun refuses an Open without.
const sst::Table* L1FileFor(const std::vector<std::shared_ptr<sst::Table>>& l1,
                            Slice key) {
  std::size_t lo = 0;
  std::size_t hi = l1.size();
  // CF-3: `hi - lo` strictly shrinks every iteration whichever branch is taken.
  // The comparator decides the direction; it does not decide that the interval
  // shrinks.
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (ExtractUserKey(Slice(l1[mid]->check().largest_key)).compare(key) < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  // AN EXCLUSIVE LARGEST DOES NOT HOLD THE KEY IT NAMES. Splitting a tombstone
  // at an output boundary leaves file i bounded by `[.., B)` and file i+1
  // starting at `B`; the binary search above finds file i for key B, and file i
  // does not contain it. Step past it.
  //
  // CF-3: the progress quantity is `lo`, AN INTEGER INDEX, bounded by
  // `l1.size()` -- neither derived from the comparator nor from the exclusive
  // flag, which are the two things this loop could be wrong about.
  //
  // ITS CORRECTNESS INSTRUMENT IS SEPARATE, per the two-instrument rule:
  // `RangeDelete.ARangeSpanningOutputFilesIsSplitAndEveryPieceApplies` reads
  // every key across a split run, and `Manifest.AdjacentLevelOneTablesAreARun`
  // asserts the touching case is admitted at all. A loop that skipped too far
  // would terminate cleanly and return the wrong file.
  while (lo < l1.size() && l1[lo]->check().largest_is_exclusive &&
         ExtractUserKey(Slice(l1[lo]->check().largest_key)).compare(key) == 0) {
    ++lo;
  }
  if (lo == l1.size()) return nullptr;
  // A COST GUARD, NOT A CORRECTNESS ONE, AND THAT WAS MEASURED RATHER THAN
  // ASSUMED. `BM73` removed this line and NOTHING FAILED: a key falling in the
  // GAP between two files of the run makes the search return the next file
  // along, whose `Get` cannot find the key either, so the answer is identical
  // and only a filter probe is wasted.
  //
  // GF-7's shape, found the same way. The property "a range test decides
  // containment" IS load-bearing -- in the compaction's INPUT SELECTION, where
  // getting it wrong resurrects deleted data -- and `BM80` is the mutant that
  // says so. The mutant went where the property lives instead of where it
  // merely appears; `BM73` was deleted rather than re-aimed.
  if (ExtractUserKey(Slice(l1[lo]->check().smallest_key)).compare(key) > 0) {
    return nullptr;
  }
  return l1[lo].get();
}

// A point read down the merge order: memtable, then the memtable being flushed,
// then tables newest first. It stops at the FIRST source that holds the key,
// because a deletion there hides everything older -- which is why this walks
// sources rather than merging them, and why it is the one read path that can
// use the bloom filter to skip a table entirely.
Status VersionGet(const Version& v, Slice key, wal::SeqNum snapshot,
                  std::string* value);

// APPLY NO LONGER EXPANDS, AND THE CIRCULARITY THAT FORCED IT IS GONE WITH IT.
//
// B2 expanded a `DeleteRange` into one point delete per live key AT APPLY, and
// recorded the expansion in the WAL. Section 8.1's argument for that was
// precise: if the WAL recorded the raw DeleteRange, recovery would have to
// expand it again -- AGAINST A STATE RECOVERY IS STILL IN THE MIDDLE OF
// REBUILDING -- so replay-time expansion was correct only if that state
// provably equalled the state at original Apply time. Correctness by argument,
// with a moving premise.
//
// B3.5 REMOVES THE PREMISE RATHER THAN STRENGTHENING IT. A range tombstone is
// one entry that means the same thing wherever it is replayed: it hides every
// version below its own sequence, and nothing about the surrounding state
// enters into it. Recovery inserts it; it computes nothing.
//
// WHAT STAYS IS THE INTRA-BATCH COLLAPSE, AND IT IS CHEAP NOW. Every op in one
// batch shares ONE sequence, so a range tombstone at that sequence cannot
// express "everything before me in this batch but not after". The model's rule
// -- a DeleteRange covers keys written EARLIER in the same batch, and a Set
// after it re-adds the key -- is therefore resolved HERE, against the batch's
// own ops and nothing else.
//
//   O(batch), not O(live keys). It reads no store, so `[A3]`'s clear-everything
//   case is one entry rather than one point delete per key in the database --
//   and `table.h`'s whole-file residency, which existed to let this function
//   read the merged view, is no longer required by it.
//
// THE SINGLE `<`, AND WHERE IT IS WRITTEN.
//
// The tombstone at sequence S hides versions with sequence STRICTLY BELOW S,
// which is what makes a Set at S survive a DeleteRange at S. The rule is
// spelled in THREE places, and they are not equally load-bearing -- stated
// precisely, because a comment that says "all three agree" invites a reader to
// trust any one of them:
//
//   `VersionGet`  -- LIVE. Every point read.
//   `IterImpl`    -- LIVE. Every scan, forwards and backwards.
//   `MemTable::Get` -- NOT ON ANY READ PATH. No caller in `src/`; the DB reads
//                    memtables through `MergedIter`. It is kept consistent
//                    because a divergent copy misleads, and it is exercised by
//                    the memtable's own tests and by the mutant lane -- but it
//                    is not what a DB read uses.
//
// `RangeDelete.TheSameRuleHoldsAtEveryPlaceItIsWritten` asserts all three, and
// the residual it cannot close is stated there: no single WORKLOAD reaches all
// three, because the third is unreachable from the DB.
struct CollapsedBatch {
  std::vector<wal::Op> ops;
  std::vector<MemRange> ranges;
};

CollapsedBatch CollapseBatch(const WriteBatch& b, std::vector<std::string>* owned) {
  // key -> (kind, value). std::map so the result is sorted by key, which is
  // what B1-D10's collapse costs and what makes "no two memtable entries share
  // a (user_key, seq) pair" assertable.
  std::map<std::string, std::pair<wal::OpKind, std::string>> pending;
  std::vector<MemRange> ranges;

  for (const WriteBatch::Entry& e : b.ops()) {
    switch (e.kind) {  // NO default: arm
      case wal::OpKind::kSet:
        pending[e.key] = {wal::OpKind::kSet, e.value};
        break;
      case wal::OpKind::kDelete:
        pending[e.key] = {wal::OpKind::kDelete, std::string()};
        break;
      case wal::OpKind::kDeleteRange: {
        const Bound start =
            e.value.empty() ? Bound::Unbounded() : Bound::At(Slice(e.key));
        // Everything written EARLIER IN THIS BATCH that falls inside simply
        // ceases to exist. B2 turned these into point deletions; there is no
        // need now, because the tombstone at this sequence hides whatever the
        // key held before the batch, and nothing in the batch survives it.
        for (auto it = pending.begin(); it != pending.end();) {
          it = InRange(Slice(it->first), start, e.end) ? pending.erase(it)
                                                       : std::next(it);
        }
        // AN UNBOUNDED START IS THE EMPTY KEY. The empty user key is the
        // minimum, so `["", end)` and `[unbounded, end)` cover the same set --
        // which is why only the END needed a representation (B3-Q4).
        MemRange r;
        if (start.bounded()) r.start = start.key().ToString();
        r.end_unbounded = !e.end.bounded();
        if (!r.end_unbounded) r.end = e.end.key().ToString();
        // AN EMPTY OR INVERTED RANGE COVERS NOTHING AND IS DROPPED HERE, not
        // written and refused later: the classifier refuses such a record, and
        // a caller is entitled to ask for a range that happens to be empty.
        if (!r.end_unbounded && r.end <= r.start) break;
        // TWO RANGES IN ONE BATCH CAN SHARE A START, and they then share a
        // sequence too -- which is a duplicate the block format refuses. They
        // are merged instead, taking the wider end, because that is what their
        // union is.
        bool merged = false;
        for (MemRange& existing : ranges) {
          if (existing.start != r.start) continue;
          if (r.end_unbounded) {
            existing.end_unbounded = true;
            existing.end.clear();
          } else if (!existing.end_unbounded && r.end > existing.end) {
            existing.end = r.end;
          }
          merged = true;
          break;
        }
        if (!merged) ranges.push_back(std::move(r));
        break;
      }
    }
  }

  owned->clear();
  owned->reserve(pending.size() * 2 + ranges.size() * 2);
  for (const auto& kv : pending) {
    owned->push_back(kv.first);
    owned->push_back(kv.second.second);
  }
  CollapsedBatch out;
  out.ops.reserve(pending.size() + ranges.size());
  std::size_t i = 0;
  for (const auto& kv : pending) {
    wal::Op op;
    op.kind = kv.second.first;
    op.key = Slice((*owned)[i]);
    if (op.kind == wal::OpKind::kSet) op.value = Slice((*owned)[i + 1]);
    out.ops.push_back(op);
    i += 2;
  }
  // THE RANGES GO IN THE WAL TOO, as `kDeleteRange` ops -- the kind B1 reserved
  // for exactly this, and the size formula already accounts for its end key.
  //
  // AN EMPTY END MEANS UNBOUNDED, and that is a different sentinel from the
  // block's `end_len == 0xFFFFFFFF`. The asymmetry is deliberate: the BLOCK
  // must tell "unbounded" apart from "empty end", because it REFUSES the empty
  // one and the refusal has to keep its force. Here there is no such refusal --
  // an empty end never reaches this point, having been dropped above as a range
  // that covers nothing -- so the value the domain already excludes is free to
  // carry the meaning, exactly as `range_offset == 0` does one format over.
  for (const MemRange& r : ranges) {
    owned->push_back(r.start);
    owned->push_back(r.end_unbounded ? std::string() : r.end);
    wal::Op op;
    op.kind = wal::OpKind::kDeleteRange;
    op.key = Slice((*owned)[i]);
    op.value = Slice((*owned)[i + 1]);
    out.ops.push_back(op);
    i += 2;
  }
  out.ranges = std::move(ranges);
  return out;
}

class DBImpl;

// Collapses versions into the view visible at a snapshot.
//
// The memtable holds entries in (user key ASCENDING, seq DESCENDING) order, so
// within one user key the first entry whose sequence is at or below the
// snapshot IS the newest visible version. That ordering is not a convenience:
// it is why a snapshot read is one seek rather than a scan, and it is the
// reason the internal key packs the tag the way it does.
class IterImpl final : public Iterator {
 public:
  IterImpl(Version v, wal::SeqNum snapshot, IterOptions o)
      : v_(std::move(v)), snapshot_(snapshot), o_(std::move(o)) {
    v_.Build(&it_);
  }

  bool First() override {
    if (o_.lower.bounded()) {
      SeekToKey(o_.lower.key());
    } else {
      it_.SeekToFirst();
    }
    return AdvanceToVisible();
  }
  bool Last() override {
    if (o_.upper.bounded()) {
      SeekToKey(o_.upper.key());
      if (it_.Valid()) it_.Prev(); else it_.SeekToLast();
    } else {
      it_.SeekToLast();
    }
    return RetreatToVisible();
  }
  bool SeekGE(Slice key) override {
    Slice from = key;
    if (o_.lower.bounded() && from.compare(o_.lower.key()) < 0) from = o_.lower.key();
    SeekToKey(from);
    return AdvanceToVisible();
  }
  bool SeekLT(Slice key) override {
    SeekToKey(key);
    if (it_.Valid()) it_.Prev(); else it_.SeekToLast();
    return RetreatToVisible();
  }
  bool Next() override {
    if (!valid_) return false;
    SkipVersionsOf(key_);
    return AdvanceToVisible();
  }
  bool Prev() override {
    if (!valid_) return false;
    SeekToKey(Slice(key_));
    it_.Prev();
    return RetreatToVisible();
  }
  bool Valid() const override { return valid_; }
  Slice Key() const override { return Slice(key_); }
  Slice Value() const override { return Slice(value_); }
  Status Error() const override { return Status::Ok(); }
  Status Close() override { valid_ = false; return Status::Ok(); }

 private:
  // Positions at the FIRST (newest) version of `k`, or the first key after it.
  // Tags sort descending, so the largest possible tag is the earliest position
  // within a user key.
  void SeekToKey(Slice k) { it_.Seek(k, ~static_cast<uint64_t>(0)); }

  void SkipVersionsOf(const std::string& k) {
    while (it_.Valid() && it_.user_key() == Slice(k)) it_.Next();
  }

  // Positions on the newest version of the CURRENT user key at or below the
  // snapshot. Returns false if that key has no visible version at all; the
  // cursor is left past the key either way.
  bool SettleOnCurrentKey(const std::string& k) {
    while (it_.Valid() && it_.user_key() == Slice(k)) {
      if ((it_.tag() >> 8) <= snapshot_) return true;
      it_.Next();
    }
    return false;
  }

  // THE TWO LOOPS BELOW TERMINATE BECAUSE THE CURSOR STRICTLY MOVES, and that
  // rests entirely on the comparator being the order it claims to be. The
  // invariant is now ASSERTED rather than commented.
  //
  // BM35 inverts the tag half of the internal key order, and under it this loop
  // never advances: it HUNG A MUTANT LANE FOR ELEVEN HOURS, spinning at 99% of
  // a core, because a hang is not a failure and nothing was watching for one.
  // The lane learned a timeout from that (HARNESS-013); this is the other half.
  //
  // A HANG IS STRICTLY WORSE THAN AN ABORT, in a lane and in production both: an
  // abort names its cause at the point of the mistake, and a hang is
  // indistinguishable from work. The user key is compared bytewise, which is a
  // property of the merged order that does NOT depend on the tag comparator --
  // so this assertion can catch the comparator being wrong.
  bool AdvanceToVisible() {
    std::string previous_key;
    bool have_previous = false;
    while (it_.Valid()) {
      const std::string k = it_.user_key().ToString();
      RIFT_CHECK(!have_previous || k > previous_key);
      previous_key = k;
      have_previous = true;
      if (o_.upper.bounded() && Slice(k).compare(o_.upper.key()) >= 0) break;
      if (SettleOnCurrentKey(k)) {
        // A RANGE TOMBSTONE ABOVE THIS VERSION HIDES THE KEY, exactly as a
        // point deletion does. Asked of the whole Version, not of the store the
        // cursor happens to be in: the tombstone need not live where the value
        // does.
        const bool covered =
            v_.NewestCovering(Slice(k), snapshot_) > SeqOfTag(it_.tag());
        if (!covered && (it_.tag() & 0xff) != 0) {  // a live, uncovered value
          key_ = k;
          value_ = it_.value().ToString();
          valid_ = true;
          return true;
        }
        SkipVersionsOf(k);  // a deletion or a covering range hides it entirely
      }
      // Either way the cursor is now past k, so this loop strictly advances.
    }
    valid_ = false;
    return false;
  }

  bool RetreatToVisible() {
    std::string previous_key;
    bool have_previous = false;
    while (it_.Valid()) {
      const std::string k = it_.user_key().ToString();
      RIFT_CHECK(!have_previous || k < previous_key);
      previous_key = k;
      have_previous = true;
      if (o_.lower.bounded() && Slice(k).compare(o_.lower.key()) < 0) break;
      const bool above_upper =
          o_.upper.bounded() && Slice(k).compare(o_.upper.key()) >= 0;
      if (!above_upper) {
        SeekToKey(Slice(k));
        if (SettleOnCurrentKey(k) && (it_.tag() & 0xff) != 0 &&
            v_.NewestCovering(Slice(k), snapshot_) <= SeqOfTag(it_.tag())) {
          key_ = k;
          value_ = it_.value().ToString();
          valid_ = true;
          return true;
        }
      }
      // Step to the last entry of the PREVIOUS user key. Strictly retreats, so
      // this loop terminates.
      SeekToKey(Slice(k));
      it_.Prev();
    }
    valid_ = false;
    return false;
  }

  // ORDER MATTERS: v_ holds the stores alive and must outlive the cursor that
  // walks them, so it is declared first and destroyed last.
  Version v_;
  MergedIter it_;
  wal::SeqNum snapshot_;
  IterOptions o_;
  bool valid_ = false;
  std::string key_;
  std::string value_;
};

// `S`, LIVE: every sequence a caller can still read at through a snapshot.
//
// WHY THIS EXISTS WHEN PINNING WOULD ALMOST DO. A snapshot holds shared
// pointers to its stores, so it reads through the OLD tables whatever a later
// compaction drops -- which makes S = {the current sequence} sound TODAY, for a
// reason that expires: it rests on the whole SSTable being resident in memory
// after its file is deleted, and B3.5 retires that residency and B3.6 changes
// file lifetime.
//
// THAT IS CORRECTNESS BY ARGUMENT WITH A MOVING PREMISE, which is the shape
// this engine already refused once, at DeleteRange's expansion. A multiset and
// two hooks make the drop rule sound on its own terms instead.
class SnapshotRegistry {
 public:
  void Take(wal::SeqNum s) {
    std::lock_guard<std::mutex> g(mu_);
    live_.insert(s);
  }
  void Release(wal::SeqNum s) {
    std::lock_guard<std::mutex> g(mu_);
    const auto it = live_.find(s);
    RIFT_CHECK(it != live_.end());  // released twice, or never taken
    live_.erase(it);
  }
  // ASCENDING AND DISTINCT, which is RunCompaction's precondition. Two
  // snapshots at one sequence are one member of S.
  std::vector<wal::SeqNum> Live() const {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<wal::SeqNum> out;
    for (wal::SeqNum s : live_) {
      if (out.empty() || out.back() != s) out.push_back(s);
    }
    return out;
  }

 private:
  mutable std::mutex mu_;
  std::multiset<wal::SeqNum> live_;
};

class SnapshotImpl final : public Snapshot {
 public:
  SnapshotImpl(Version v, wal::SeqNum seq,
               std::shared_ptr<SnapshotRegistry> registry)
      : v_(std::move(v)), seq_(seq), registry_(std::move(registry)) {
    registry_->Take(seq_);
  }
  ~SnapshotImpl() override { Release(); }
  Status Get(Slice key, std::string* value) const override {
    return VersionGet(v_, key, seq_, value);
  }
  std::unique_ptr<Iterator> NewIter(const IterOptions& o) const override {
    return std::unique_ptr<Iterator>(new IterImpl(v_, seq_, o));
  }
  // CLOSE AND DESTRUCTION BOTH RELEASE, AND EXACTLY ONE OF THEM COUNTS. The
  // frozen interface has Close; C++ has a destructor; a caller doing both must
  // not remove a live sequence twice, and a caller doing neither would pin the
  // drop rule forever.
  Status Close() override { Release(); return Status::Ok(); }

 private:
  // A SNAPSHOT PINS ITS STORES. The frozen interface says a snapshot "holds its
  // version against compaction until it is Closed"; in B1 that was trivially
  // true because nothing was ever retired. Holding the shared pointers is what
  // makes it true now, and it is why a flush may drop the DB's reference to a
  // memtable while a snapshot taken before it still reads through it.
  void Release() {
    if (released_) return;
    released_ = true;
    registry_->Release(seq_);
  }

  Version v_;
  wal::SeqNum seq_;
  std::shared_ptr<SnapshotRegistry> registry_;
  bool released_ = false;
};

Status VersionGet(const Version& v, Slice key, wal::SeqNum snapshot,
                  std::string* value) {
  // A MEMTABLE Get CANNOT SAY WHY IT FOUND NOTHING, and here that matters: it
  // returns kNotFound both for "no such key" and for "the newest visible
  // version is a deletion", and those differ the moment there is an older store
  // to fall through to. So the memtables are asked through a cursor instead --
  // one seek, and the first visible version it lands on is the answer whatever
  // store it came from.
  //
  // THE TABLES ARE ASKED ONE AT A TIME, NEWEST FIRST, AND HERE THE ORDER IS THE
  // WHOLE ANSWER. The first table holding the user key wins and the walk stops,
  // so an older table's value must never be reached before a newer table's
  // deletion. This is the line `BM55` is pointed at, after it survived being
  // pointed at `Version::Build` -- where the same words were true and the same
  // property was not.
  //
  // It is also the only path on which the bloom filter can skip a whole file.
  // THE NEWEST COVERING RANGE TOMBSTONE, COMPUTED ONCE AND BEFORE THE WALK.
  //
  // It cannot be folded into the walk, because the walk STOPS at the first
  // store holding the key and the tombstone may be in a store it never reaches
  // -- or in one it already passed. Computing it first costs one pass over the
  // tombstone lists, which are bounded by what a flush or a compaction wrote.
  //
  // STRICTLY NEWER HIDES. Equal sequences mean one batch, where a Set issued
  // after a DeleteRange must survive it -- the model's intra-batch rule, and
  // the reason the comparison below is `>` and not `>=`.
  const wal::SeqNum cover = v.NewestCovering(key, snapshot);

  MergedIter mem_only;
  mem_only.AddMemTable(v.mem.get());
  if (v.imm != nullptr) mem_only.AddMemTable(v.imm.get());
  mem_only.Seek(key, MakeTag(snapshot, ValueType::kValue));
  if (mem_only.Valid() && mem_only.user_key() == key) {
    if (cover > SeqOfTag(mem_only.tag())) return Status::NotFound("");
    if (TypeOfTag(mem_only.tag()) == ValueType::kDeletion) return Status::NotFound("");
    *value = mem_only.value().ToString();
    return Status::Ok();
  }
  for (const auto& t : v.l0) {
    bool deleted = false;
    bool filtered = false;
    wal::SeqNum found_seq = 0;
    const Status s = t->Get(key, snapshot, value, &deleted, &filtered, &found_seq);
    if (s.ok()) {
      // A COVERED VALUE IS NOT AN ANSWER, AND THE WALK STOPS ANYWAY: everything
      // older than this version is older than the tombstone too.
      if (cover > found_seq) return Status::NotFound("");
      return s;
    }
    if (deleted) return Status::NotFound("");
  }
  // L1 IS ONE LOOKUP, NOT |L1| LOOKUPS. Every L0 file must be asked because
  // they overlap; at most one L1 file can hold the key, so the read
  // amplification B3-D8 records is |L0| + 1 rather than |L0| + |L1|.
  if (const sst::Table* t = L1FileFor(v.l1, key)) {
    bool deleted = false;
    bool filtered = false;
    wal::SeqNum found_seq = 0;
    const Status s = t->Get(key, snapshot, value, &deleted, &filtered, &found_seq);
    if (s.ok()) {
      if (cover > found_seq) return Status::NotFound("");
      return s;
    }
    if (deleted) return Status::NotFound("");
  }
  return Status::NotFound("");
}

class DBImpl final : public DB {
 public:
  DBImpl(Env* env, std::string dir, wal::Caps caps,
         std::shared_ptr<MemTable> table, std::unique_ptr<wal::Wal> w,
         wal::SeqNum seq, std::unique_ptr<sst::Manifest> manifest,
         sst::ManifestState mstate,
         std::vector<std::shared_ptr<sst::Table>> l0,
         std::vector<std::shared_ptr<sst::Table>> l1,
         wal::SeqNum flushed_through)
      : env_(env), dir_(std::move(dir)), caps_(std::move(caps)),
        mem_(std::move(table)), wal_(std::move(w)), seq_(seq),
        manifest_(std::move(manifest)), mstate_(std::move(mstate)),
        l0_(std::move(l0)), l1_(std::move(l1)),
        flushed_through_(flushed_through) {}

  Status Write(const WriteBatch& b, wal::SeqNum* seq) override {
    std::vector<std::string> owned;
    wal::SeqNum assigned = 0;
    std::vector<wal::Op> ops;
    std::vector<MemRange> ranges;
    // ONE LOCK ACQUISITION, ACROSS THE WAL APPEND TOO -- and B2 is what forces
    // it. B1 released the lock around wal_->Apply, which was harmless while
    // nothing ever replaced wal_ or the memtable. The flush replaces BOTH, so
    // in the gap a Write could name the OLD WAL and then apply to the NEW
    // memtable: the record lands in a log that is about to be deleted and the
    // data lands in a memtable the flushed table does not contain. A LOST
    // WRITE, with no corruption anywhere.
    //
    // It does not weaken the mutex-depth guard, which is about holding the lock
    // across I/O: Apply makes NO Env call by contract (section 8.3), and the
    // Env-call counter asserts it. The lock is still only held around memory.
    {
      wal::DbLock lock(mu_);
      if (closed_) return Status::InvalidArgument("Write after Close");
      // +1 per Write INCLUDING EMPTY ONES, identical to engine/model's counter
      // (section 5.3.4). A sequence space that skipped empty batches would put
      // a translation table inside B4's oracle.
      assigned = ++seq_;
      const CollapsedBatch collapsed = CollapseBatch(b, &owned);
      ops = collapsed.ops;
      ranges = collapsed.ranges;
      Status s = wal_->Apply(assigned, ops);
      if (!s.ok()) {
        // APPLIES NOTHING, ATOMICALLY. The sequence is consumed either way,
        // which is legal -- the frozen contract requires monotonicity, not
        // density -- and is simpler than unwinding a counter two threads can
        // see.
        *seq = assigned;
        return s;
      }
      for (const wal::Op& op : ops) {
        switch (op.kind) {  // NO default: arm
          case wal::OpKind::kSet:
            mem_->Add(assigned, ValueType::kValue, op.key, op.value);
            break;
          case wal::OpKind::kDelete:
            mem_->Add(assigned, ValueType::kDeletion, op.key, Slice());
            break;
          case wal::OpKind::kDeleteRange:
            // Applied from `ranges` below, where the bounds are already
            // normalised. Nothing to do here.
            break;
        }
      }
      for (const MemRange& r : ranges) {
        mem_->AddRangeTombstone(assigned, Slice(r.start), Slice(r.end),
                                r.end_unbounded);
      }
    }
    *seq = assigned;
    return Status::Ok();
  }

  // MONOTONE ACROSS A FLUSH, which is the whole reason this is a maximum and
  // not simply the WAL's number. Rolling the WAL gives the new one a durable
  // sequence of zero, so a DurableSeq that read only the live WAL would go
  // BACKWARDS at every flush -- and the frozen contract says monotone
  // non-decreasing.
  //
  // The floor rises only when a flushed table AND the manifest edit naming it
  // are both durable. Until then the table exists and nothing refers to it, so
  // a crash loses it and the promise would have been false.
  wal::SeqNum DurableSeq() const override {
    wal::DbLock lock(mu_);
    const wal::SeqNum w = wal_->DurableSeq();
    return w > flushed_through_ ? w : flushed_through_;
  }

  Status Sync(wal::SeqNum* watermark) override {
    // ONE CALLER, ASSERTED RATHER THAN ASSUMED -- and B3.4 is what made the
    // difference matter.
    //
    // The contract has always been single-caller: db.h says "B5's poller owns
    // this", and the TSan harness says it in the strongest form there is --
    // "one writer and one syncer ... NOT MORE, BECAUSE MORE WOULD BE A CLAIM
    // THE CONTRACT DOES NOT MAKE."
    //
    // What changed is the cost of violating it. Until B3.4 the manifest had ONE
    // appender; now it has two, and `Manifest::AppendGroup` takes no lock, so
    // two maintenance paths appending at once would interleave records inside
    // one another's groups and write a manifest no reader can replay.
    //
    // AND THE GUARD THAT LOOKS LIKE IT SERIALISES DOES NOT. `Flush` returns
    // early when `imm_ != nullptr`, but `imm_` is set several steps AFTER the
    // first AppendGroup -- so two concurrent Syncs would both pass it. That
    // guard makes a flush a no-op while one is PENDING; it has never been a
    // serialiser, and reading it as one is the trap.
    //
    // So the precondition is enforced where it is cheap and exact: a second
    // concurrent caller aborts at the mistake instead of leaving a corrupt
    // manifest for the next Open to refuse.
    const SingleCaller entered(&in_sync_);

    // NO LOCK HELD. Everything below makes Env calls, and the mutex-depth guard
    // would fire if one were.
    Status s = wal_->Sync(watermark);
    if (!s.ok()) return s;
    // THE FLUSH RUNS HERE AND CAN RUN NOWHERE ELSE. It is I/O, and Write never
    // blocks on I/O -- asserted by the Env-call counter, not promised -- so the
    // only blocking entry point the frozen interface has is this one. A
    // background thread is the other answer and it is not available: A5's scope
    // rule says code that needs a thread is orchestration and lives outside the
    // engine.
    bool needed = false;
    { wal::DbLock lock(mu_); needed = !closed_ && mem_->MemoryUsage() >= caps_.flush_bytes; }
    if (needed) {
      const Status f = Flush();
      if (!f.ok()) return f;
    }
    // COMPACTION RUNS WHERE THE FLUSH RUNS, and for the same reason: it is I/O,
    // and this is the only blocking entry point the frozen interface has. It
    // runs AFTER the flush because the flush is what pushes L0 over the
    // trigger, so a Sync that flushes can compact in the same call rather than
    // leaving L0 one file over until the next one.
    const Status c = MaybeCompact();
    if (!c.ok()) return c;
    // AND ANY FILE WHOSE LAST READER HAS GONE. Here rather than at Close time,
    // because a Close makes no Env call by contract and this is one -- and
    // because `Sync` is the only blocking entry point the frozen interface has.
    const Status o = DropObsolete();
    if (!o.ok()) return o;
    if (!needed) return s;
    { wal::DbLock lock(mu_); *watermark = wal_->DurableSeq() > flushed_through_
                                              ? wal_->DurableSeq() : flushed_through_; }
    return s;
  }

  Status Get(Slice key, std::string* value) const override {
    wal::SeqNum snap;
    Version v;
    { wal::DbLock lock(mu_); snap = seq_; v = CurrentVersionLocked(); }
    return VersionGet(v, key, snap, value);
  }

  // AN ITERATOR IS NOT IN `S`, AND THAT IS A DELIBERATE ASYMMETRY WITH
  // Snapshot -- stated because the two look alike and are not.
  //
  // The frozen interface promises pinning for `Snapshot` in so many words: "it
  // holds its version against compaction until it is Closed." It promises
  // nothing of the sort for `Iterator`. This one is consistent anyway, because
  // it captures a sequence and holds shared pointers to its stores -- but that
  // is the residency argument, and B3.5 and B3.6 both move it.
  //
  // So it is a NAMED GAP for B3.6's step (version lifetime, file deletion after
  // the last reference) rather than a silent one. The drop rule does not rest
  // on it: see SnapshotRegistry for why the promise that IS made got a registry
  // instead of an argument.
  std::unique_ptr<Iterator> NewIter(const IterOptions& o) const override {
    wal::SeqNum snap;
    Version v;
    { wal::DbLock lock(mu_); snap = seq_; v = CurrentVersionLocked(); }
    return std::unique_ptr<Iterator>(new IterImpl(std::move(v), snap, o));
  }

  std::unique_ptr<Snapshot> NewSnapshot() override {
    wal::DbLock lock(mu_);
    return std::unique_ptr<Snapshot>(
        new SnapshotImpl(CurrentVersionLocked(), seq_, snapshots_));
  }

  Status ApproximateDiskBytes(const Bound& start, const Bound& end,
                              uint64_t* out) const override {
    wal::SeqNum snap;
    Version v;
    { wal::DbLock lock(mu_); snap = seq_; v = CurrentVersionLocked(); }
    uint64_t total = 0;
    IterOptions o;
    o.lower = start;
    o.upper = end;
    IterImpl it(std::move(v), snap, o);
    for (bool ok = it.First(); ok; ok = it.Next()) {
      total += it.Key().size() + it.Value().size();
    }
    *out = total;
    return Status::Ok();
  }

  Status Close() override {
    { wal::DbLock lock(mu_); if (closed_) return Status::Ok(); closed_ = true; }
    // The error is RETURNED, not dropped. close(2) reports EIO for writeback
    // that failed after the last Sync, which is the last moment anyone can
    // learn the data is gone (BM7).
    const Status w = wal_->Close();
    const Status m = manifest_ != nullptr ? manifest_->Close() : Status::Ok();
    return w.ok() ? m : w;
  }

 private:
  Version CurrentVersionLocked() const {
    Version v;
    v.mem = mem_;
    v.imm = imm_;
    v.l0 = l0_;
    v.l1 = l1_;
    return v;
  }

  // FILE LIFETIME BY REFERENCE COUNT -- B3.6, AND IT REPLACES AN ARGUMENT.
  //
  // Until now, deleting an input file while a snapshot still read through it
  // was safe for ONE reason: `table.h` holds the whole image in memory, so the
  // bytes outlive the directory entry. That is correctness by an argument whose
  // premise moves (`GF-20`) -- and the mover is named: anything that reads a
  // block on demand, which is the first thing a cache would do.
  //
  // THE COUNT IS `shared_ptr::use_count()`, NOT A SECOND COUNTER. Every reader
  // already holds a `shared_ptr<Table>` for exactly as long as it may read:
  // `Version` copies them under the lock, and a `Snapshot` or `Iterator` holds
  // its `Version` for its whole life.
  //
  //   A SEPARATE REFCOUNT WOULD BE A SECOND SOURCE OF TRUTH ABOUT ONE FACT.
  //
  // That is the one-fact-two-places class -- the shape Track A paid for in
  // `BUG-032` -- and it is avoided here BY CONSTRUCTION rather than by keeping
  // two numbers in step. There is no second number to keep in step: the thing
  // that makes a reader a reader is the same thing that counts it.
  //
  // EVERY RETIRED TABLE GOES THROUGH `obsolete_`, AND THAT IS THE WHOLE POINT
  // OF THE DESIGN RATHER THAN A DETOUR.
  //
  // The first version compared `use_count()` at the retirement site against a
  // threshold worked out by reasoning: "`t` is one reference and the caller's
  // vector is another, so above two is a reader." IT DOUBLE-COUNTED THE SAME
  // REFERENCE -- `t` is a reference TO the vector's element, not a copy -- and
  // the file was deleted with a snapshot still holding it.
  //
  //   A REFERENCE-COUNT THRESHOLD DERIVED BY REASONING ABOUT WHICH LOCALS
  //   HAPPEN TO EXIST IS A NUMBER THAT CHANGES WHEN SOMEONE ADDS A VARIABLE.
  //
  // So there is exactly ONE place a count is taken and exactly ONE holder to
  // subtract: `obsolete_` itself. `use_count() == 1` there means the list is
  // the only holder. Nothing to derive, and adding a local anywhere else cannot
  // move it.
  //
  // WHAT A CRASH DOES TO A FILE STILL WAITING: nothing that matters. The
  // manifest edit removing it is already durable, so the next Open finds an
  // UNNAMED `.sst` and deletes it as an orphan -- B2-D5 candidate (c), which
  // this reuses rather than extends. A leaked file is removed by the next Open;
  // a file deleted too early is a wrong answer.
  void RetireTable(const std::shared_ptr<sst::Table>& t) {
    wal::DbLock lock(mu_);
    obsolete_.push_back(t);
  }

  // Deletes every obsolete file whose last reader has gone. Called where an Env
  // call is already permitted -- never from a read path, which must make none.
  Status DropObsolete() {
    std::vector<std::shared_ptr<sst::Table>> ready;
    {
      wal::DbLock lock(mu_);
      std::vector<std::shared_ptr<sst::Table>> still_held;
      for (const auto& t : obsolete_) {
        // One reference is `obsolete_`'s own; anything more is a live reader.
        (t.use_count() > 1 ? still_held : ready).push_back(t);
      }
      obsolete_ = std::move(still_held);
    }
    if (ready.empty()) return Status::Ok();
    for (const auto& t : ready) {
      const Status s = env_->DeleteFile(sst::TablePath(dir_, t->number()));
      if (!s.ok()) return s;
    }
    return SyncDir();
  }

  // AT MOST THE LAST FILE OF THE RUN MAY REACH TO INFINITY.
  //
  // A tombstone with no upper bound covers every key above its start, so a file
  // holding one overlaps every file after it -- and L1 would stop being a run.
  // Only the last file has no successor to overlap, which is why B3.5e splits
  // an unbounded tombstone at each output boundary and leaves the unbounded
  // piece in the LAST output alone.
  //
  // ASSERTED HERE, WHERE L1 IS INSTALLED, because `Version::NewestCovering`
  // DEPENDS ON IT: it consults the file the binary search finds, plus the last
  // file if that one is unbounded, and nothing else. An unbounded tombstone
  // anywhere else would be invisible to every read that did not land on its
  // file -- a range delete that silently stops applying.
  static void CheckOnlyTheLastMayBeUnbounded(
      const std::vector<std::shared_ptr<sst::Table>>& l1) {
    for (std::size_t i = 0; i + 1 < l1.size(); ++i) {
      // THE MESSAGE CARRIES THE CONSEQUENCE, NOT THE RULE. Whoever hits this is
      // holding a wrong answer, not a style violation, and needs to know what
      // it looks like from the outside.
      RIFT_CHECK_MSG(!l1[i]->check().unbounded_end,
                     "an L1 file that is not the last of the run holds a range "
                     "tombstone with no upper bound: every read that does not "
                     "land on this file will MISS IT, so the range delete "
                     "silently stops applying above this file's bounds and "
                     "deleted data comes back");
    }
  }

  // WHEN TO COMPACT -- B3-D3(b), and the number with its derivation at the
  // definition site, per section 8.4.
  //
  // Read amplification for a point lookup is |L0| + 1: every L0 file must be
  // asked because they overlap, and at most one L1 file can hold the key. So
  // the trigger IS the read-amplification bound, and 4 is chosen to match the
  // structure this engine already has rather than copied: a flush produces one
  // L0 file, so the trigger is how many flushes may accumulate before their
  // cost is paid, and the bloom filter makes an absent key cost a filter probe
  // rather than a block read.
  //
  // THE MEASUREMENT THAT WOULD MOVE IT is B3.7's, which records read and space
  // amplification against this number. Until then it is stated, not tuned.
  static constexpr std::size_t kL0CompactionTrigger = 4;

  // ---------------------------------------------------------------------
  // THE INSTALL ORDERING, AND IT IS B2-D5's WITH ONE SENTENCE ADDED.
  //
  //   1. write the output table, Sync it
  //   2. Directory::Sync            (its NAME becomes durable)
  //   3. one manifest group: the counter, the ADD of the output, the DELETE of
  //      every input; AppendGroup Syncs
  //   4. Directory::Sync            (the manifest's extent is durable)
  //   5. only now: delete the input files, then Directory::Sync
  //
  // THE ADDED SENTENCE: the add and the deletes are ONE GROUP because the
  // output becoming live and the inputs ceasing to be are the same fact. A
  // crash between them either loses the compaction's work or loses the data,
  // and only one ordering makes both impossible.
  //
  // A crash before 3 leaves the output as an unnamed .sst, which Open removes.
  // A crash between 3 and 5 leaves the INPUTS as unnamed .sst files, which Open
  // removes by the same rule. Neither window loses a version.
  Status MaybeCompact() {
    std::vector<std::shared_ptr<sst::Table>> in_l0;
    std::vector<std::shared_ptr<sst::Table>> in_l1;
    std::vector<std::shared_ptr<sst::Table>> keep_l1;
    std::vector<wal::SeqNum> observable;
    {
      wal::DbLock lock(mu_);
      if (closed_ || compacting_ || l0_.size() < kL0CompactionTrigger) {
        return Status::Ok();
      }
      compacting_ = true;
      in_l0 = l0_;
      // THE INPUT SET IS EVERY L0 FILE PLUS EVERY L1 FILE THAT OVERLAPS THEM,
      // and that is a correctness requirement rather than a policy: clause 2
      // of the drop claim permits dropping a tombstone only when nothing older
      // survives ANYWHERE, so a compaction that left an overlapping L1 file out
      // could resurrect deleted data.
      std::string lo = ExtractUserKey(Slice(in_l0[0]->check().smallest_key)).ToString();
      std::string hi = ExtractUserKey(Slice(in_l0[0]->check().largest_key)).ToString();
      for (const auto& t : in_l0) {
        const std::string a = ExtractUserKey(Slice(t->check().smallest_key)).ToString();
        const std::string b = ExtractUserKey(Slice(t->check().largest_key)).ToString();
        if (a < lo) lo = a;
        if (b > hi) hi = b;
      }
      for (const auto& t : l1_) {
        const Slice a = ExtractUserKey(Slice(t->check().smallest_key));
        const Slice b = ExtractUserKey(Slice(t->check().largest_key));
        const bool overlaps = a.compare(Slice(hi)) <= 0 && b.compare(Slice(lo)) >= 0;
        (overlaps ? in_l1 : keep_l1).push_back(t);
      }
    }
    // `S`, READ ONCE, BEFORE THE FIRST INPUT ENTRY IS CONSUMED. B3-Q2 is ruled:
    // S is the LIVE snapshots, plus the sequence a caller reads at with no
    // snapshot. A retired snapshot has no reader that can observe what it
    // pinned, so keeping its versions required would make compaction unable to
    // reclaim space nothing can see -- the same over-requirement `keep(k)` had.
    observable = ObservableNow();
    // AND READING IT LATER WOULD ALSO BE SAFE, WHICH IS WORTH KNOWING RATHER
    // THAN GUESSING AT. `BM84` planted the obvious future optimization -- read
    // S as late as possible so it is as small as possible so more can be
    // dropped -- and it SURVIVED, correctly: a release only makes the kept set
    // larger than required, and an acquisition lands above every sequence the
    // inputs hold. The timing is not what carries the correctness.
    //
    // WHAT CARRIES IT is the assertion in DoCompact, `pin_seq <= max(S)`, and
    // that is where a future change would break. The mutant was deleted rather
    // than kept as a class that can never fail; this comment is its record.
    //
    // It is read here anyway, because ONCE-AT-THE-START is the property that
    // stays true if the inputs ever stop being frozen at selection time.
    const Status s = DoCompact(in_l0, in_l1, keep_l1, observable);
    { wal::DbLock lock(mu_); compacting_ = false; }
    if (!s.ok()) return s;
    // THE LOCAL INPUT VECTORS ARE RELEASED BEFORE THE COLLECTION, and that
    // ordering is the whole reason this is here rather than inside DoCompact:
    // while they are alive they are readers by `use_count`'s reckoning, and
    // nothing could ever be collected in the same call.
    in_l0.clear();
    in_l1.clear();
    keep_l1.clear();
    return DropObsolete();
  }

  // ROLLS THE OUTPUT INTO A RUN OF BOUNDED FILES.
  //
  // ONE OUTPUT FILE WOULD BE CANDIDATE (a) WEARING (b)'s NAME: every compaction
  // would rewrite the whole database, which is exactly the write amplification
  // B3-D3 rejected (a) for. With a run, a compaction reads only the L1 files
  // that overlap its inputs.
  //
  // THE SIZE IS DERIVED, NOT CHOSEN: an output file is capped at the FLUSH
  // THRESHOLD, so an L1 file is the same order as the L0 file that produced it.
  // That ties it to a number that already has a derivation (caps.h) instead of
  // inventing a second one, and it moves with the caps -- so the sweep, which
  // sets the flush threshold low to make flushes reachable, gets a multi-file
  // L1 for free rather than needing a second knob.
  class TableRoller final : public CompactionSink {
   public:
    TableRoller(DBImpl* db, uint64_t max_bytes) : db_(db), max_bytes_(max_bytes) {}

    Status Add(Slice internal_key, Slice value, bool boundary) override {
      if (builder_ != nullptr && boundary && builder_->file_size() >= max_bytes_) {
        // THE ROLL POINT IS THE BOUNDARY THE TOMBSTONES ARE CLIPPED AT. The
        // file being closed covers `[file_start_, this key)`, and the next one
        // begins here.
        boundary_ = ExtractUserKey(internal_key).ToString();
        const Status s = CloseCurrent();
        if (!s.ok()) return s;
      }
      if (builder_ == nullptr) {
        file_start_ = ExtractUserKey(internal_key).ToString();
        have_start_ = true;
        const Status s = OpenNext();
        if (!s.ok()) return s;
      }
      builder_->Add(internal_key, value);
      return builder_->status();
    }

    void SetTombstones(std::vector<CompactionTombstone> tombstones) override {
      tombstones_ = std::move(tombstones);
      // ASCENDING BY START, which is what the block requires -- and ties broken
      // by sequence so the order is total and no two are indistinguishable.
      std::sort(tombstones_.begin(), tombstones_.end(),
                [](const CompactionTombstone& a, const CompactionTombstone& b) {
                  if (a.start != b.start) return a.start < b.start;
                  return a.seq < b.seq;
                });
    }

    // Finishes whatever is open. Called once, after RunCompaction returns.
    //
    // The last file has no successor, so its tombstones are not clipped above
    // and it is the one that may carry an unbounded piece.
    Status Finish() {
      boundary_.reset();
      if (builder_ == nullptr) {
        // NOTHING WAS EMITTED. Tombstones that survived still have to be
        // written, or a compaction that dropped every point version would drop
        // the tombstones with them -- and a tombstone survives precisely when
        // something below it is still observable.
        if (tombstones_.empty()) return Status::Ok();
        const Status s = OpenNext();
        if (!s.ok()) return s;
        have_start_ = false;
      }
      return CloseCurrent();
    }

    const std::vector<sst::TableMeta>& outputs() const { return outputs_; }

   private:
    Status OpenNext() {
      {
        wal::DbLock lock(db_->mu_);
        number_ = db_->mstate_.next_file_number;
        db_->mstate_.next_file_number = number_ + 1;
      }
      const Status s = db_->env_->NewWritableFile(sst::TablePath(db_->dir_, number_),
                                                  &file_);
      if (!s.ok()) return s;
      builder_.reset(new sst::TableBuilder(file_.get()));
      return Status::Ok();
    }

    // ONE FILE'S WHOLE ORDERING, IN ONE PLACE: Finish, Sync, Close. The
    // DIRECTORY sync is the caller's, once, after the last file -- B2-D5's
    // step 2 for a run rather than for a file.
    // EVERY TOMBSTONE OVERLAPPING THIS FILE, CLIPPED TO IT. See compaction.h:
    // an unclipped tombstone reaches past its file's bounds, and input
    // selection reads those bounds.
    void WriteClippedTombstones() {
      const bool last_file = !boundary_.has_value();
      for (const CompactionTombstone& t : tombstones_) {
        std::string lo = t.start;
        if (have_start_ && lo < file_start_) lo = file_start_;
        const uint64_t tag = MakeTag(t.seq, ValueType::kDeletion);
        if (t.end_unbounded && last_file) {
          // ONLY THE LAST FILE MAY CARRY THE UNBOUNDED PIECE, because only the
          // last has no successor to overlap. Every earlier file gets the
          // tombstone clipped to its own boundary instead.
          builder_->AddUnboundedRangeTombstone(Slice(lo), tag);
          continue;
        }
        std::string hi;
        if (t.end_unbounded) {
          hi = *boundary_;
        } else {
          hi = t.end;
          if (!last_file && hi > *boundary_) hi = *boundary_;
        }
        if (!(lo < hi)) continue;  // the clip left nothing in this file
        builder_->AddRangeTombstone(Slice(lo), Slice(hi), tag);
      }
    }

    Status CloseCurrent() {
      WriteClippedTombstones();
      Status s = builder_->Finish();
      if (!s.ok()) return s;
      sst::TableMeta meta;
      meta.number = number_;
      meta.level = 1;
      meta.file_bytes = builder_->file_size();
      meta.smallest = builder_->smallest().ToString();
      meta.largest = builder_->largest().ToString();
      meta.largest_seq = builder_->largest_seq();
      outputs_.push_back(meta);
      builder_.reset();
      have_start_ = false;
      s = file_->Sync();
      if (!s.ok()) return s;
      s = file_->Close();
      file_.reset();
      return s;
    }

    DBImpl* db_;
    uint64_t max_bytes_;
    uint64_t number_ = 0;
    WritableFilePtr file_;
    std::unique_ptr<sst::TableBuilder> builder_;
    std::vector<sst::TableMeta> outputs_;
    std::vector<CompactionTombstone> tombstones_;
    std::string file_start_;
    bool have_start_ = false;
    // Set when a roll happens, cleared for the file that has no successor. The
    // LAST file is the only one whose upper bound is open, which is why it is
    // the only one that may hold an unbounded tombstone.
    std::optional<std::string> boundary_;
  };

  // `S`. Takes the lock itself, so it is called OUTSIDE one.
  std::vector<wal::SeqNum> ObservableNow() const {
    std::vector<wal::SeqNum> s = snapshots_->Live();
    wal::DbLock lock(mu_);
    // WITHOUT THE VISIBLE SEQUENCE, `S` CAN BE EMPTY -- and an empty S makes
    // every version of every key unobservable and therefore droppable, which is
    // the one drop no compaction may ever make. B3-Q2's ruling is "live
    // snapshots only"; reading it as "live snapshots ALONE" is the dangerous
    // misreading, and `BM83` is the mutant that keeps it visible.
    if (s.empty() || s.back() != seq_) s.push_back(seq_);
    std::sort(s.begin(), s.end());
    s.erase(std::unique(s.begin(), s.end()), s.end());
    return s;
  }

  Status DoCompact(const std::vector<std::shared_ptr<sst::Table>>& in_l0,
                   const std::vector<std::shared_ptr<sst::Table>>& in_l1,
                   const std::vector<std::shared_ptr<sst::Table>>& keep_l1,
                   const std::vector<wal::SeqNum>& observable) {
    // THE DERIVED BOUND OF B3-D7a, and `pin_seq` beside it. Both are counted
    // from the inputs before the merge starts, out of what ValidateTable
    // already measured when each table was opened -- GF-13: a bound derived
    // from another instrument's measurement cannot be raised without
    // contradicting that instrument. There is no number here to tune.
    uint64_t bound = 0;
    wal::SeqNum pin_seq = 0;
    std::vector<const sst::Table*> run;
    MergedIter merge;
    // EVERY TOMBSTONE THE INPUTS HOLD, COPIED OUT. The input tables are deleted
    // at step 5, so nothing may hold Slices into their images past the install
    // -- and a `CompactionTombstone` owns its bounds for that reason.
    std::vector<CompactionTombstone> tombstones;
    const auto collect = [&tombstones](const sst::Table* t) {
      for (const sst::RangeTombstone& rt : t->tombstones()) {
        CompactionTombstone c;
        c.start = rt.start.ToString();
        if (!rt.end_unbounded) c.end = rt.end.ToString();
        c.end_unbounded = rt.end_unbounded;
        c.seq = rt.seq();
        tombstones.push_back(std::move(c));
      }
    };
    for (const auto& t : in_l0) {
      bound += t->check().entries;
      if (t->check().largest_seq > pin_seq) pin_seq = t->check().largest_seq;
      collect(t.get());
      merge.AddTable(t.get());
    }
    for (const auto& t : in_l1) {
      bound += t->check().entries;
      if (t->check().largest_seq > pin_seq) pin_seq = t->check().largest_seq;
      collect(t.get());
      run.push_back(t.get());
    }
    merge.AddRun(std::move(run));

    // WHY `S` IS SAFE TO READ ONCE, AND THE ASSERTION THAT MAKES IT SO.
    //
    // `S` moves under a running compaction, in two directions, and the design's
    // §1.3 argues both are safe. One of the two arguments rests on a fact about
    // the INPUTS, and a fact is assertable:
    //
    //   A SNAPSHOT RELEASED during the compaction shrinks `S`. The compaction
    //   then kept versions nothing can observe -- OVER-KEEPING, which the claim
    //   permits in so many words. Safe with nothing to check.
    //
    //   A SNAPSHOT TAKEN during the compaction gets the CURRENT sequence, which
    //   only ever increases. If every sequence the inputs hold is at or below
    //   the visible sequence already in `S`, then for any later `s`, keep(k) at
    //   `s` is the newest version of k in the inputs -- WHICH THE VISIBLE
    //   SEQUENCE ALREADY REQUIRED. A later snapshot requires NOTHING NEW.
    //
    // That second argument is the whole of why no lock is held across the
    // compaction, and it is exactly `pin_seq <= max(S)`. It is true today
    // because tables hold only flushed data while `seq_` runs ahead of them --
    // and it would STOP being true the day a compaction took the immutable
    // memtable as an input, which is a change someone will propose.
    RIFT_CHECK(!observable.empty());
    RIFT_CHECK(pin_seq <= observable.back());

    // BOTTOM-MOST, COMPUTED RATHER THAN ASSUMED. The inputs hold every version
    // of every key they contain exactly when no L1 file left out of them can
    // reach into their key range. It is true by construction here -- L1 is a
    // run and every overlapping member was taken -- and it is computed anyway,
    // because clause 2's soundness rests on it and a false belief about it
    // resurrects deleted data with nothing structurally wrong anywhere.
    //
    // The memtables are not a hazard: they hold sequences ABOVE every table's,
    // so they can only hide an input version, never be hidden by one.
    bool bottom_most = true;
    if (!keep_l1.empty()) {
      std::string lo, hi;
      bool first = true;
      const auto widen = [&](const std::shared_ptr<sst::Table>& t) {
        const std::string a = ExtractUserKey(Slice(t->check().smallest_key)).ToString();
        const std::string b = ExtractUserKey(Slice(t->check().largest_key)).ToString();
        if (first) { lo = a; hi = b; first = false; return; }
        if (a < lo) lo = a;
        if (b > hi) hi = b;
      };
      for (const auto& t : in_l0) widen(t);
      for (const auto& t : in_l1) widen(t);
      for (const auto& t : keep_l1) {
        const Slice a = ExtractUserKey(Slice(t->check().smallest_key));
        const Slice b = ExtractUserKey(Slice(t->check().largest_key));
        if (a.compare(Slice(hi)) <= 0 && b.compare(Slice(lo)) >= 0) bottom_most = false;
      }
    }

    // 1. the output run, each file Finished, Synced and Closed as it fills.
    CompactionStats stats;
    TableRoller roller(this, caps_.flush_bytes);
    {
      Status s = RunCompaction(&merge, observable, bottom_most, pin_seq, bound,
                               tombstones, &roller, &stats);
      if (!s.ok()) return s;
      s = roller.Finish();
      if (!s.ok()) return s;
    }
    const std::vector<sst::TableMeta>& outputs = roller.outputs();
    // A COMPACTION MAY EMIT NO POINT VERSIONS AND STILL PRODUCE A FILE, when
    // tombstones survive but everything they hid is gone. `outputs` is the
    // authority on what was written, never `stats.emitted`.

    // 2. the directory, so every output's NAME is durable. ONCE, after the last
    //    file: the manifest edit that names them has not been written yet, so
    //    until step 3 they are all equally invisible.
    Status s = SyncDir();
    if (!s.ok()) return s;

    // 3. ONE GROUP: the counter, the add, and every delete.
    {
      std::vector<sst::ManifestEdit> group;
      sst::ManifestEdit bump;
      bump.kind = sst::EditKind::kNextFileNumber;
      {
        // THE COUNTER AS IT STANDS NOW, not as it stood when the number was
        // reserved. A flush between the two would have raised it, and writing
        // the older value here would move the durable counter BACKWARDS below a
        // live file -- which is exactly the manifest BM54 refuses.
        wal::DbLock lock(mu_);
        bump.number = mstate_.next_file_number;
      }
      group.push_back(bump);
      for (const sst::TableMeta& meta : outputs) {
        sst::ManifestEdit add;
        add.kind = sst::EditKind::kAddTable;
        add.table = meta;
        group.push_back(add);
      }
      for (const auto& t : in_l0) {
        sst::ManifestEdit del;
        del.kind = sst::EditKind::kDeleteTable;
        del.number = t->number();
        group.push_back(del);
      }
      for (const auto& t : in_l1) {
        sst::ManifestEdit del;
        del.kind = sst::EditKind::kDeleteTable;
        del.number = t->number();
        group.push_back(del);
      }
      s = manifest_->AppendGroup(group);
      if (!s.ok()) return s;
    }

    // 4. the directory again.
    s = SyncDir();
    if (!s.ok()) return s;

    // The outputs and the edges naming them are all durable, so everything they
    // hold now survives a crash. Only now does the in-memory state move.
    std::vector<std::shared_ptr<sst::Table>> opened;
    for (const sst::TableMeta& meta : outputs) {
      std::shared_ptr<sst::Table> t;
      s = sst::Table::Open(env_, sst::TablePath(dir_, meta.number), meta.number, &t);
      if (!s.ok()) return s;
      opened.push_back(std::move(t));
    }
    {
      wal::DbLock lock(mu_);
      std::set<uint64_t> gone;
      for (const auto& t : in_l0) gone.insert(t->number());
      for (const auto& t : in_l1) gone.insert(t->number());
      std::vector<std::shared_ptr<sst::Table>> kept_l0;
      for (const auto& t : l0_) {
        if (gone.find(t->number()) == gone.end()) kept_l0.push_back(t);
      }
      l0_ = std::move(kept_l0);
      l1_ = keep_l1;
      for (const auto& t : opened) l1_.push_back(t);
      std::sort(l1_.begin(), l1_.end(),
                [](const std::shared_ptr<sst::Table>& a,
                   const std::shared_ptr<sst::Table>& b) {
                  return ExtractUserKey(Slice(a->check().smallest_key))
                             .compare(ExtractUserKey(Slice(b->check().smallest_key))) < 0;
                });
      CheckOnlyTheLastMayBeUnbounded(l1_);
      for (uint64_t n : gone) mstate_.tables.erase(n);
      for (const sst::TableMeta& meta : outputs) mstate_.tables[meta.number] = meta;
    }

    // 5. only now: the input files -- OR, IF ANYONE IS STILL READING THEM, a
    //    record that they are obsolete and a deletion when the last reader
    //    goes. See `ObsoleteFiles` below: B3.6 replaces "the bytes are
    //    resident so deleting the file is safe" with a reference count, which
    //    is what the frozen interface's "holds its version against compaction
    //    until it is Closed" has always meant.
    //    A compaction that emits NOTHING leaves no output file at all -- the
    //    roller opens one on its first entry and never before -- which is the
    //    correct outcome for a key written and deleted with no snapshot below
    //    it, and it is why TableBuilder may keep refusing to Finish an empty
    //    table.
    //
    //    DELETING A FILE A LIVE SNAPSHOT STILL READS IS SAFE HERE ONLY BECAUSE
    //    THE WHOLE TABLE IS RESIDENT (table.h). That is a property B3.5 and
    //    B3.6 change, and file lifetime by reference count is B3.6's step. The
    //    DROP RULE does not rest on it -- see SnapshotRegistry.
    for (const auto& t : in_l0) RetireTable(t);
    for (const auto& t : in_l1) RetireTable(t);
    // AND RECLAIMED IMMEDIATELY IF NOBODY IS READING. `DropObsolete` is called
    // from `Sync` as well, which is what eventually collects a file whose
    // reader outlives this call -- but a compaction nobody is reading through
    // must not leave its inputs on disk until the next Sync, or `Tables()`
    // never settles and the space is not reclaimed when it could be.
    //
    // THE CALLER'S `in_l0`/`in_l1` STILL HOLD REFERENCES AT THIS POINT, so
    // these files are not yet collectable here -- they are collected by the
    // `Sync` that follows, which is why the compaction path calls it below.
    return DropObsolete();
  }

  // B2-D5's ordering, and there is only one correct one. Every adjacent pair
  // below is a kill point the sweep visits, and the crash-consistency claim for
  // B2 is that every one of them recovers to a promised watermark.
  //
  //   1. write the SSTable, Sync it
  //   2. Directory::Sync              (its NAME becomes durable)
  //   3. append the manifest edit, Sync it
  //   4. Directory::Sync              (the manifest's extent is durable)
  //   5. only now: delete the WAL(s) fully covered, then Directory::Sync
  //
  // Reversing 3 and 5 loses data outright. Reversing 1 and 3 names a file that
  // may not exist.
  Status Flush() {
    // NUMBERS ARE RESERVED IN THE MANIFEST BEFORE ANY FILE USES THEM. A crash
    // between allocating a number and recording it would leave the counter
    // below a file that exists, and the next allocation would then truncate a
    // live file by creating its replacement. BM54 is the check that refuses
    // such a manifest; this is what stops one being written.
    uint64_t table_number = 0;
    uint64_t new_wal_number = 0;
    {
      wal::DbLock lock(mu_);
      // A NO-OP WHILE A FLUSH IS PENDING -- WHICH IS NOT THE SAME AS A
      // SERIALISER, AND WAS READ AS ONE. `imm_` is set several steps below,
      // AFTER the first AppendGroup, so two concurrent callers would both pass
      // this line. It never serialised anything; `Sync`'s SingleCaller does
      // that, and it exists because B3.4 gave the manifest a second appender.
      if (closed_ || imm_ != nullptr) return Status::Ok();
      table_number = mstate_.next_file_number;
      new_wal_number = table_number + 1;
      mstate_.next_file_number = table_number + 2;
    }
    // CREATE THE FILE, THEN NAME IT, AND ONLY THEN WRITE TO IT. The window
    // between creation and naming leaves an EMPTY unnamed WAL, which recovery
    // proves empty and deletes. The other order leaves a NAME WITH NO FILE,
    // which persists and destroys the meaning of "named and absent" forever
    // after -- see manifest.h, and the 41 sweep violations that found it.
    std::unique_ptr<wal::Wal> fresh;
    Status s = wal::Wal::Open(env_, dir_, new_wal_number, caps_, &fresh);
    if (!s.ok()) return s;
    {
      sst::ManifestEdit bump;
      bump.kind = sst::EditKind::kNextFileNumber;
      bump.number = table_number + 2;
      sst::ManifestEdit add_wal;
      add_wal.kind = sst::EditKind::kAddWal;
      add_wal.number = new_wal_number;
      s = manifest_->AppendGroup({bump, add_wal});
      if (!s.ok()) return s;
      wal::DbLock lock(mu_);
      mstate_.wals.insert(new_wal_number);
    }

    // THE ROLL IS ONE LOCKED STEP. A Write must land in the old memtable AND
    // the old WAL, or in the new memtable AND the new WAL -- never one of each.
    std::shared_ptr<MemTable> flushing;
    std::unique_ptr<wal::Wal> retiring;
    uint64_t retired_number = 0;
    wal::SeqNum roll_seq = 0;
    {
      wal::DbLock lock(mu_);
      flushing = mem_;
      imm_ = flushing;
      mem_ = std::make_shared<MemTable>();
      retiring = std::move(wal_);
      wal_ = std::move(fresh);
      retired_number = retiring->file_number();
      roll_seq = seq_;
    }

    // 1. the table, then its own Sync.
    const std::string path = sst::TablePath(dir_, table_number);
    sst::TableMeta meta;
    meta.number = table_number;
    {
      WritableFilePtr f;
      s = env_->NewWritableFile(path, &f);
      if (!s.ok()) return s;
      sst::TableBuilder b(f.get());
      MemTable::Iter it(flushing.get());
      std::string ikey;
      for (it.SeekToFirst(); it.Valid(); it.Next()) {
        ikey.clear();
        AppendInternalKey(&ikey, it.user_key(), it.tag());
        b.Add(Slice(ikey), it.value());
      }
      // THE TOMBSTONES, ASCENDING BY START. The memtable holds them in
      // submission order because it queries them by scanning; the BLOCK
      // requires ascending starts, because it is searched. Sorting here rather
      // than keeping the memtable sorted puts the cost on the flush, which
      // happens once per memtable, instead of on every Write.
      //
      // Ties broken by sequence so the order is total: two tombstones may share
      // a start when their sequences differ, and the block refuses only a
      // shared start AND a shared tag.
      std::vector<MemRange> ranges = flushing->Ranges();
      std::sort(ranges.begin(), ranges.end(),
                [](const MemRange& a, const MemRange& c) {
                  if (a.start != c.start) return a.start < c.start;
                  return a.seq < c.seq;
                });
      for (const MemRange& r : ranges) {
        const uint64_t tag = MakeTag(r.seq, ValueType::kDeletion);
        if (r.end_unbounded) {
          b.AddUnboundedRangeTombstone(Slice(r.start), tag);
        } else {
          b.AddRangeTombstone(Slice(r.start), Slice(r.end), tag);
        }
      }
      s = b.Finish();
      if (!s.ok()) return s;
      meta.file_bytes = b.file_size();
      meta.smallest = b.smallest().ToString();
      meta.largest = b.largest().ToString();
      meta.largest_seq = b.largest_seq();
      s = f->Sync();
      if (!s.ok()) return s;
      s = f->Close();
      if (!s.ok()) return s;
    }

    // 2. the directory, so the table's NAME is durable.
    s = SyncDir();
    if (!s.ok()) return s;

    // 3. the manifest edit, then its own Sync (AppendGroup does both).
    //
    // ONE GROUP, and it must be one group: the table becoming live and the WALs
    // it covers becoming obsolete are the same fact. A crash between them
    // would either lose the table or lose the WAL, and the whole ordering
    // exists so that neither is possible.
    {
      sst::ManifestEdit add;
      add.kind = sst::EditKind::kAddTable;
      add.table = meta;
      // The table becoming live and the WALs it covers ceasing to be are ONE
      // FACT, so they are one group.
      std::vector<sst::ManifestEdit> group;
      group.push_back(add);
      {
        wal::DbLock lock(mu_);
        for (uint64_t n : mstate_.wals) {
          if (n >= new_wal_number) continue;
          sst::ManifestEdit drop;
          drop.kind = sst::EditKind::kDeleteWal;
          drop.number = n;
          group.push_back(drop);
        }
      }
      s = manifest_->AppendGroup(group);
      if (!s.ok()) return s;
    }

    // 4. the directory again.
    s = SyncDir();
    if (!s.ok()) return s;

    // The table and the edge naming it are both durable, so everything the
    // table holds now survives a crash. ONLY NOW may the watermark rise past
    // what the retired WAL ever promised.
    std::shared_ptr<sst::Table> opened;
    s = sst::Table::Open(env_, path, table_number, &opened);
    if (!s.ok()) return s;
    {
      wal::DbLock lock(mu_);
      l0_.insert(l0_.begin(), opened);  // newest first; a flush lands at L0
      mstate_.tables[meta.number] = meta;
      for (auto it = mstate_.wals.begin(); it != mstate_.wals.end();) {
        it = (*it < new_wal_number) ? mstate_.wals.erase(it) : std::next(it);
      }
      imm_.reset();
      if (roll_seq > flushed_through_) flushed_through_ = roll_seq;
    }

    // 5. only now: every WAL the manifest has declared obsolete, and then the
    //    directory. Sweeping the whole directory rather than deleting the one
    //    file just retired folds in B2-D5 candidate (c): a crash before this
    //    point leaks a file that the NEXT flush removes, rather than one that
    //    nothing removes.
    const Status closed = retiring->Close();
    if (!closed.ok()) return closed;
    (void)retired_number;
    std::vector<std::string> children;
    s = env_->GetChildren(dir_, &children);
    if (!s.ok()) return s;
    std::sort(children.begin(), children.end());  // never iterate unsorted
    for (const std::string& name : children) {
      uint64_t n = 0;
      if (!ParseWalNumber(name, &n)) continue;
      if (n >= new_wal_number) continue;
      s = env_->DeleteFile(dir_ + "/" + name);
      if (!s.ok()) return s;
    }
    return SyncDir();
  }

  // NNNNNN.log, and nothing else. The engine names its own files, so this is a
  // parse of a name this process wrote rather than of arbitrary input -- but it
  // still refuses anything that is not exactly the shape, because a directory
  // holds files other programs put there too.
  static bool ParseWalNumber(const std::string& name, uint64_t* out) {
    if (name.size() != 10 || name.compare(6, 4, ".log") != 0) return false;
    uint64_t n = 0;
    for (std::size_t i = 0; i < 6; ++i) {
      if (name[i] < '0' || name[i] > '9') return false;
      n = n * 10 + static_cast<uint64_t>(name[i] - '0');
    }
    *out = n;
    return true;
  }

  Status SyncDir() const {
    DirectoryPtr d;
    Status s = env_->NewDirectory(dir_, &d);
    if (!s.ok()) return s;
    s = d->Sync();
    const Status closed = d->Close();
    return s.ok() ? closed : s;
  }

  Env* env_;
  std::string dir_;
  wal::Caps caps_;
  mutable std::mutex mu_;
  std::shared_ptr<MemTable> mem_;
  std::shared_ptr<MemTable> imm_;
  std::unique_ptr<wal::Wal> wal_;
  wal::SeqNum seq_ = 0;
  std::unique_ptr<sst::Manifest> manifest_;
  sst::ManifestState mstate_;
  std::vector<std::shared_ptr<sst::Table>> l0_;  // newest first
  std::vector<std::shared_ptr<sst::Table>> l1_;  // ascending, non-overlapping
  // The highest sequence a DURABLE table holds. See DurableSeq for why the
  // watermark is a maximum over this and the live WAL rather than the WAL alone.
  wal::SeqNum flushed_through_ = 0;
  bool closed_ = false;
  bool compacting_ = false;
  std::atomic<bool> in_sync_{false};
  // Tables the manifest no longer names, whose files are still on disk because
  // a snapshot or iterator is still reading them. Guarded by `mu_`.
  std::vector<std::shared_ptr<sst::Table>> obsolete_;
  std::shared_ptr<SnapshotRegistry> snapshots_ =
      std::make_shared<SnapshotRegistry>();
};

}  // namespace

Status DB::Open(Env* env, const std::string& dir, const wal::Caps& caps,
                std::unique_ptr<DB>* out) {
  // THE LOCK IS TAKEN HERE AND HELD ACROSS BOTH HALVES. B1 had recovery take
  // it, recover, and release it. B2 must read the MANIFEST under the same lock,
  // because the manifest is what supplies recovery's file number and its
  // sequence floor -- and a function that both locks and recovers cannot be
  // composed with one that has to run inside the lock.
  FileLockPtr lock;
  Status s = env->LockFile(dir + "/LOCK", &lock);
  if (!s.ok()) return s;
  auto unlock = [&](Status result) {
    (void)env->UnlockFile(std::move(lock));
    return result;
  };

  sst::ManifestState mstate;
  std::vector<std::shared_ptr<sst::Table>> tables;  // newest first
  std::unique_ptr<sst::Manifest> manifest;
  s = sst::Manifest::Open(env, dir, &mstate, &tables, &manifest);
  if (!s.ok()) return unlock(s);

  // S: the highest sequence the SSTables hold. Recovery skips everything at or
  // below it, and the partition invariant is stated against it.
  wal::SeqNum covered = 0;
  for (const auto& entry : mstate.tables) {
    if (entry.second.largest_seq > covered) covered = entry.second.largest_seq;
  }

  const uint64_t fresh_wal = mstate.next_file_number;
  wal::RecoverOptions options;
  options.next_file_number = fresh_wal;
  options.covered_through = covered;
  options.named_wals.assign(mstate.wals.begin(), mstate.wals.end());
  wal::RecoveryResult r;
  s = wal::Recover(env, dir, caps, options, &r);
  if (!s.ok()) return unlock(s);

  // THE FRESH WAL IS NAMED AFTER Recover CREATED IT AND BEFORE ANYTHING IS
  // WRITTEN TO IT. A crash in that window leaves an empty unnamed file, which
  // the next Open proves empty and removes. A crash before the counter is
  // recorded hands the same number out again, which truncates that same empty
  // file -- harmless for exactly the same reason.
  {
    sst::ManifestEdit bump;
    bump.kind = sst::EditKind::kNextFileNumber;
    bump.number = fresh_wal + 1;
    sst::ManifestEdit add_wal;
    add_wal.kind = sst::EditKind::kAddWal;
    add_wal.number = fresh_wal;
    s = manifest->AppendGroup({bump, add_wal});
    if (!s.ok()) return unlock(s);
    mstate.next_file_number = fresh_wal + 1;
    mstate.wals.insert(fresh_wal);
  }

  // ORPHAN FILES, REMOVED. A table is created, synced, dirsynced and only then
  // named, so an unnamed .sst is one a crash caught before its manifest edit --
  // nothing refers to it and nothing can. Deleting it at Open is B2-D5
  // candidate (c) folded in, and it is the same argument the retired manifests
  // and the obsolete WALs get: a crash leaks a file the NEXT Open removes,
  // rather than one nothing removes.
  {
    std::vector<std::string> children;
    if (env->GetChildren(dir, &children).ok()) {
      std::sort(children.begin(), children.end());  // never iterate unsorted
      for (const std::string& name : children) {
        if (name.size() < 5 || name.compare(name.size() - 4, 4, ".sst") != 0) continue;
        uint64_t n = 0;
        bool digits = !name.empty();
        for (std::size_t i = 0; i + 4 < name.size(); ++i) {
          if (name[i] < '0' || name[i] > '9') { digits = false; break; }
          n = n * 10 + static_cast<uint64_t>(name[i] - '0');
        }
        if (!digits) continue;
        if (mstate.tables.find(n) != mstate.tables.end()) continue;
        (void)env->DeleteFile(dir + "/" + name);
      }
    }
  }

  // THE SPLIT COMES FROM THE MANIFEST, NOT FROM THE FILES. A table's level is
  // recorded, never inferred from its key range: two tables can be
  // non-overlapping and still both belong at L0, and inferring would make the
  // structure a function of the data rather than of what the engine decided.
  std::vector<std::shared_ptr<sst::Table>> l0;  // newest first, as handed over
  std::vector<std::shared_ptr<sst::Table>> l1;
  for (const auto& t : tables) {
    const auto it = mstate.tables.find(t->number());
    RIFT_CHECK(it != mstate.tables.end());  // it was opened BECAUSE it is named
    (it->second.level == 0 ? l0 : l1).push_back(t);
  }
  // L1 ascending by user key. VerifyL1IsARun has already refused an Open where
  // that order is ambiguous, so this sort has a unique answer.
  std::sort(l1.begin(), l1.end(),
            [](const std::shared_ptr<sst::Table>& a,
               const std::shared_ptr<sst::Table>& b) {
              return ExtractUserKey(Slice(a->check().smallest_key))
                         .compare(ExtractUserKey(Slice(b->check().smallest_key))) < 0;
            });

  for (std::size_t i = 0; i + 1 < l1.size(); ++i) {
    // The same invariant as the install path's, checked on the way in: a
    // manifest naming an unbounded tombstone anywhere but the last file of the
    // run describes an L1 whose reads would silently miss it.
    if (l1[i]->check().unbounded_end) {
      return unlock(Status::Corruption(
          "level 1 file " + std::to_string(l1[i]->number()) +
          " holds a range tombstone with no upper bound and is not the last of "
          "the run"));
    }
  }

  out->reset(new DBImpl(env, dir, caps,
                        std::shared_ptr<MemTable>(std::move(r.table)),
                        std::move(r.wal), r.recovered_seq, std::move(manifest),
                        std::move(mstate), std::move(l0), std::move(l1),
                        covered));
  return unlock(Status::Ok());
}

}  // namespace rift
