#include "db.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <utility>

#include "check.h"
#include "env_guard.h"
#include "recovery.h"
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
std::vector<wal::Op> ExpandAndCollapse(const WriteBatch& b, const MemTable& table,
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
        // And everything live in the memtable. Reads memory, not Env: this is
        // the operation most likely to violate "Apply performs no I/O", which
        // is why that assertion is re-made against this path.
        MemTable::Iter it(&table);
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
  IterImpl(const MemTable* table, wal::SeqNum snapshot, IterOptions o)
      : it_(table), snapshot_(snapshot), o_(std::move(o)) {}

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

  bool AdvanceToVisible() {
    while (it_.Valid()) {
      const std::string k = it_.user_key().ToString();
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
    while (it_.Valid()) {
      const std::string k = it_.user_key().ToString();
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

  MemTable::Iter it_;
  wal::SeqNum snapshot_;
  IterOptions o_;
  bool valid_ = false;
  std::string key_;
  std::string value_;
};

class SnapshotImpl final : public Snapshot {
 public:
  SnapshotImpl(const MemTable* table, wal::SeqNum seq) : table_(table), seq_(seq) {}
  Status Get(Slice key, std::string* value) const override {
    return table_->Get(key, seq_, value);
  }
  std::unique_ptr<Iterator> NewIter(const IterOptions& o) const override {
    return std::unique_ptr<Iterator>(new IterImpl(table_, seq_, o));
  }
  Status Close() override { return Status::Ok(); }

 private:
  const MemTable* table_;
  wal::SeqNum seq_;
};

class DBImpl final : public DB {
 public:
  DBImpl(std::unique_ptr<MemTable> table, std::unique_ptr<wal::Wal> w,
         wal::SeqNum seq)
      : table_(std::move(table)), wal_(std::move(w)), seq_(seq) {}

  Status Write(const WriteBatch& b, wal::SeqNum* seq) override {
    std::vector<std::string> owned;
    wal::SeqNum assigned = 0;
    std::vector<wal::Op> ops;
    {
      wal::DbLock lock(mu_);
      if (closed_) return Status::InvalidArgument("Write after Close");
      // +1 per Write INCLUDING EMPTY ONES, identical to engine/model's counter
      // (section 5.3.4). A sequence space that skipped empty batches would put
      // a translation table inside B4's oracle.
      assigned = ++seq_;
      ops = ExpandAndCollapse(b, *table_, assigned - 1, &owned);
    }
    Status s = wal_->Apply(assigned, ops);
    if (!s.ok()) {
      // APPLIES NOTHING, ATOMICALLY. The sequence is consumed either way, which
      // is legal -- the frozen contract requires monotonicity, not density --
      // and is simpler than unwinding a counter two threads can see.
      *seq = assigned;
      return s;
    }
    {
      wal::DbLock lock(mu_);
      for (const wal::Op& op : ops) {
        switch (op.kind) {  // NO default: arm
          case wal::OpKind::kSet:
            table_->Add(assigned, ValueType::kValue, op.key, op.value);
            break;
          case wal::OpKind::kDelete:
            table_->Add(assigned, ValueType::kDeletion, op.key, Slice());
            break;
          case wal::OpKind::kDeleteRange:
            RIFT_UNREACHABLE("DeleteRange survived expansion");
        }
      }
    }
    *seq = assigned;
    return Status::Ok();
  }

  wal::SeqNum DurableSeq() const override { return wal_->DurableSeq(); }

  Status Sync(wal::SeqNum* watermark) override {
    // NO LOCK HELD. Everything below makes Env calls, and the mutex-depth guard
    // would fire if one were.
    return wal_->Sync(watermark);
  }

  Status Get(Slice key, std::string* value) const override {
    wal::SeqNum snap;
    { wal::DbLock lock(mu_); snap = seq_; }
    return table_->Get(key, snap, value);
  }

  std::unique_ptr<Iterator> NewIter(const IterOptions& o) const override {
    wal::SeqNum snap;
    { wal::DbLock lock(mu_); snap = seq_; }
    return std::unique_ptr<Iterator>(new IterImpl(table_.get(), snap, o));
  }

  std::unique_ptr<Snapshot> NewSnapshot() override {
    wal::DbLock lock(mu_);
    return std::unique_ptr<Snapshot>(new SnapshotImpl(table_.get(), seq_));
  }

  Status ApproximateDiskBytes(const Bound& start, const Bound& end,
                              uint64_t* out) const override {
    wal::SeqNum snap;
    { wal::DbLock lock(mu_); snap = seq_; }
    uint64_t total = 0;
    IterOptions o;
    o.lower = start;
    o.upper = end;
    IterImpl it(table_.get(), snap, o);
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
    return wal_->Close();
  }

 private:
  mutable std::mutex mu_;
  std::unique_ptr<MemTable> table_;
  std::unique_ptr<wal::Wal> wal_;
  wal::SeqNum seq_ = 0;
  bool closed_ = false;
};

}  // namespace

Status DB::Open(Env* env, const std::string& dir, const wal::Caps& caps,
                std::unique_ptr<DB>* out) {
  wal::RecoveryResult r;
  Status s = wal::Recover(env, dir, caps, &r);
  if (!s.ok()) return s;
  out->reset(new DBImpl(std::move(r.table), std::move(r.wal), r.recovered_seq));
  return Status::Ok();
}

}  // namespace rift
