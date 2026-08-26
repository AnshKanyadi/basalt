#include "db.h"

#include <set>

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "check.h"
#include "compaction.h"
#include "env_guard.h"
#include "manifest.h"
#include "merged_iter.h"
#include "recovery.h"
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
  if (lo == l1.size()) return nullptr;
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

// THE EXPANSION HAPPENS AT Apply AND THE WAL RECORDS THE EXPANSION. Section 8.1.
//
// If the WAL recorded the raw DeleteRange, recovery would have to expand it
// again -- AGAINST A STATE RECOVERY IS STILL IN THE MIDDLE OF REBUILDING. The
// expansion is a function of the state at the time it runs, so replay-time
// expansion is correct only if that state provably equals the state at original
// Apply time. It probably does today, for a reason that depends on the WAL's
// start point coinciding exactly with the flush boundary -- a property B2 is
// about to start changing. THAT IS CORRECTNESS BY ARGUMENT, AND THE ARGUMENT
// HAS A MOVING PREMISE. Recording the post-expansion op list makes it
// correctness by construction: recovery replays point deletes, there is nothing
// left to compute, the circularity is gone.
//
// Intra-batch semantics come out right because this walks the ops IN ORDER: at
// a DeleteRange the expansion covers the current state AND keys written earlier
// in the same batch, and a Set after it re-adds the key -- the model's rule
// reproduced.
std::vector<wal::Op> ExpandAndCollapse(const WriteBatch& b, const Version& v,
                                       wal::SeqNum snapshot,
                                       std::vector<std::string>* owned) {
  // key -> (kind, value). std::map so the result is sorted by key, which is
  // what B1-D10's collapse costs and what makes "no two memtable entries share
  // a (user_key, seq) pair" assertable.
  std::map<std::string, std::pair<wal::OpKind, std::string>> pending;

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
        // Everything written EARLIER IN THIS BATCH that falls inside.
        for (auto it = pending.begin(); it != pending.end(); ++it) {
          if (InRange(Slice(it->first), start, e.end)) {
            it->second = {wal::OpKind::kDelete, std::string()};
          }
        }
        // And everything live in THE MERGED VIEW -- the memtable, the memtable
        // being flushed, and every SSTable. B2-D7 section 8: at B1 this read
        // the memtable, and reading only the memtable now would make a
        // DeleteRange silently miss every key that had been flushed.
        //
        // Reads memory, not Env. This is the operation most likely to violate
        // "Apply performs no I/O", which is why that assertion is re-made
        // against this path -- and why table.h holds whole SSTables resident.
        MergedIter it;
        v.Build(&it);
        if (start.bounded()) {
          it.Seek(start.key(), (snapshot << 8) | 1);
        } else {
          it.SeekToFirst();
        }
        std::string last_user_key;
        bool have_last = false;
        for (; it.Valid(); it.Next()) {
          const Slice k = it.user_key();
          if (e.end.bounded() && k.compare(e.end.key()) >= 0) break;
          if (have_last && k == Slice(last_user_key)) continue;  // older version
          last_user_key = k.ToString();
          have_last = true;
          if ((it.tag() >> 8) > snapshot) continue;
          if ((it.tag() & 0xff) == 0) continue;  // already a deletion
          if (!InRange(k, start, e.end)) continue;
          if (pending.find(last_user_key) == pending.end()) {
            pending[last_user_key] = {wal::OpKind::kDelete, std::string()};
          }
        }
        break;
      }
    }
  }

  owned->clear();
  owned->reserve(pending.size() * 2);
  std::vector<wal::Op> out;
  out.reserve(pending.size());
  for (const auto& kv : pending) {
    owned->push_back(kv.first);
    owned->push_back(kv.second.second);
  }
  std::size_t i = 0;
  for (const auto& kv : pending) {
    wal::Op op;
    op.kind = kv.second.first;
    op.key = Slice((*owned)[i]);
    if (op.kind == wal::OpKind::kSet) op.value = Slice((*owned)[i + 1]);
    out.push_back(op);
    i += 2;
  }
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
        if ((it_.tag() & 0xff) != 0) {  // a live value, not a deletion
          key_ = k;
          value_ = it_.value().ToString();
          valid_ = true;
          return true;
        }
        SkipVersionsOf(k);  // a deletion hides this key entirely
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
        if (SettleOnCurrentKey(k) && (it_.tag() & 0xff) != 0) {
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
  MergedIter mem_only;
  mem_only.AddMemTable(v.mem.get());
  if (v.imm != nullptr) mem_only.AddMemTable(v.imm.get());
  mem_only.Seek(key, MakeTag(snapshot, ValueType::kValue));
  if (mem_only.Valid() && mem_only.user_key() == key) {
    if (TypeOfTag(mem_only.tag()) == ValueType::kDeletion) return Status::NotFound("");
    *value = mem_only.value().ToString();
    return Status::Ok();
  }
  for (const auto& t : v.l0) {
    bool deleted = false;
    bool filtered = false;
    const Status s = t->Get(key, snapshot, value, &deleted, &filtered);
    if (s.ok()) return s;
    if (deleted) return Status::NotFound("");
  }
  // L1 IS ONE LOOKUP, NOT |L1| LOOKUPS. Every L0 file must be asked because
  // they overlap; at most one L1 file can hold the key, so the read
  // amplification B3-D8 records is |L0| + 1 rather than |L0| + |L1|.
  if (const sst::Table* t = L1FileFor(v.l1, key)) {
    bool deleted = false;
    bool filtered = false;
    const Status s = t->Get(key, snapshot, value, &deleted, &filtered);
    if (s.ok()) return s;
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
      ops = ExpandAndCollapse(b, CurrentVersionLocked(), assigned - 1, &owned);
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
            RIFT_UNREACHABLE("DeleteRange survived expansion");
        }
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
      observable = snapshots_->Live();
      // `S` = the live snapshots PLUS the sequence a caller reads at with no
      // snapshot. Without the second member the newest version of every key
      // would be droppable, which is the one drop no compaction may make.
      if (observable.empty() || observable.back() != seq_) observable.push_back(seq_);
      std::sort(observable.begin(), observable.end());
      observable.erase(std::unique(observable.begin(), observable.end()),
                       observable.end());
    }
    const Status s = DoCompact(in_l0, in_l1, keep_l1, observable);
    { wal::DbLock lock(mu_); compacting_ = false; }
    return s;
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
        const Status s = CloseCurrent();
        if (!s.ok()) return s;
      }
      if (builder_ == nullptr) {
        const Status s = OpenNext();
        if (!s.ok()) return s;
      }
      builder_->Add(internal_key, value);
      return builder_->status();
    }

    // Finishes whatever is open. Called once, after RunCompaction returns.
    Status Finish() { return builder_ == nullptr ? Status::Ok() : CloseCurrent(); }

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
    Status CloseCurrent() {
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
  };

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
    for (const auto& t : in_l0) {
      bound += t->check().entries;
      if (t->check().largest_seq > pin_seq) pin_seq = t->check().largest_seq;
      merge.AddTable(t.get());
    }
    for (const auto& t : in_l1) {
      bound += t->check().entries;
      if (t->check().largest_seq > pin_seq) pin_seq = t->check().largest_seq;
      run.push_back(t.get());
    }
    merge.AddRun(std::move(run));

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
                               &roller, &stats);
      if (!s.ok()) return s;
      s = roller.Finish();
      if (!s.ok()) return s;
    }
    const std::vector<sst::TableMeta>& outputs = roller.outputs();

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
      for (uint64_t n : gone) mstate_.tables.erase(n);
      for (const sst::TableMeta& meta : outputs) mstate_.tables[meta.number] = meta;
    }

    // 5. only now: the input files.
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
    for (const auto& t : in_l0) {
      s = env_->DeleteFile(sst::TablePath(dir_, t->number()));
      if (!s.ok()) return s;
    }
    for (const auto& t : in_l1) {
      s = env_->DeleteFile(sst::TablePath(dir_, t->number()));
      if (!s.ok()) return s;
    }
    return SyncDir();
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

  out->reset(new DBImpl(env, dir, caps,
                        std::shared_ptr<MemTable>(std::move(r.table)),
                        std::move(r.wal), r.recovered_seq, std::move(manifest),
                        std::move(mstate), std::move(l0), std::move(l1),
                        covered));
  return unlock(Status::Ok());
}

}  // namespace rift
