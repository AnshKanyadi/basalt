// A CURSOR OVER A SORTED RUN OF NON-OVERLAPPING TABLES -- L1, as one source.
//
// B3-D4. L0 files overlap each other and must all be consulted; L1 is a single
// non-overlapping run, so at most one file can hold a given key. Handing L1 to
// the merge as `|L1|` separate sources would make the merge's `k` grow with the
// database; handing it as ONE source keeps `k` at `|L0| + 1`.
//
// That is not a micro-optimisation: `MergedIter::PickSmallest` is a LINEAR scan
// over sources, chosen at B2 knowing `k` was small and bounded. This is what
// keeps that choice honest once L1 has more than a handful of files.
//
// ---------------------------------------------------------------------------
// CF-3, AND THE AUDIT WAS DONE BEFORE THE CODE.
//
// Every loop here asserts the movement it terminates on, over a quantity it does
// NOT derive from the comparator -- because a termination argument that assumes
// the thing being mutated is not a termination argument (HARNESS-013).
//
//   Next / Prev  advance `file_`, AN INTEGER INDEX. It moves whatever the
//                comparator says.
//   Seek         shrinks `hi - lo` every iteration. THE COMPARATOR DECIDES THE
//                DIRECTION; THE INTERVAL SHRINKS WHICHEVER DIRECTION IS TAKEN.
//                Correctness depends on the comparator here; TERMINATION does
//                not, and that distinction is the whole point of the rule.
#ifndef RIFT_SST_CONCAT_ITER_H_
#define RIFT_SST_CONCAT_ITER_H_

#include <cstddef>
#include <memory>
#include <vector>

#include "slice.h"
#include "table.h"

namespace rift {
namespace sst {

class ConcatIter {
 public:
  // `run` must be ASCENDING by key range and non-overlapping. That is a
  // precondition on the caller, asserted at construction rather than assumed:
  // an overlapping run silently returns one file's version of a key and hides
  // another's, which is a wrong answer with no failing structure anywhere.
  explicit ConcatIter(std::vector<const Table*> run);

  bool Valid() const;
  void SeekToFirst();
  void SeekToLast();
  void Seek(Slice target);   // first entry >= target, in the internal order
  void Next();
  void Prev();
  Slice key() const;
  Slice value() const;

  std::size_t files() const { return run_.size(); }

 private:
  void OpenFile(std::size_t i);
  void Invalidate();

  std::vector<const Table*> run_;
  std::size_t file_ = 0;             // == run_.size() means invalid
  std::unique_ptr<Table::Iter> it_;
};

}  // namespace sst
}  // namespace rift

#endif  // RIFT_SST_CONCAT_ITER_H_
