// ONE CURSOR OVER THE WHOLE DATABASE: the live memtable, the memtable being
// flushed, and every live SSTable, in internal key order.
//
// It exists so that the two places which walk the database -- the read
// iterator, and `DeleteRange`'s expansion -- do not each learn how many stores
// there are. B1 had one store; B2 has three kinds; B3 changes the number again.
// A merge in one place is a merge that can be wrong in one place.
//
// THE SURFACE IS MemTable::Iter's, EXACTLY. That is deliberate: the two loops
// that walk the database are the most dangerous code in the engine, and giving
// the merge a different shape would have meant editing both of them to
// accommodate a new type rather than changing what they are handed.
//
// IT MAKES NO Env CALL, which is a requirement and not an observation. B2-D7
// has `DeleteRange` expanding at Apply, Apply makes no Env call, and the
// expansion reads through this -- see table.h for why the whole SSTable is
// resident.
//
// DIRECTION SWITCHES RE-SEEK EVERY SOURCE, which is LevelDB's approach and is
// not an optimisation to be tidied away later. A k-way merge only knows which
// source holds the next key in the direction it is currently walking; the other
// sources are positioned for THAT direction and say nothing about the other
// one. Stepping back without re-seeking reads whichever entries happen to be
// under the other cursors, which is wrong in a way that looks like a
// missing key.
//
// The merge itself is a LINEAR SCAN over sources rather than a heap: two
// memtables at most plus the live tables, so k is small and does not vary
// within a run. B3's compaction is where the number of tables stops being
// small, and where this is revisited.
#ifndef BASALT_MERGED_ITER_H_
#define BASALT_MERGED_ITER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "memtable.h"
#include "basalt/slice.h"
#include "concat_iter.h"
#include "table.h"

namespace basalt {

class MergedIter {
 public:
  void AddMemTable(const MemTable* m);
  void AddTable(const sst::Table* t);
  // L1 AS ONE SOURCE, NOT |L1| SOURCES -- B3-D4. `run` must be ascending and
  // non-overlapping; ConcatIter asserts it. An empty run adds nothing, because
  // a source that is never Valid still costs a comparison on every step.
  void AddRun(std::vector<const sst::Table*> run);

  bool Valid() const { return current_ >= 0; }
  void SeekToFirst();
  void SeekToLast();
  // Positions at the first entry >= (user_key, tag) in the internal order.
  void Seek(Slice user_key, uint64_t tag);
  void Next();
  void Prev();

  Slice user_key() const;
  uint64_t tag() const;
  Slice value() const;

 private:
  enum class Direction { kForward, kReverse };

  struct Source {
    std::unique_ptr<MemTable::Iter> mem;
    // ONE ARM FOR EVERY SSTABLE CURSOR, whether it walks one file or a run.
    // See internal_iter.h: the alternative was nine three-way branches.
    std::unique_ptr<sst::InternalIter> table;
    bool Valid() const;
    void Next();
    void Prev();
    void SeekToFirst();
    void SeekToLast();
    void Seek(Slice user_key, uint64_t tag, const std::string& internal);
    Slice user_key() const;
    uint64_t tag() const;
    Slice value() const;
  };

  void PickSmallest();
  void PickLargest();
  // Compares source `i` against source `j` in the internal order.
  int CompareSources(std::size_t i, std::size_t j) const;
  void SeekAllTo(Slice user_key, uint64_t tag);

  std::vector<Source> sources_;
  int current_ = -1;
  Direction dir_ = Direction::kForward;
  // Scratch for the composed internal key a table seek needs. Held here so a
  // Slice into it cannot outlive the expression that built it.
  std::string seek_key_;
};

}  // namespace basalt

#endif  // BASALT_MERGED_ITER_H_
