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

#include "arena.h"
#include "slice.h"
#include "status.h"
#include "tower.h"

namespace rift {

// LevelDB's convention, and it is load-bearing for ordering: a deletion and a
// value at the same sequence must sort deterministically, and the type is the
// low byte of the tag so they do.
enum class ValueType : uint8_t {
  kDeletion = 0,
  kValue = 1,
};

using SeqNum = uint64_t;

// internal_key = user_key || ((seq << 8) | value_type) as u64 little-endian
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
// compaction filter, and does not need to, because version GC is A5's job on
// the Go side.
class MemTable {
 public:
  MemTable() = default;

  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  // Inserts one version. Makes NO Env call and performs no I/O; it touches the
  // arena and the skiplist and nothing else.
  void Add(SeqNum seq, ValueType type, Slice user_key, Slice value);

  // Returns the newest version at or below `snapshot`. kNotFound covers both
  // "no such key" and "the newest visible version is a deletion", which is what
  // the frozen interface's ErrNotFound means.
  Status Get(Slice user_key, SeqNum snapshot, std::string* value) const;

  std::size_t MemoryUsage() const;
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

  mutable std::mutex mu_;
  Arena arena_;
  Node* head_ = nullptr;
  int height_ = 1;
  std::size_t count_ = 0;
};

}  // namespace rift

#endif  // RIFT_MEMTABLE_MEMTABLE_H_
