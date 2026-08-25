// A bump allocator whose whole lifetime is the memtable's.
//
// B1-D6a: nodes and key bytes come from here and the whole arena dies with the
// memtable. Two reasons, and neither is speed.
//
// EXACT MEMORY ACCOUNTING. B2's flush threshold needs to know how large the
// memtable is, and a general allocator cannot answer that cheaply or exactly.
// Here the answer is a counter.
//
// NO PER-NODE FREE PATH TO GET WRONG UNDER A KILL. Nothing in the memtable is
// ever individually freed, so there is no free that can race a reader, no
// use-after-free reachable from a kill point, and no destructor ordering to
// reason about when the rig destroys a DB mid-operation.
#ifndef RIFT_MEMTABLE_ARENA_H_
#define RIFT_MEMTABLE_ARENA_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rift {

class Arena {
 public:
  Arena() = default;
  ~Arena();

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  // Returns `bytes` of storage aligned for any object this engine allocates.
  char* Allocate(std::size_t bytes);

  // Every byte handed out plus every byte of block overhead. Exact, not an
  // estimate: B2 makes a flush decision from it.
  std::size_t MemoryUsage() const { return usage_; }

 private:
  static constexpr std::size_t kBlockBytes = 4096;
  static constexpr std::size_t kAlign = sizeof(void*) > 8 ? sizeof(void*) : 8;

  char* AllocateNewBlock(std::size_t bytes);

  char* head_ = nullptr;
  std::size_t remaining_ = 0;
  std::size_t usage_ = 0;
  std::vector<char*> blocks_;
};

}  // namespace rift

#endif  // RIFT_MEMTABLE_ARENA_H_
