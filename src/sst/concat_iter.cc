#include "concat_iter.h"

#include "basalt/check.h"
#include "internal_key.h"

namespace basalt {
namespace sst {

ConcatIter::ConcatIter(std::vector<const Table*> run) : run_(std::move(run)) {
  // ASCENDING AND NON-OVERLAPPING, ASSERTED AT CONSTRUCTION. An overlapping run
  // returns one file's version of a key and hides another's -- a wrong answer
  // with nothing structurally wrong anywhere to report it.
  for (std::size_t i = 1; i < run_.size(); ++i) {
    BASALT_CHECK(CompareInternalKey(Slice(run_[i]->check().smallest_key),
                                  Slice(run_[i - 1]->check().largest_key)) > 0);
  }
  file_ = run_.size();
}

void ConcatIter::Invalidate() {
  file_ = run_.size();
  it_.reset();
}

void ConcatIter::OpenFile(std::size_t i) {
  BASALT_CHECK(i < run_.size());
  file_ = i;
  it_.reset(new Table::Iter(run_[i]));
}

bool ConcatIter::Valid() const { return it_ != nullptr && it_->Valid(); }

void ConcatIter::SeekToFirst() {
  if (run_.empty()) { Invalidate(); return; }
  OpenFile(0);
  it_->SeekToFirst();
}

void ConcatIter::SeekToLast() {
  if (run_.empty()) { Invalidate(); return; }
  OpenFile(run_.size() - 1);
  it_->SeekToLast();
}

void ConcatIter::Seek(Slice target) {
  // BINARY SEARCH OVER FILE RANGES. The first file whose LARGEST key is at or
  // above the target is the only one that can hold it, because the run does not
  // overlap.
  //
  // PROGRESS QUANTITY: `hi - lo`, which strictly shrinks EVERY iteration. The
  // comparator decides which half is taken; the interval shrinks either way. So
  // this loop terminates even if CompareInternalKey is wrong -- which is the
  // property CF-3 asks for, and the reason the assertion is on the interval and
  // not on a key.
  std::size_t lo = 0;
  std::size_t hi = run_.size();
  while (lo < hi) {
    const std::size_t before = hi - lo;
    const std::size_t mid = lo + (hi - lo) / 2;
    if (CompareInternalKey(Slice(run_[mid]->check().largest_key), target) < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
    BASALT_CHECK(hi - lo < before);
  }
  if (lo >= run_.size()) { Invalidate(); return; }
  OpenFile(lo);
  it_->Seek(target);
  // The index said this file COULD hold the target. If it does not -- the
  // target falls in the gap before this file's smallest key -- the file's own
  // seek lands on its first entry, which is still the right answer for a
  // forward seek. If it lands past the end, fall through to the next file.
  if (!it_->Valid()) Next();
}

void ConcatIter::Next() {
  // PROGRESS QUANTITY: `file_`, strictly increasing. It advances whatever the
  // comparator says, so this loop terminates independently of it.
  if (it_ != nullptr && it_->Valid()) it_->Next();
  while (it_ == nullptr || !it_->Valid()) {
    if (file_ + 1 >= run_.size()) { Invalidate(); return; }
    const std::size_t before = file_;
    OpenFile(file_ + 1);
    BASALT_CHECK(file_ > before);
    it_->SeekToFirst();
  }
}

void ConcatIter::Prev() {
  // PROGRESS QUANTITY: `file_`, strictly decreasing.
  if (it_ != nullptr && it_->Valid()) it_->Prev();
  while (it_ == nullptr || !it_->Valid()) {
    if (file_ == 0 || file_ > run_.size()) { Invalidate(); return; }
    const std::size_t before = file_;
    OpenFile(file_ - 1);
    BASALT_CHECK(file_ < before);
    it_->SeekToLast();
  }
}

Slice ConcatIter::key() const {
  BASALT_CHECK(Valid());
  return it_->key();
}

Slice ConcatIter::value() const {
  BASALT_CHECK(Valid());
  return it_->value();
}

}  // namespace sst
}  // namespace basalt
