// ONE SURFACE FOR THE TWO THINGS THAT WALK INTERNAL KEYS.
//
// `Table::Iter` and `ConcatIter` already had byte-identical surfaces; this makes
// that a type rather than a coincidence, and the reason is a specific bug class
// rather than tidiness.
//
// `MergedIter::Source` dispatches NINE methods over its arms. Adding L1 as a
// third arm would have meant nine three-way branches, and A MERGE THAT FORGETS
// THE NEW ARM IN ONE OF NINE PLACES RETURNS A WRONG ANSWER WITH NO FAILING
// STRUCTURE ANYWHERE -- no checksum notices, no classifier refuses, the key is
// simply absent. So the arms stay at two: a memtable cursor, and one of these.
//
// The virtual call is paid on every step of the k-way merge and is not free.
// The measurement that would move it is B5's readrandom against source count;
// until then, the cost of a wrong answer here is not comparable to the cost of
// an indirect call.
#ifndef BASALT_SST_INTERNAL_ITER_H_
#define BASALT_SST_INTERNAL_ITER_H_

#include "basalt/slice.h"

namespace basalt {
namespace sst {

class InternalIter {
 public:
  virtual ~InternalIter() = default;
  virtual bool Valid() const = 0;
  virtual void SeekToFirst() = 0;
  virtual void SeekToLast() = 0;
  // Positions at the first entry >= `target` IN THE INTERNAL ORDER -- user key
  // ascending, tag DESCENDING. Not memcmp; see internal_key.h.
  virtual void Seek(Slice target) = 0;
  virtual void Next() = 0;
  virtual void Prev() = 0;
  virtual Slice key() const = 0;
  virtual Slice value() const = 0;
};

}  // namespace sst
}  // namespace basalt

#endif  // BASALT_SST_INTERNAL_ITER_H_
