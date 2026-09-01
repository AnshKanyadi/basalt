// The frozen `engine/` interface, met exactly.
//
// "Correct" for B4 means BYTE-IDENTICAL TO engine/model, so this must meet the
// interface frozen at A0.5 exactly -- not approximately. This step's real
// acceptance test is B4's differential rig; the suite here exists to make B4's
// failures debuggable, not to substitute for them.
//
// ---------------------------------------------------------------------------
// WHERE C++ CANNOT EXPRESS THE FROZEN SHAPE. Four places, reported here rather
// than adapted quietly. Two are already ruled; two are consequences of the
// language and are being recorded for the first time.
//
// 1. OnDurable(func(SeqNum)) IS ABSENT. Ruled (DR-11, section 7.1): the engine
//    does not call out. An embedder's per-engine poller owns the blocking
//    Sync() below and delivers the durability event wherever that embedder
//    wants it delivered, on whatever thread it wants it delivered on. What the
//    engine exposes instead is Sync(), which is strictly more primitive: a
//    callback can be built from a poller and a poller cannot be built from a
//    callback.
//
// 2. Apply's `sync bool` IS ABSENT. Ruled (section 7.1): the flag's POLICY --
//    how eagerly the poller wakes -- is a B5 decision about the PAIR, not a B1
//    decision about the engine. Write() never blocks on I/O whatever the caller
//    wanted, so a flag that promised otherwise would be a flag the engine
//    cannot honour.
//
// 3. A NIL BOUND HAS NO Slice EQUIVALENT, and the frozen interface
//    depends on the distinction: InRange treats a nil bound as unbounded, and
//    an EMPTY KEY IS A VALID KEY in this engine. `Slice()` cannot mean both
//    "the empty key" and "no bound". So bounds are a `Bound`, which is
//    explicitly one or the other. DeleteRange(Unbounded, Unbounded) is
//    section 8.2's clear-everything case; DeleteRange(At(""), At("")) is empty
//    and deletes nothing, and those must not be the same call.
//
// 4. ApproximateDiskBytes SCANS. The frozen comment says "Approximate is in the
//    name because the C++ engine answers from table metadata rather than by
//    scanning" -- and it does not. There are tables now and there is metadata
//    now (TableMeta carries file_bytes), and this call consults neither: it
//    walks the merged view over the current version and sums the key and value
//    bytes visible at the current sequence. So the answer is exact, O(n) in the
//    live entries in range, and is LIVE LOGICAL BYTES rather than bytes on
//    disk -- it counts no block framing, no filter, no footer, and nothing an
//    overwritten or deleted version still occupies in a file it has not been
//    compacted out of. Stated because it is what a caller gets today rather
//    than what the name promises.
#ifndef BASALT_DB_H_
#define BASALT_DB_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "basalt/caps.h"
#include "basalt/env.h"
#include "basalt/format.h"
#include "basalt/slice.h"
#include "basalt/status.h"

namespace basalt {

// A bound that is either a key or UNBOUNDED. See divergence 3 above.
class Bound {
 public:
  static Bound Unbounded() { return Bound(); }
  static Bound At(Slice key) { return Bound(key); }

  bool bounded() const { return bounded_; }
  Slice key() const { return Slice(key_); }

 private:
  Bound() = default;
  explicit Bound(Slice k) : bounded_(true), key_(k.ToString()) {}
  bool bounded_ = false;
  std::string key_;
};

// Half-open [start, end), agreeing with engine.InRange by construction.
bool InRange(Slice key, const Bound& start, const Bound& end);

// An ordered set of writes applied atomically.
//
// IT OWNS ITS BYTES, like the frozen Batch, and for the frozen Batch's stated
// reason: retaining caller buffers makes an engine's behaviour depend on
// whether a caller happens to reuse one, which is a class of bug that
// reproduces on one machine and not another.
class WriteBatch {
 public:
  struct Entry {
    wal::OpKind kind = wal::OpKind::kSet;
    std::string key;
    std::string value;  // kSet only
    Bound end = Bound::Unbounded();  // kDeleteRange only
  };

  WriteBatch& Set(Slice key, Slice value);
  WriteBatch& Delete(Slice key);
  // start and end are half-open; either may be Unbounded.
  WriteBatch& DeleteRange(const Bound& start, const Bound& end);

  const std::vector<Entry>& ops() const { return ops_; }
  std::size_t Len() const { return ops_.size(); }
  bool Empty() const { return ops_.empty(); }
  void Reset() { ops_.clear(); }

 private:
  std::vector<Entry> ops_;
};

struct IterOptions {
  Bound lower = Bound::Unbounded();
  Bound upper = Bound::Unbounded();
};

// Walks keys in byte order. Key() and Value() are valid only until the next
// positioning call, which is what lets the engine hand back pointers into the
// arena rather than copying every pair out to the caller.
class Iterator {
 public:
  virtual ~Iterator() = default;
  virtual bool SeekGE(Slice key) = 0;
  virtual bool SeekLT(Slice key) = 0;
  virtual bool First() = 0;
  virtual bool Last() = 0;
  virtual bool Next() = 0;
  virtual bool Prev() = 0;
  virtual bool Valid() const = 0;
  virtual Slice Key() const = 0;
  virtual Slice Value() const = 0;
  virtual Status Error() const = 0;
  virtual Status Close() = 0;
};

// A pinned view. Reads through it see the state as of the moment it was taken.
//
// "IT HOLDS ITS VERSION AGAINST COMPACTION UNTIL IT IS Closed" IS REAL NOW, and
// it is held by two mechanisms rather than one. The snapshot keeps shared
// pointers to the memtable and the tables it was taken over, so it goes on
// reading through them after a flush or a compaction drops the DB's own
// references; and its sequence is registered in the live set the drop rule
// consults, so a compaction may not drop a version this snapshot can still
// read. The second is not redundant with the first: pinning alone would rest on
// a whole SSTable staying resident after its file is unlinked, which is an
// argument with a moving premise.
//
// Close() and destruction both release it, and exactly one of them counts.
class Snapshot {
 public:
  virtual ~Snapshot() = default;
  virtual Status Get(Slice key, std::string* value) const = 0;
  virtual std::unique_ptr<Iterator> NewIter(const IterOptions& o) const = 0;
  virtual Status Close() = 0;
};

class DB {
 public:
  static Status Open(Env* env, const std::string& dir, const wal::Caps& caps,
                     std::unique_ptr<DB>* out);
  virtual ~DB() = default;

  // Makes b visible to subsequent reads immediately and returns the sequence at
  // which it became visible. NEVER BLOCKS ON I/O -- asserted by the Env-call
  // counter, not promised.
  virtual Status Write(const WriteBatch& b, wal::SeqNum* seq) = 0;

  // The highest SeqNum guaranteed to survive a crash. Monotone non-decreasing.
  virtual wal::SeqNum DurableSeq() const = 0;

  // Blocking. B5's poller owns this and adapts it to the async durability
  // contract; see divergence 1.
  //
  // SINGLE-CALLER, AND NOW ENFORCED. It was always the contract -- one poller
  // per engine -- and it was never written down here. It is the path that
  // flushes and compacts, so it is the only path that appends to the manifest,
  // and two callers would interleave two groups into one log. A second
  // concurrent caller aborts.
  virtual Status Sync(wal::SeqNum* watermark) = 0;

  virtual Status Get(Slice key, std::string* value) const = 0;
  virtual std::unique_ptr<Iterator> NewIter(const IterOptions& o) const = 0;
  virtual std::unique_ptr<Snapshot> NewSnapshot() = 0;
  virtual Status ApproximateDiskBytes(const Bound& start, const Bound& end,
                                      uint64_t* out) const = 0;

  // Does NOT sync, deliberately. The watermark is the engine's only durability
  // promise; a Close that synced would make clean shutdown a hidden durability
  // event engine/model's Close does not have, and the two engines would then
  // disagree in precisely the differential rig. The consequence is a good test:
  // close-then-reopen must be indistinguishable from kill-then-reopen.
  virtual Status Close() = 0;
};

}  // namespace basalt

#endif  // BASALT_DB_H_
