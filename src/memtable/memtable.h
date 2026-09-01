// The memtable: an arena-allocated skiplist under the DB mutex.
//
// B1-D6c, RULED BY ANSH: A LOCK. Not a lock-free single-writer/multi-reader
// skiplist, which is what LevelDB has and what this session proposed.
//
// The recommendation was wrong in a specific and recorded way. Amendment A6
// governs -- the simplest correct thing wins v1 and the faster thing is a
// recorded upgrade path -- and this session applied A6 to compaction policy
// three sections earlier and then violated it here. Uneven application of a
// principle is more dangerous than not having the principle, because the
// citation makes it look considered.
//
// What made the question look open: the frozen interface has Apply running on
// the node loop while a separate thread owns the blocking Sync, so the engine
// IS called from two threads and must be internally synchronized. What does not
// follow is that the memtable needs to be lock-free. Apply is non-blocking BY
// CONTRACT, not by parallelism -- section 8.3's invariant is that it makes no
// Env call, which a mutex does not threaten.
//
// A lock-free structure would spend this project's scarcest resource, C++
// correctness under fault injection, to buy throughput no measurement has asked
// for, and its failure mode is the one the project exists to eliminate: a bug
// that appears on one machine, at one core count, one run in ten thousand, and
// does not replay.
//
// THE MEASUREMENT THAT WOULD REOPEN IT, so the upgrade path is a threshold
// rather than a mood: B5's standalone numbers showing the memtable mutex is the
// bottleneck -- a readrandom mix whose throughput scales sublinearly with
// reader threads while the same workload against engine/model does not, with
// lock contention attributed BY PROFILE rather than inferred. Absent that
// number, the lock stays.
//
// THE FAILURE MODE THE RULING OPENED (section 0.1 principle 1): a Sync holding
// the DB mutex across an fsync blocks every reader for the fsync's duration.
// That is a consequence of this decision, invisible until B5's benchmarks
// looked inexplicably bad. Section 8.3's mutex-depth guard closes it and lands
// with the WAL at B1.6; this file's job is to make sure the mutex is only ever
// held around memory.
#ifndef RIFT_MEMTABLE_MEMTABLE_H_
#define RIFT_MEMTABLE_MEMTABLE_H_

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "arena.h"
#include "internal_key.h"
#include "range_tombstone.h"
#include "slice.h"
#include "status.h"
#include "tower.h"

namespace rift {

// ValueType, SeqNum, MakeTag and the internal key layout now live in
// internal_key.h. B2 gave the layout a SECOND holder -- the SSTable -- and a
// second holder is the moment a format defined in one file's private helpers
// becomes two formats that agree until they do not.
//
// Ordering: user key ASCENDING by memcmp, then seq DESCENDING, so the newest
// version of a key sorts first and a snapshot read is one seek.
//
// Multiple versions per key are REQUIRED -- NewSnapshot pins a sequence and a
// read through it must skip newer versions -- so the memtable is append-only
// and never overwrites.
//
// THE COMPARATOR IS BYTEWISE AND IS NOT PLUGGABLE IN v1. A pluggable comparator
// is the door through which the storage engine learns what a key MEANS, and A5
// puts MVCC timestamps inside keys. Ruling 2 says the engine never interprets
// time; a fixed bytewise comparison makes that uncompilable rather than
// remembered. The cost is named: B3 cannot implement a timestamp-aware
// compaction filter, and does not need to, because version GC belongs to the
// layer above this engine.
// One range deletion, as a memtable holds it: owning its bounds, because the
// batch that submitted them is gone by the time anyone reads them.
struct MemRange {
  std::string start;
  std::string end;              // unused when `end_unbounded`
  bool end_unbounded = false;
  SeqNum seq = 0;

  // ONE PREDICATE, AND IT IS `sst::RangeTombstone::Covers`. A third
  // implementation of the half-open test is a boundary bug waiting for a
  // boundary key -- the memtable, the table and the compaction must agree
  // about which keys a range deletes, so they share the answer.
  //
  // (The HARNESS's copy in `rig/version_model.h` is a deliberate FOURTH, and
  // the reason it must stay separate is written there: B3-D2b forbids the
  // checker deriving its expectation from the engine.)
  bool Covers(Slice user_key) const {
    sst::RangeTombstone t;
    t.start = Slice(start);
    t.end = Slice(end);
    t.end_unbounded = end_unbounded;
    return t.Covers(user_key);
  }
};

class MemTable {
  // Declared first so Iter can name it; defined below.
  struct Node;
  friend class Iter;

 public:
  MemTable() = default;

  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  // Inserts one version. Makes NO Env call and performs no I/O; it touches the
  // arena and the skiplist and nothing else.
  void Add(SeqNum seq, ValueType type, Slice user_key, Slice value);

  // A RANGE DELETION, held as one entry rather than expanded into one point
  // delete per live key. `[start, end)`, and `end_unbounded` means no upper
  // bound -- B3-Q4, and `end` is then unused.
  //
  // It is NOT in the skiplist. A range tombstone has no user key of its own,
  // and giving it one would put a fictional member in the key space every
  // cursor walks.
  void AddRangeTombstone(SeqNum seq, Slice start, Slice end, bool end_unbounded);

  // The sequence of the newest range tombstone at or below `snapshot` that
  // covers `user_key`, or 0 if none does. Sequences start at 1, so 0 is a safe
  // "none" -- the same reasoning that lets `range_offset == 0` mean "no range
  // block".
  SeqNum NewestCovering(Slice user_key, SeqNum snapshot) const;

  // Every tombstone this memtable holds, in submission order. The flush reads
  // it; nothing else should.
  std::vector<MemRange> Ranges() const;

  // Returns the newest version at or below `snapshot`. kNotFound covers both
  // "no such key" and "the newest visible version is a deletion", which is what
  // the frozen interface's ErrNotFound means.
  Status Get(Slice user_key, SeqNum snapshot, std::string* value) const;

  std::size_t MemoryUsage() const;
  std::size_t RangeCount() const;
  std::size_t Count() const;

  // A hash of the memtable's SHAPE, not of its contents alone: every entry's
  // tower height participates.
  //
  // It is the C++ analogue of Track A's trace hash and it catches three things
  // for one test -- ambient randomness, uninitialized bytes reaching a
  // structure, and any height source that is not a pure function of the key.
  // A PRNG height source, or one derived from an address or from uninitialized
  // memory, all produce a digest that differs between two runs of the same
  // workload; a mapping change produces one that differs from the pinned value.
  uint64_t StructuralDigest() const;

  // A cursor over internal keys, in (user key ascending, seq descending) order.
  //
  // EVERY POSITIONING CALL TAKES THE DB MUTEX. That is coarse and it is
  // B1-D6c's ruling carried through honestly: LevelDB's iterator holds no lock
  // because its skiplist is lock-free, and ours is not. Holding the lock ACROSS
  // a caller's loop would be worse -- it would put an unbounded user-controlled
  // span inside the mutex -- so the cost is one acquisition per step.
  //
  // Key() and Value() return pointers into the arena, which is safe here for a
  // reason that expires: B1 has no flush, so nodes are never freed and the
  // arena outlives every iterator. B2 must revisit this the moment a memtable
  // can be retired.
  //
  // Prev() re-descends from the head rather than following a back pointer,
  // which is O(log n) instead of O(1). A doubly-linked skiplist would double
  // the pointer writes on the insert path to speed up a direction B1 has no
  // measurement for.
  class Iter {
   public:
    explicit Iter(const MemTable* table) : table_(table) {}

    bool Valid() const { return node_ != nullptr; }
    void SeekToFirst();
    void SeekToLast();
    // Positions at the first entry >= (user_key, tag).
    void Seek(Slice user_key, uint64_t tag);
    void Next();
    void Prev();

    Slice user_key() const;
    uint64_t tag() const;
    Slice value() const;

   private:
    const MemTable* table_;
    Node* node_ = nullptr;
  };

 private:
  struct Node {
    const char* entry;  // in the arena: u32 klen, key+tag, u32 vlen, value
    int height;
    Node** next;        // in the arena: `height` entries
  };

  Node* NewNode(const char* entry, int height);
  static Slice EntryUserKey(const char* entry);
  static uint64_t EntryTag(const char* entry);
  static Slice EntryValue(const char* entry);
  // Compares (user_key ascending, tag descending).
  static int CompareEntry(const char* entry, Slice user_key, uint64_t tag);
  Node* FindGreaterOrEqual(Slice user_key, uint64_t tag, Node** prev) const;
  Node* FindLessThan(Slice user_key, uint64_t tag) const;
  Node* FindLast() const;

  // Guarded by `mu_` like everything else here. A vector and a linear scan:
  // range tombstones are rare beside point entries, and the memtable is bounded
  // by the flush threshold. THE MEASUREMENT THAT WOULD MOVE IT is a workload
  // whose range deletions are dense enough for the scan to show up beside the
  // skiplist descent -- B5's numbers, attributed by profile rather than
  // inferred.
  SeqNum NewestCoveringLocked(Slice user_key, SeqNum snapshot) const;

  std::vector<MemRange> ranges_;
  mutable std::mutex mu_;
  Arena arena_;
  Node* head_ = nullptr;
  int height_ = 1;
  std::size_t count_ = 0;
};

}  // namespace rift

#endif  // RIFT_MEMTABLE_MEMTABLE_H_
